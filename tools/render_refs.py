#!/usr/bin/env python3
"""
Freeze a rendering baseline, and check later builds against it.

Why a fingerprint and not screenshots
-------------------------------------
The obvious baseline is a folder of captures, but a screenshot of this game is ROM-derived
data and cannot be committed. What is stored instead is a coarse colour fingerprint: each
frame is reduced to an 8x6 grid of mean RGB. That is 144 numbers per stage, enough to catch a
texture binding to the wrong surface, a palette going wrong, geometry disappearing or the
whole frame shifting, and far too little to reconstruct anything.

Why it exists
-------------
docs/ROADMAP.md gates the enhancement work on having a baseline to regress against. It is also
what today's tile-selection fix needed: the only way to know it changed Depot and nothing else
was to compare ten stages by hand.

  tools/render_refs.py capture     write tools/refs/render.txt from the current build
  tools/render_refs.py check       recapture and compare, non-zero exit on drift

The capture is deterministic: a fixed stage list, a fixed input script, the synthetic clock,
and one frame chosen well after load. Two runs of the same binary produce the same numbers.
"""

import os, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "getv" / "build-mac" / "goldeneye"
REFS = ROOT / "tools" / "refs" / "render.txt"

# Solo-loadable stages, plus the multiplayer-only ones flagged so they get two players.
STAGES = [(9, "Bunker1", 0), (20, "Silo", 0), (22, "Statue", 0), (23, "Control", 0),
          (24, "Archives", 0), (25, "Train", 0), (26, "Frigate", 0), (27, "Bunker2", 0),
          (28, "Aztec", 0), (29, "Streets", 0), (30, "Depot", 0), (32, "Egypt", 0),
          (33, "Dam", 0), (34, "Facility", 0), (35, "Runway", 0), (36, "Surface", 0),
          (37, "Jungle", 0), (39, "Caverns", 0), (41, "Cradle", 0), (43, "Surface2", 0),
          (54, "Cuba", 0), (31, "Complex", 2), (38, "Temple", 2), (45, "Basement", 2),
          (46, "Stack", 2), (48, "Library", 2), (50, "Caves", 2)]

SCRIPT = "60:START:6,120:START:6,180:START:6"
SHOT_FRAME, EXIT_FRAME = 280, 300
COLS, ROWS = 8, 6
TOLERANCE = 6          # per-channel mean difference that counts as drift


def fingerprint(bmp: Path):
    d = bmp.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    bypp, H = bpp // 8, abs(h)
    stride = ((w * bypp + 3) // 4) * 4
    out = []
    for gy in range(ROWS):
        for gx in range(COLS):
            r = g = b = n = 0
            for y in range(gy * H // ROWS, (gy + 1) * H // ROWS, 4):
                base = off + (H - 1 - y) * stride
                for x in range(gx * w // COLS, (gx + 1) * w // COLS, 4):
                    i = base + x * bypp
                    b += d[i]; g += d[i + 1]; r += d[i + 2]; n += 1
            out += [r // n, g // n, b // n] if n else [0, 0, 0]
    return out


def capture_all():
    if not GAME.exists():
        sys.exit(f"no binary at {GAME}; build first")
    rows = {}
    with tempfile.TemporaryDirectory() as td:
        shot = Path(td) / "f.bmp"
        for sid, name, players in STAGES:
            env = dict(os.environ, GETV_STAGE=str(sid), GETV_INTROCAM="0",
                       GETV_SCRIPT=SCRIPT, GETV_EXIT_FRAME=str(EXIT_FRAME),
                       GETV_SHOTFRAME=str(SHOT_FRAME), GETV_SHOTPATH=str(shot),
                       GETV_NO_AUDIO="1", GETV_WINDOW="1")
            if players:
                env["GETV_MP"] = str(players)
            shot.unlink(missing_ok=True)
            subprocess.run([str(GAME)], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=180)
            if not shot.exists():
                print(f"  {name:10s} {sid:3d}  NO CAPTURE")
                continue
            rows[f"{sid}:{name}"] = fingerprint(shot)
            print(f"  {name:10s} {sid:3d}  ok")
    return rows


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    rows = capture_all()

    if mode == "capture":
        REFS.parent.mkdir(parents=True, exist_ok=True)
        with REFS.open("w") as fh:
            fh.write("# Rendering baseline: 8x6 grid of mean RGB per stage.\n")
            fh.write("# Written by tools/render_refs.py capture. Check with 'check'.\n")
            for k in sorted(rows, key=lambda s: int(s.split(":")[0])):
                fh.write(f"{k} {' '.join(map(str, rows[k]))}\n")
        print(f"\n  wrote {len(rows)} stages to {REFS.relative_to(ROOT)}")
        return 0

    if not REFS.exists():
        sys.exit(f"no baseline at {REFS}; run 'capture' first")
    want = {}
    for line in REFS.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        k, _, rest = line.partition(" ")
        want[k] = [int(v) for v in rest.split()]

    drift = 0
    for k, got in rows.items():
        if k not in want:
            print(f"  {k}: not in baseline"); drift += 1; continue
        worst = max(abs(a - b) for a, b in zip(got, want[k]))
        if worst > TOLERANCE:
            print(f"  {k}: DRIFT, worst channel differs by {worst}"); drift += 1
    missing = set(want) - set(rows)
    for k in sorted(missing):
        print(f"  {k}: in baseline but not captured"); drift += 1

    print(f"\n  {len(rows) - drift} of {len(want)} stages match within {TOLERANCE}")
    return 1 if drift else 0


if __name__ == "__main__":
    raise SystemExit(main())
