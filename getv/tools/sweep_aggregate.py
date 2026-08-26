#!/usr/bin/env python3
"""Aggregate N level_sweep.sh sample directories into one board.

See PORTING_PLAYBOOK.md 2.5. Outcomes on this port vary across launches of an identical
binary, so a single sample is not a measurement. This script never reports a best run:
for every metric it reports median / min / max over the samples, and it classifies each
level as stable or bimodal.

Bimodality is a finding, not noise. A level whose `submitted` or `drawn` swings by more
than 2x across identical launches depends on uninitialised memory or on heap layout.
That localises a real bug; do not tune around it.

Run via level_sweep_multi.sh, which supplies the sample directories.
"""
import argparse, glob, os, re, statistics, sys

RATIO_LIMIT = 2.0     # max/min above this on submitted or drawn => bimodal

# ---- Ground truth from docs/research/GE_GAME_FACTS.md 1 --------------------
# The denominator matters. "22 of 40 levels render" counts stage ids that can never load
# against the total, which flatters the number: a stage id is not a level. Four classes,
# and only the first two can ever be a bug:
#   SOLO      a real single-player mission (20 of them)
#   MP-ONLY   no solo setup file exists in the ROM; must be run with GETV_MP>=2, because
#             at 1 player prop.c asks for "Usetup<code>Z", which does not exist, and the
#             stage loads geometry with no setup: no props, no Bond spawn.
#   NO-DATA   geometry absent from the ROM (or, for CITADEL, geometry present but no
#             setup file anywhere). Cannot ever work, so not a failure.
#   NOT-LEVEL menus, the end credits, and aliases that silently resolve to Bunker 1.
STAGE_CLASS = {
    "9": ("SOLO", "Bunker 1"), "20": ("SOLO", "Silo"), "22": ("SOLO", "Statue Park"),
    "23": ("SOLO", "Control"), "24": ("SOLO", "Archives"), "25": ("SOLO", "Train"),
    "26": ("SOLO", "Frigate"), "27": ("SOLO", "Bunker 2"), "28": ("SOLO", "Aztec"),
    "29": ("SOLO", "Streets"), "30": ("SOLO", "Depot"), "32": ("SOLO", "Egyptian"),
    "33": ("SOLO", "Dam"), "34": ("SOLO", "Facility"), "35": ("SOLO", "Runway"),
    "36": ("SOLO", "Surface 1"), "37": ("SOLO", "Jungle"), "39": ("SOLO", "Water Caverns"),
    "41": ("SOLO", "Cradle"), "43": ("SOLO", "Surface 2"),
    "31": ("MP-ONLY", "Complex"), "38": ("MP-ONLY", "Temple"), "45": ("MP-ONLY", "Basement"),
    "46": ("MP-ONLY", "Stack"), "48": ("MP-ONLY", "Library"), "50": ("MP-ONLY", "Caves"),
    "40": ("NO-DATA", "geometry present, NO setup file anywhere in the ROM"),
    "42": ("NO-DATA", "cut, bg 0 bytes"), "44": ("NO-DATA", "cut, no bg manifest row"),
    "47": ("NO-DATA", "cut, no bg manifest row"), "49": ("NO-DATA", "cut, bg 0 bytes"),
    "51": ("NO-DATA", "cut, bg 0 bytes"), "52": ("NO-DATA", "cut, bg 0 bytes"),
    "53": ("NO-DATA", "cut, bg 0 bytes"), "55": ("NO-DATA", "cut, bg 0 bytes"),
    "56": ("NO-DATA", "cut, bg 0 bytes"), "57": ("NO-DATA", "sentinel, not a level"),
    "0":  ("NOT-LEVEL", "ALIAS -> Bunker 1 geometry, setup pointer is NULL"),
    "21": ("NOT-LEVEL", "unnamed alias of Bunker 1"),
    "54": ("NOT-LEVEL", "THE END CREDITS, not a mission"),
    "90": ("NOT-LEVEL", "front-end menu; falls back to Bunker 1 geometry"),
}

# Authored dark: low coverage on these is the art direction, not breakage. Sky RGB comes
# from fog_tables (bgfog.c:174-222). Only four levels use the flat-fill sky path in 1P.
DARK_BY_DESIGN = {
    "23": "sky RGB (0,0,0) -- flat PURE BLACK by design, Clouds=0",
    "39": "sky RGB (8,0,8) -- near-black, underground, Clouds=0",
    "37": "sky RGB (24,32,0) -- near-black olive, Clouds=0",
    "34": "sky RGB (16,32,16) -- flat dark green, Clouds=0",
    "22": "night level, 3,500 far-fog, sky (0,0,8)",
    "24": "sky RGB (0,0,0) backdrop + white cloud layer",
    "28": "sky RGB (0,0,0) backdrop + grey clouds",
    "27": "sky RGB (16,0,0) near-black red",
}

# bg file per stage (GE_GAME_FACTS 1). Used to ask whether the bimodal levels share
# geometry; a correlation there gives a much tighter target than "somewhere in the render
# path".
BG_FILE = {
    "9":"bg_sev","20":"bg_silo","22":"bg_stat","23":"bg_arec","24":"bg_arch","25":"bg_tra",
    "26":"bg_dest","27":"bg_sevb","28":"bg_azt","29":"bg_pete","30":"bg_depo","31":"bg_ref",
    "32":"bg_cryp","33":"bg_dam","34":"bg_ark","35":"bg_run","36":"bg_sevx","37":"bg_jun",
    "38":"bg_dish","39":"bg_cave","40":"bg_cat","41":"bg_crad","43":"bg_sevx","45":"bg_ame",
    "46":"bg_ame","48":"bg_ame","50":"bg_oat","54":"bg_len","0":"bg_sev","21":"bg_sev","90":"bg_sev",
}

# Levels that share one bg file. Identical output inside a group is expected and is not a
# finding: reporting it as "N levels have the same bug" counts one map three times.
SHARED_BG = {
    "bg_ame": ["45", "46", "48"],      # Basement, Stack, Library -- one map, three setups
    "bg_sevx": ["36", "43"],           # Surface 1 and Surface 2
    "bg_sev": ["9", "0", "21", "90"],  # Bunker 1, plus everything that falls back to it
}


# results.tsv column order, as written by level_sweep.sh.
COLS = ("id name stage_load frame_loop gfx_tasks tris_sub tris_drawn frames "
        "coverage uniq top1share outcome fault_pc fault_addr last_mark").split()

RE_REJECT = re.compile(r"\[lt\] f\d+ tri reject: clip=(\d+) cull=(\d+) cullboth=(\d+) drawn=(\d+)")
RE_CAM    = re.compile(r"\[getv\]\[rt\] f=\d+ .*? cam=(\d+)")
RE_EYE    = re.compile(r"eyeheight=(-?[\d.]+)")
RE_ROOMS  = re.compile(r"maxrooms=(\d+)")
# Counters armed elsewhere in the port that did not fire on the levels available at the
# time. A whole-game sweep is the cheapest way to learn whether they are live anywhere,
# so they are harvested here rather than needing their own run.
#   geTexSelCiNoLut -- a CI texture whose lutmode hit no case, so NO combiner was
#                      emitted and the PREVIOUS draw call's combiner stayed in force.
#                      GoldenEye is CI-heavy; if this prints it is a real render bug.
#   [getv][hitidx] -- propobjFindHit() is the only writer of mtxindex/dlnode and runs
#                      only for HIT_GUN/HIT_HAT, so any other body part passed on an
#                      uninitialised ModelNode* on the shooting path.
#   [getv][texidx] -- besttexture == -1 indexed g_Textures BEFORE the array.
RE_COUNTERS = {
    "geTexSelCiNoLut": re.compile(r"geTexSelCiNoLut"),
    "hitidx":          re.compile(r"\[getv\]\[hitidx\]"),
    "texidx":          re.compile(r"\[getv\]\[texidx\]"),
}
CAMNAME   = {0:"NONE",1:"INTRO",2:"FADESWIRL",3:"SWIRL",4:"FP",5:"DEATH_SP",6:"DEATH_MP",
             7:"POSEND",8:"FP_NOINPUT",9:"MP",10:"FADE_TITLE"}


def num(s):
    """'-' and empty mean missing, not zero. Conflating them averages a crashed level in
    as a legitimate 0 and drags the median to a number no launch ever produced."""
    if s is None:
        return None
    s = s.strip()
    if s in ("", "-"):
        return None
    try:
        return float(s)
    except ValueError:
        return None


LOAD_LIMIT = 100.0   # host load above this makes a hang result unusable

def load_trace(path):
    """timestamp -> 1-minute load average, sampled alongside the sweep.

    Host starvation, a wedged simulator and a real hang are indistinguishable in the log:
    all three emit a short log with no signal and no final line. At a load average of 722
    with 22 booted simulators, `uptime` itself does not return inside 120 s. Since nothing
    in the log separates them after the fact, the load is recorded while the sweep runs
    and joined back to each sample by timestamp. A row whose runs happened inside a
    starvation window is flagged as lower confidence rather than presented as clean."""
    pts = []
    if path and os.path.exists(path):
        for line in open(path):
            f = line.split()
            if len(f) == 2:
                try:
                    pts.append((int(f[0]), float(f[1])))
                except ValueError:
                    pass
    return sorted(pts)


def load_at(pts, ts):
    if not pts:
        return None
    best = min(pts, key=lambda p: abs(p[0] - ts))
    return best[1] if abs(best[0] - ts) <= 180 else None


def read_sample(d, loadpts=()):
    """One sample directory -> {level_id: row dict}. Extra columns that level_sweep.sh
    does not compute (clip/cull, true frame count, camera mode, eyeheight) are parsed
    out of the .log that sits beside each row, so the harness itself stays untouched."""
    out = {}
    tsv = os.path.join(d, "results.tsv")
    if not os.path.exists(tsv):
        return out
    with open(tsv) as f:
        lines = f.read().splitlines()
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) < len(COLS):
            continue
        r = dict(zip(COLS, parts))
        log = os.path.join(d, "%s-%s.log" % (r["id"], r["name"]))
        r.update(clip=None, cull=None, real_frames=None, cam=None, eye=None, rooms=None,
                 wedge=False, logsize=0, load=None, counters={}, springboard=False)
        if os.path.exists(log):
            r["logsize"] = os.path.getsize(log)
            r["load"] = load_at(loadpts, int(os.path.getmtime(log)))
            # A wedged simulator is a harness artifact, not a result. It produces a
            # ~1-4 KB log that stops partway through boot with no signal and no final
            # line, which the parser scores as SILENT-HANG, indistinguishable from a real
            # hang. On a control run, DAM sample 2 gave a 4,066-byte log ending at the
            # launch PID line while samples 1 and 3 gave 1.4 MB and 812 KB and both
            # rendered. Recovery is `simctl shutdown <UDID> && simctl boot <UDID>`, and
            # the run should be repeated, never recorded.
            #
            # Flagged here rather than silently dropped: a wedge counted as a SILENT-HANG
            # invents a bimodal level that does not exist, and inventing a
            # memory-corruption finding is worse than losing a sample.
            if r["outcome"] == "SILENT-HANG" and r["logsize"] < 8192 and r["frame_loop"] != "yes":
                r["wedge"] = True
            txt = open(log, errors="replace").read()
            rej = RE_REJECT.findall(txt)
            if rej:
                # Last window, matching level_sweep.sh's "take the last frame" rule:
                # frame 0 is often a clear-only frame. ge_lt is memset after each dump,
                # so these are per-window totals, not cumulative.
                r["clip"], r["cull"] = float(rej[-1][0]), float(rej[-1][1])
                r["real_frames"] = float(len(rej))   # lighttrace prints once per frame
            cams = [int(c) for c in RE_CAM.findall(txt)]
            if cams:
                # Highest mode reached, not the last: a level that reaches first person
                # and then falls back to a death cam still reached first person, which is
                # the question this column answers.
                r["cam"] = max(cams)
            eyes = RE_EYE.findall(txt)
            if eyes:
                r["eye"] = float(eyes[-1])
            # Springboard guard. `simctl io screenshot` captures the device, not the app.
            # If the app exits during the ~1-2 s the capture takes, the image is the tvOS
            # home screen, which scores 99.90-99.91% non-black on every level and would
            # award the best coverage in the table to a level that drew nothing (DEFAULT,
            # CONTROL, SHO and CRADLE all reported exactly 99.91). Any coverage in that
            # narrow band is discarded as unmeasurable rather than trusted.
            try:
                cov = float(r["coverage"])
                if 99.87 <= cov <= 99.95:
                    r["springboard"] = True
                    r["coverage"] = r["uniq"] = r["top1share"] = "-"
            except (TypeError, ValueError):
                pass
            r["counters"] = {k: len(rx.findall(txt)) for k, rx in RE_COUNTERS.items()
                             if rx.search(txt)}
            rm = RE_ROOMS.findall(txt)
            if rm:
                r["rooms"] = int(rm[-1])
        out[r["id"]] = r
    return out


def stat(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None, None, None, 0
    return statistics.median(vals), min(vals), max(vals), len(vals)


def fmt(m, lo, hi, n, prec=0):
    if m is None:
        return "-"
    f = ("%%.%df" % prec)
    if lo == hi:
        return f % m
    return "%s (%s-%s)" % (f % m, f % lo, f % hi)


def confidence(r):
    """Low confidence is not the same as a bad number: it means the host was busy enough
    that a hang or a short log cannot be trusted, so the row deserves a re-run before
    anything is built on it. Stated per row rather than as a blanket caveat."""
    bits = []
    if r["n"] < 3:
        bits.append("only %d/%d launches usable" % (r["n"], r["nlaunch"]))
    if r.get("springboard"):
        bits.append("%d screenshot(s) caught the springboard, coverage discarded" % r["springboard"])
    if r.get("starved"):
        bits.append("%d run(s) at load>%d (peak %.0f)" % (r["starved"], LOAD_LIMIT, r["maxload"]))
    return "; ".join(bits) if bits else "ok"


def ratio(lo, hi):
    if lo is None:
        return None
    if lo == 0:
        return float("inf") if hi > 0 else 1.0
    return hi / lo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("--n", type=int, default=3)
    ap.add_argument("--ss", default="?")
    ap.add_argument("--skipintro", default="?")
    ap.add_argument("--lighttrace", default="?")
    ap.add_argument("--sim", default="?")
    ap.add_argument("--slot", default="?")
    ap.add_argument("--mp", default="off")
    ap.add_argument("--loadlog", default=None)
    a = ap.parse_args()

    dirs = sorted(glob.glob(os.path.join(a.base, "s*")),
                  key=lambda p: int(re.sub(r"\D", "", os.path.basename(p)) or 0))
    loadpts = load_trace(a.loadlog)
    samples = [read_sample(d, loadpts) for d in dirs]
    samples = [s for s in samples if s]
    if not samples:
        sys.exit("no sample directories with results.tsv under %s" % a.base)

    ids = []
    for s in samples:
        for k in s:
            if k not in ids:
                ids.append(k)
    ids.sort(key=int)

    rows = []
    for lid in ids:
        allgot = [s[lid] for s in samples if lid in s]
        name = allgot[0]["name"]
        wedged = [g for g in allgot if g["wedge"]]
        got = [g for g in allgot if not g["wedge"]]
        # If every sample wedged there is nothing to report and nothing to exclude, so
        # keep them and the level shows up as needing a re-run rather than vanishing.
        if not got:
            got = allgot
            wedged = []
        outcomes = [g["outcome"] for g in got]
        uniq_out = sorted(set(outcomes), key=outcomes.count, reverse=True)
        # Outcome is reported as the majority plus every dissenting value. A level that
        # crashed on one launch of three is not a rendering level with a footnote.
        out_txt = uniq_out[0] if len(uniq_out) == 1 else \
            "%s (%s)" % (uniq_out[0], ", ".join("%s x%d" % (o, outcomes.count(o)) for o in uniq_out[1:]))

        met = {}
        for key, src, prec in (("frames", "real_frames", 0), ("submitted", "tris_sub", 0),
                               ("drawn", "tris_drawn", 0), ("clip", "clip", 0),
                               ("cull", "cull", 0), ("coverage", "coverage", 2),
                               ("distinct", "uniq", 0), ("top1share", "top1share", 1)):
            vals = [num(g[src]) if isinstance(g[src], str) else g[src] for g in got]
            met[key] = stat(vals) + (prec,)
        if met["frames"][0] is None:   # no lighttrace -> fall back to the harness's
            met["frames"] = stat([num(g["frames"]) * 60 if num(g["frames"]) else None
                                  for g in got]) + (0,)

        sb = len([g for g in allgot if g.get("springboard")])
        ctr = {}
        for g in allgot:
            for k, v in (g.get("counters") or {}).items():
                ctr[k] = ctr.get(k, 0) + v
        loads = [g["load"] for g in allgot if g.get("load") is not None]
        starved = [l for l in loads if l > LOAD_LIMIT]
        cams = [g["cam"] for g in got if g["cam"] is not None]
        cam_max = max(cams) if cams else None
        eyes = [g["eye"] for g in got if g["eye"] is not None]
        rms = [g["rooms"] for g in got if g["rooms"] is not None]
        rooms = max(rms) if rms else None

        flags = []
        for key in ("submitted", "drawn"):
            m, lo, hi, n, _ = met[key]
            if n >= 2:
                r = ratio(lo, hi)
                if r is not None and r > RATIO_LIMIT:
                    flags.append("%s %s" % (key, "inf" if r == float("inf") else "%.1fx" % r))
        if len(set(outcomes)) > 1:
            flags.insert(0, "outcome")

        cls, note = STAGE_CLASS.get(lid, ("SOLO", ""))
        fp = cam_max is not None and cam_max >= 4        # CAMERAMODE_FP / FP_NOINPUT
        paints = out_txt.startswith("RENDERS") and not out_txt.startswith("RENDERS-BLACK") \
                 and not out_txt.startswith("RENDERS-FLAT")
        if cls in ("NO-DATA",):
            bucket = "DATA-ABSENT"
        elif cls == "NOT-LEVEL":
            bucket = "NOT-A-LEVEL"
        elif fp and paints:
            bucket = "PLAYABLE"
        else:
            bucket = "BROKEN"
        rows.append(dict(id=lid, name=name, outcome=out_txt, met=met, flags=flags,
                         cam=cam_max, eyes=eyes, n=len(got), cls=cls, note=note,
                         bucket=bucket, fp=fp, paints=paints, rooms=rooms,
                         wedged=len(wedged), nlaunch=len(allgot),
                         starved=len(starved), maxload=(max(loads) if loads else None),
                         counters=ctr, springboard=sb,
                         bg=BG_FILE.get(lid),
                         dark=DARK_BY_DESIGN.get(lid)))

    tsv = os.path.join(a.base, "board.tsv")
    with open(tsv, "w") as f:
        f.write("id\tname\tclass\tbucket\tn\toutcome\tcam\tframes\tsubmitted\tdrawn\tclip\tcull\t"
                "coverage\tdistinct\ttop1share\tstability\n")
        for r in rows:
            f.write("\t".join([
                r["id"], r["name"], r["cls"], r["bucket"], str(r["n"]), r["outcome"],
                (CAMNAME.get(r["cam"], str(r["cam"])) if r["cam"] is not None else "-"),
                fmt(*r["met"]["frames"]), fmt(*r["met"]["submitted"]), fmt(*r["met"]["drawn"]),
                fmt(*r["met"]["clip"]), fmt(*r["met"]["cull"]), fmt(*r["met"]["coverage"]),
                fmt(*r["met"]["distinct"]), fmt(*r["met"]["top1share"]),
                ("BIMODAL: " + ", ".join(r["flags"])) if r["flags"] else "stable"]) + "\n")

    bim = [r for r in rows if r["flags"]]
    real = [r for r in rows if r["bucket"] in ("PLAYABLE", "BROKEN")]
    playable = [r for r in rows if r["bucket"] == "PLAYABLE"]
    broken = [r for r in rows if r["bucket"] == "BROKEN"]
    absent = [r for r in rows if r["bucket"] == "DATA-ABSENT"]
    notlvl = [r for r in rows if r["bucket"] == "NOT-A-LEVEL"]

    md = os.path.join(a.base, "board.md")
    with open(md, "w") as f:
        w = f.write
        w("# Level board -- n=%d samples per level\n\n" % a.n)
        w("Knobs held CONSTANT for every sample. Never diff a number here against a run\n"
          "with different knobs -- `GETV_SUPERSAMPLE` alone changes framebuffer size ->\n"
          "heap layout -> OUTCOMES (PORTING_PLAYBOOK 2.1).\n\n")
        w("- `GETV_SUPERSAMPLE=%s`\n"
          "- `GETV_GUN_SKIPINTRO=%s` -- measured in FIRST-PERSON GAMEPLAY, not the intro camera\n"
          "- `GETV_LIGHTTRACE=%s` (clip/cull counters) `GETV_ROOMTRACE=%s` (camera mode, eyeheight)\n"
          "- `GETV_MP=%s`\n- slot `%s`, sim `%s`\n\n"
          % (a.ss, a.skipintro, a.lighttrace, a.lighttrace, a.mp, a.slot, a.sim))
        w("Every cell is `median (min-max)` over %d launches; a bare number means all %d\n"
          "samples agreed exactly. `-` means NOT MEASURABLE, never zero -- coverage is gated\n"
          "on the app still being alive, because `simctl` screenshots the DEVICE and a\n"
          "crashed level captures the tvOS springboard at 99.90-99.91%% non-black.\n\n"
          % (a.n, a.n))
        w("`coverage` alone lies (a flat fill scored 68% once). Read it with `distinct` and\n"
          "`top1share`: `top1share` is the fraction of LIT pixels that are one single colour,\n"
          "so a flat sky drives it toward 100 and real geometry drives it down.\n\n")

        w("## Headline\n\n")
        w("| bucket | count | meaning |\n|---|---|---|\n")
        w("| **PLAYABLE** | **%d / %d** | reaches first person AND paints the level |\n"
          % (len(playable), len(real)))
        w("| **BROKEN** | **%d / %d** | should work, does not |\n" % (len(broken), len(real)))
        w("| DATA-ABSENT | %d | cannot ever load; excluded from the denominator |\n" % len(absent))
        w("| not a level | %d | menus, credits, aliases; excluded |\n\n" % len(notlvl))
        w("The denominator is **%d**, not the number of stage ids swept. %d ids can never\n"
          "load and %d are not levels at all; counting them in would flatter the fraction.\n\n"
          % (len(real), len(absent), len(notlvl)))

        for title, group, blurb in (
            ("PLAYABLE", playable, "First person reached and the level paints."),
            ("BROKEN", broken, "Real stages with real data that do not reach a painted first-person frame."),
            ("DATA-ABSENT", absent, "No geometry, or (CITADEL) geometry with no setup file anywhere in the ROM. NOT failures."),
            ("Not a level", notlvl, "Menus, end credits and aliases that silently resolve to Bunker 1's geometry."),
        ):
            if not group:
                continue
            w("## %s -- %d\n\n%s\n\n" % (title, len(group), blurb))
            w("| id | level | class | n | outcome | cam | frames | submitted | drawn | clip | cull | coverage | distinct | top1share | stability | confidence |\n")
            w("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
            for r in group:
                w("| %s | %s | %s | %d | **%s** | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" % (
                    r["id"], r["name"], r["cls"], r["n"], r["outcome"],
                    (CAMNAME.get(r["cam"], str(r["cam"])) if r["cam"] is not None else "-"),
                    fmt(*r["met"]["frames"]), fmt(*r["met"]["submitted"]), fmt(*r["met"]["drawn"]),
                    fmt(*r["met"]["clip"]), fmt(*r["met"]["cull"]), fmt(*r["met"]["coverage"]),
                    fmt(*r["met"]["distinct"]), fmt(*r["met"]["top1share"]),
                    ("**BIMODAL** " + ", ".join(r["flags"])) if r["flags"] else "stable",
                    confidence(r)))
            w("\n")

        w("## Bimodal levels -- %d of %d measured\n\n" % (len(bim), len(rows)))
        w("These swung by more than %.0fx on `submitted` or `drawn`, or landed on DIFFERENT\n"
          "OUTCOMES, across launches of the SAME binary with the SAME knobs on the SAME\n"
          "device. That is uninitialised memory or heap-layout dependence. It is a BUG WITH\n"
          "A LOCATION, not measurement noise, and it is the most actionable thing in this\n"
          "file: a candidate mechanism already exists in `othermodemicrocode.c:400`, which\n"
          "leaks four uninitialised locals into `gDPLoadBlock` in the render path.\n\n"
          % RATIO_LIMIT)
        if bim:
            w("| level | id | bg file | rooms | what swung | submitted | drawn | outcomes |\n")
            w("|---|---|---|---|---|---|---|---|\n")
            for r in bim:
                w("| **%s** | %s | %s | %s | %s | %s | %s | %s |\n" % (
                    r["name"], r["id"], r["bg"] or "-", r["rooms"] if r["rooms"] else "-",
                    ", ".join(r["flags"]), fmt(*r["met"]["submitted"]),
                    fmt(*r["met"]["drawn"]), r["outcome"]))
            w("\n### Do the bimodal levels share anything?\n\n")
            bgs = {}
            for r in bim:
                bgs.setdefault(r["bg"], []).append(r["name"])
            allbgs = {}
            for r in real:
                allbgs.setdefault(r["bg"], []).append(r["name"])
            w("- bg files involved: %s\n" % ", ".join(
                "`%s` (%s)" % (k, ", ".join(v)) for k, v in sorted(bgs.items(), key=lambda x: str(x[0]))))
            w("- of the %d stages measured in PLAYABLE+BROKEN, %d are bimodal, spread over %d\n"
              "  distinct bg files out of %d -- %s\n" % (
                  len(real), len([r for r in bim if r in real]), len(bgs), len(allbgs),
                  "NOT concentrated in one map, so the mechanism is shared code, not one asset"
                  if len(bgs) > 1 else "concentrated in ONE map -- look at that asset first"))
            rr = [r["rooms"] for r in bim if r["rooms"]]
            sr = [r["rooms"] for r in real if r["rooms"] and not r["flags"]]
            if rr and sr:
                w("- room count, bimodal: median %d (%d-%d) | stable: median %d (%d-%d)\n" % (
                    statistics.median(rr), min(rr), max(rr),
                    statistics.median(sr), min(sr), max(sr)))
        else:
            w("None. Every measured level reproduced within %.0fx across all %d launches.\n" % (RATIO_LIMIT, a.n))

        wedgy = [r for r in rows if r.get("wedged")]
        if wedgy:
            w("\n## Discarded samples -- wedged simulator, NOT results\n\n")
            w("A wedged simulator stops logging partway through boot with no signal, which the\n"
              "parser cannot tell from a real hang. These samples were EXCLUDED from every\n"
              "statistic above; the medians and spreads come from the surviving launches.\n"
              "Counting them would have invented bimodal levels that do not exist.\n\n")
            for r in wedgy:
                w("- %s (%s): %d of %d launches wedged; %d sample(s) used\n"
                  % (r["name"], r["id"], r["wedged"], r["nlaunch"], r["n"]))

        hot = [r for r in rows if r.get("counters")]
        w("\n## Armed counters -- did any fire?\n\n")
        if hot:
            w("These fired. Each names a real defect, not a warning:\n\n")
            for r in hot:
                w("- **%s** (%s): %s\n" % (r["name"], r["id"],
                  ", ".join("`%s` x%d" % (k, v) for k, v in sorted(r["counters"].items()))))
        else:
            w("None fired on any level in this sweep. That is a MEASURED NEGATIVE for\n"
              "`geTexSelCiNoLut`, `[getv][hitidx]` and `[getv][texidx]` across every stage\n"
              "swept -- not proof they are unreachable, but the CI-no-LUT path in\n"
              "particular was suspected game-wide and did not fire once.\n"
              "The two shooting-path counters need a run that actually SHOOTS; this sweep\n"
              "does not pull the trigger (`GETV_GUN_AUTOFIRE` was off), so read their silence\n"
              "as 'not exercised', not as 'not broken'.\n")

        w("\n## Dark by design -- do NOT read low coverage as breakage\n\n")
        w("Sky RGB comes from `fog_tables` (`bgfog.c:174-222`). Only four levels use the\n"
          "flat-fill sky path in 1P at all.\n\n")
        for r in rows:
            if r["dark"]:
                w("- **%s** (%s): %s -- measured coverage %s, distinct %s, top1share %s\n" % (
                    r["name"], r["id"], r["dark"], fmt(*r["met"]["coverage"]),
                    fmt(*r["met"]["distinct"]), fmt(*r["met"]["top1share"])))

        w("\n## Shared geometry -- identical output inside a group is EXPECTED\n\n")
        for bg, ids in sorted(SHARED_BG.items()):
            have = [r for r in rows if r["id"] in ids]
            if len(have) > 1:
                w("- `%s`: %s\n" % (bg, " | ".join(
                    "%s sub=%s distinct=%s" % (r["name"], fmt(*r["met"]["submitted"]),
                                               fmt(*r["met"]["distinct"])) for r in have)))

        w("\n## First person\n\n")
        w("- reached CAMERAMODE_FP / FP_NOINPUT: **%d / %d** measured stages\n"
          % (len([r for r in rows if r["fp"]]), len(rows)))
        w("- of the %d real stages: **%d reached first person**, **%d of those also paint**\n"
          % (len(real), len([r for r in real if r["fp"]]), len(playable)))
        nofp = [r for r in real if not r["fp"]]
        if nofp:
            w("- real stages that never reached first person: %s\n"
              % ", ".join("%s (%s)" % (r["name"], r["outcome"].split()[0]) for r in nofp))
        w("\n### eyeheight -- the deterministic scalar. Prefer it to ANY frame statistic.\n\n")
        w("A value one code path computes is evidence; a frame aggregate is a lottery ticket\n"
          "until repeated. `167.314` on Dam means the room-vertex fix is live; `1569.814`\n"
          "means it is not.\n\n")
        for r in rows:
            if r["eyes"]:
                vs = sorted(set("%.3f" % e for e in r["eyes"]))
                w("- %s: %s%s\n" % (r["name"], ", ".join(vs),
                                     "  <- varies across launches" if len(vs) > 1 else ""))

    print(open(md).read())
    print("board.tsv -> %s" % tsv)


if __name__ == "__main__":
    main()
