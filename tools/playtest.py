#!/usr/bin/env python3
"""
Drive a level and report what happened, as a pass or fail.

Why
---
Everything else in this repository verifies that the game renders. The stage census proves
levels load, and render_refs proves frames do not change unexpectedly, but neither can tell
you Bond moved, or that an objective advanced. That is why the README says a full playthrough
is unverified.

This runs a stage with scripted input, reads the GETV_STATE lines, and answers questions a
script can act on:

  did the player reach gameplay at all, or stay on the briefing camera
  did the player move once there
  how many objectives the mission has, and did any of them change
  did the mission complete

None of that is a playthrough. It is the smallest honest step toward one: a run that fails is
now a run that says why.

  tools/playtest.py 33                 drive Dam with the default input
  tools/playtest.py 33 --frames 1200   longer run
  tools/playtest.py --all              every solo-loadable stage

Exit is non-zero if any stage never reached gameplay or never moved, which are the two
outcomes that mean the run told us nothing.
"""

import argparse, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "getv" / "build-mac" / "goldeneye"

SOLO = [(9, "Bunker1"), (20, "Silo"), (22, "Statue"), (23, "Control"), (24, "Archives"),
        (25, "Train"), (26, "Frigate"), (27, "Bunker2"), (28, "Aztec"), (29, "Streets"),
        (30, "Depot"), (32, "Egypt"), (33, "Dam"), (34, "Facility"), (35, "Runway"),
        (36, "Surface"), (37, "Jungle"), (39, "Caverns"), (41, "Cradle"), (43, "Surface2"),
        (54, "Cuba")]

LINE = re.compile(r"f=(\d+) players=(\d+)(?: pos=([-\d.]+),([-\d.]+),([-\d.]+))?"
                  r" objectives=(-?\d+) status=(\S+) complete=(\d)")


def run(stage, frames, forward):
    """One bounded run. Presses START to clear the briefing, then holds forward."""
    script = "60:START:6,120:START:6,180:START:6"
    if forward:
        # hold the stick forward in bursts once gameplay has started
        # SY=<n> is the N64 stick, positive is forward as the game reads it. Entries
        # overlap by design, so consecutive bursts hold the stick continuously.
        script += "," + ",".join(f"{f}:SY=70:60" for f in range(240, frames - 60, 60))
    env = dict(os.environ, GETV_STAGE=str(stage), GETV_INTROCAM="0", GETV_SCRIPT=script,
               GETV_EXIT_FRAME=str(frames), GETV_STATE="30", GETV_NO_AUDIO="1",
               GETV_WINDOW="1")
    try:
        p = subprocess.run([str(GAME)], env=env, capture_output=True, text=True,
                           timeout=max(120, frames // 2))
    except subprocess.TimeoutExpired:
        return None, "timed out"
    samples = [LINE.search(l) for l in p.stdout.splitlines() if "[getv][state]" in l]
    samples = [m for m in samples if m]
    if not samples:
        return None, "no state lines (did the build predate GETV_STATE?)"
    return samples, None


def summarise(samples):
    pos = [(float(m.group(3)), float(m.group(4)), float(m.group(5)))
           for m in samples if m.group(3)]
    moved = 0.0
    for a, b in zip(pos, pos[1:]):
        moved += max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
    objectives = int(samples[-1].group(6))
    statuses = {m.group(7) for m in samples}
    return {
        "frames": int(samples[-1].group(1)),
        "players": int(samples[-1].group(2)),
        "reached_gameplay": len(pos) > 0,
        "distance": moved,
        "objectives": objectives,
        "status_changed": len(statuses) > 1,
        "final_status": samples[-1].group(7),
        "complete": samples[-1].group(8) == "1",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", nargs="?", type=int)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--no-forward", action="store_true")
    a = ap.parse_args()
    if not GAME.exists():
        sys.exit(f"no binary at {GAME}; build first")
    todo = SOLO if a.all else [(a.stage, str(a.stage))] if a.stage else [(33, "Dam")]

    bad = 0
    print(f"  {'stage':<12}{'gameplay':>9}{'moved':>10}{'objs':>6}{'changed':>9}{'done':>6}")
    for sid, name in todo:
        samples, err = run(sid, a.frames, not a.no_forward)
        if err:
            print(f"  {name:<12}{err}")
            bad += 1
            continue
        s = summarise(samples)
        ok = s["reached_gameplay"] and s["distance"] > 1.0
        if not ok:
            bad += 1
        print(f"  {name:<12}{'yes' if s['reached_gameplay'] else 'NO':>9}"
              f"{s['distance']:>10.0f}{s['objectives']:>6}"
              f"{'yes' if s['status_changed'] else 'no':>9}"
              f"{'yes' if s['complete'] else 'no':>6}")
    print()
    print(f"  {len(todo) - bad} of {len(todo)} reached gameplay and moved")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
