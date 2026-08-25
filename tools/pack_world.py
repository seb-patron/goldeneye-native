#!/usr/bin/env python3
"""Pack extracted level knowledge into a compact binary the runtime can load.

WHY A BINARY

Everything the bots and the learning agents need about a level now exists -- waypoints, rooms,
routes to objectives, guard positions, threat exposure -- and all of it is JSON on disk that
nothing at runtime can read. Parsing JSON inside the port would mean shipping a parser, doing
allocation on a frame budget, and carrying files that are mostly whitespace.

This is a flat, fixed-width format instead: read the header, seek, index. No parsing, no
allocation, no ambiguity about field widths. The port maps it once at level load and answers
questions from it.

WHY IT ROUND-TRIPS

The reader below is not a convenience -- it is the test. A packer without one is a format that
is correct until somebody changes a field, and the failure mode is silent misalignment: every
offset after the change reads the neighbouring field and the numbers still look plausible. Every
pack is unpacked and compared before it is written.

Format (little-endian throughout, matching every platform the port targets):

    magic   'GEWD'          4
    version u32             4
    level   char[16]       16   name, NUL padded
    counts  u32 * 4        16   waypoints, guards, objectives, steps
    ---- waypoints ----     16 each   id u16, room u16, x/y/z f32... (see WP_FMT)
    ---- guards ----        16 each
    ---- objectives ----    24 each
    ---- steps ----         24 each

Usage:
    python3 tools/pack_world.py --out build/world
"""

import argparse
import glob
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"GEWD"
VERSION = 1

HDR_FMT = "<4sI16s4I"                 # magic, version, level, counts
WP_FMT  = "<HHfff"                    # id, room, x, y, z
GD_FMT  = "<HHfff"                    # chrnum, room, x, y, z
OB_FMT  = "<HHHHIfff"                 # index, difficulty, target_count, step_count,
                                      # step_first, tx, ty, tz
ST_FMT  = "<HHffffH2x"                # from_node, to_node, dist, heading, turn, _pad, threats


def pack_level(level, levels_dir):
    kp = os.path.join(levels_dir, level + ".json")
    tp = os.path.join(levels_dir, level + ".tactics.json")
    rp = os.path.join(levels_dir, level + ".routes.json")
    mp = os.path.join(levels_dir, level + ".rooms.json")
    if not (os.path.exists(kp) and os.path.exists(rp)):
        return None

    know = json.load(open(kp, encoding="utf-8"))
    routes = json.load(open(rp, encoding="utf-8"))
    tact = json.load(open(tp, encoding="utf-8")) if os.path.exists(tp) else {"objectives": []}
    rooms = json.load(open(mp, encoding="utf-8")) if os.path.exists(mp) else {}
    wproom = rooms.get("waypoint_room", {})
    proproom = rooms.get("prop_room", {})

    waypoints = []
    for w in know.get("waypoints", []):
        if not w.get("pos"):
            continue
        r = wproom.get(str(w["index"]))
        waypoints.append((w["index"] & 0xFFFF, (r if r is not None else 0xFFFF) & 0xFFFF,
                          w["pos"][0], w["pos"][1], w["pos"][2]))

    guards = []
    for p in know.get("props", []):
        if p.get("type") != "Guard" or not p.get("pos"):
            continue
        r = proproom.get(str(p["propdef"]))
        guards.append(((p.get("obj") or 0) & 0xFFFF, (r if r is not None else 0xFFFF) & 0xFFFF,
                       p["pos"][0], p["pos"][1], p["pos"][2]))

    # Objectives carry where their first route step lives, so a caller can walk a route without
    # searching. Objectives with no route point at zero steps rather than being omitted: a bot
    # still has to know the objective exists and that it cannot be walked to.
    objectives, steps = [], []
    by_index = {o.get("objective"): o for o in tact.get("objectives", [])}
    for r in routes.get("routes", []):
        idx = r.get("objective")
        o = by_index.get(idx, {})
        first = len(steps)
        tx = ty = tz = 0.0
        count = 0
        for leg in r.get("legs", []) or []:
            if leg.get("pos"):
                tx, ty, tz = leg["pos"]
            for s in leg.get("steps", []) or []:
                steps.append((s["from"] & 0xFFFF, s["to"] & 0xFFFF,
                              float(s["distance"]), float(s["heading"]),
                              float(s.get("turn") or 0.0), 0.0, len(s.get("threats", []))))
                count += 1
        objectives.append((idx & 0xFFFF, (o.get("min_difficulty") or 0) & 0xFFFF,
                           len(r.get("legs") or []) & 0xFFFF, count & 0xFFFF,
                           first, tx, ty, tz))

    blob = struct.pack(HDR_FMT, MAGIC, VERSION, level.encode()[:16],
                       len(waypoints), len(guards), len(objectives), len(steps))
    for w in waypoints:
        blob += struct.pack(WP_FMT, *w)
    for g in guards:
        blob += struct.pack(GD_FMT, *g)
    for o in objectives:
        blob += struct.pack(OB_FMT, *o)
    for s in steps:
        blob += struct.pack(ST_FMT, *s)
    return blob, (waypoints, guards, objectives, steps)


def unpack_level(blob):
    """The reader that makes the packer testable. Mirrors what the C loader must do."""
    hs = struct.calcsize(HDR_FMT)
    magic, version, name, nw, ng, no, ns = struct.unpack(HDR_FMT, blob[:hs])
    if magic != MAGIC:
        raise ValueError("bad magic %r" % magic)
    off = hs
    out = []
    for fmt, n in ((WP_FMT, nw), (GD_FMT, ng), (OB_FMT, no), (ST_FMT, ns)):
        sz = struct.calcsize(fmt)
        items = [struct.unpack(fmt, blob[off + i * sz: off + (i + 1) * sz]) for i in range(n)]
        off += sz * n
        out.append(items)
    if off != len(blob):
        raise ValueError("trailing bytes: consumed %d of %d" % (off, len(blob)))
    return name.rstrip(b"\0").decode(), version, out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--levels", default=os.path.join("build", "levels"))
    ap.add_argument("--out", default=os.path.join("build", "world"))
    args = ap.parse_args()
    levels_dir = args.levels if os.path.isabs(args.levels) else os.path.join(ROOT, args.levels)
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)

    levels = sorted({os.path.basename(p)[:-len(".routes.json")]
                     for p in glob.glob(os.path.join(levels_dir, "*.routes.json"))})
    total = errors = 0
    for level in levels:
        packed = pack_level(level, levels_dir)
        if packed is None:
            continue
        blob,orig = packed
        name, version, back = unpack_level(blob)

        # Round-trip: what came back must equal what went in, field for field. A format that
        # only writes is a format that is wrong the first time a field width changes.
        ok = True
        for a, b in zip(orig, back):
            if len(a) != len(b):
                ok = False
                break
            for ra, rb in zip(a, b):
                if any(abs(x - y) > 1e-3 if isinstance(x, float) else x != y
                       for x, y in zip(ra, rb[:len(ra)])):
                    ok = False
                    break
        if name != level[:16] or version != VERSION:
            ok = False
        if not ok:
            print("  ERROR %s: round-trip mismatch" % level)
            errors += 1
            continue

        with open(os.path.join(out_dir, level + ".gew"), "wb") as fh:
            fh.write(blob)
        total += len(blob)
        print("  %-10s %5d waypoints %4d guards %2d objectives %5d steps  %6d bytes"
              % (level, len(orig[0]), len(orig[1]), len(orig[2]), len(orig[3]), len(blob)))

    # Stage id -> level name, so the runtime can load its own knowledge instead of being told
    # which level it is in. bossGetStageNum() reports the stage; nothing mapped it to the names
    # the extractor uses, which is why ge_bot_route.c originally demanded an env var and wrongly
    # claimed the port could not know.
    idx_path = os.path.join(levels_dir, "index.json")
    if os.path.exists(idx_path):
        idx = json.load(open(idx_path, encoding="utf-8"))
        rows = sorted({(l["stage_id"], l["level"]) for l in idx.get("levels", [])
                       if not l.get("arena") and l.get("stage_id")})
        hdr = ["/* Generated by tools/pack_world.py -- do not edit. */",
               "#ifndef GE_WORLD_LEVELS_H", "#define GE_WORLD_LEVELS_H", "",
               "/* bossGetStageNum() -> the level name the extraction uses. */",
               "typedef struct { int stage; const char *level; } GeWorldStageName;", "",
               "static const GeWorldStageName ge_world_stage_names[] = {"]
        for stage, level in rows:
            hdr.append('    { %d, "%s" },' % (stage, level))
        hdr += ["};",
                "#define GE_WORLD_STAGE_COUNT %d" % len(rows), "",
                "#endif /* GE_WORLD_LEVELS_H */"]
        dst = os.path.join(ROOT, "getv", "port", "src", "ge_world_levels.h")
        if os.path.isdir(os.path.dirname(dst)):
            with open(dst, "w", encoding="utf-8") as fh:
                fh.write("\n".join(hdr) + "\n")
            print("  wrote %s (%d stages)" % (os.path.relpath(dst, ROOT), len(rows)))

    print("\n%d levels, %d bytes total -> %s" % (len(levels) - errors, total, out_dir))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
