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
  did the SCRIPTED INPUT change where the player ended up
  how many objectives the mission has, and did any of them change
  did the mission complete

None of that is a playthrough. It is the smallest honest step toward one: a run that fails is
now a run that says why.

  tools/playtest.py 33                 drive Dam with the default input
  tools/playtest.py 33 --frames 1200   longer run
  tools/playtest.py --all              every solo-loadable stage

Every stage runs TWICE, once with the scripted stick and once without, and the verdict is
whether the two disagree. That doubles the wall clock and it is not optional, because the
single-run version could not fail.

It used to measure total distance travelled and pass on anything above 1.0. Measured on four
stages, 86 to 97 percent of that distance was ONE step: the cutscene handing the player to
gameplay, which teleports him thousands of units and happens whether or not a controller is
attached. DAM reported 17,232 units with the stick held forward and 17,232 with no input at
all, to the unit, and passed both times. The number was real and it was not measuring input.

A control run cannot be fooled that way. Teleports, spawn settling and scripted intro walks all
happen identically in both runs and cancel; only what the stick did survives the subtraction.

Exit is non-zero if any stage never reached gameplay, or if the scripted stick made no
difference to where the player ended up.
"""

import argparse, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "getv" / "build-mac" / "goldeneye"

# Stages where the stick is EXPECTED to change nothing, with the reason. Kept as a named set
# rather than a tolerance on the number, because "this stage has no player to steer" and "input
# is broken on this stage" are different answers and must not share a threshold.
#
# Cuba is the end credits, not a mission. It reports no objectives, so objectiveIsAllComplete()
# is trivially true and the tool prints complete=yes over an empty set. Measured: spawn 56,
# walked 0, input 0, against 22 to 5,368 everywhere else.
NO_INPUT_EXPECTED = {54: "end credits, no player control"}

SOLO = [(9, "Bunker1"), (20, "Silo"), (22, "Statue"), (23, "Control"), (24, "Archives"),
        (25, "Train"), (26, "Frigate"), (27, "Bunker2"), (28, "Aztec"), (29, "Streets"),
        (30, "Depot"), (32, "Egypt"), (33, "Dam"), (34, "Facility"), (35, "Runway"),
        (36, "Surface"), (37, "Jungle"), (39, "Caverns"), (41, "Cradle"), (43, "Surface2"),
        (54, "Cuba")]

# The state line carries one `pN=x,y,z` per player, not a single `pos=`. It said `pos=` here
# until the game gained per-player output, and because that group is optional the regex did not
# fail loudly: it stopped matching entirely and every run reported "no state lines", which reads
# as a stale build rather than as a broken pattern. Capture player 0 and step over the rest.
LINE = re.compile(r"f=(\d+) players=(\d+)"
                  r"(?: p0=([-\d.]+),([-\d.]+),([-\d.]+))?"
                  r"(?: p\d+=[-\d.]+,[-\d.]+,[-\d.]+)*"
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
               GETV_WINDOW="320x240")
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


def track(samples):
    """Player 0's position at each sample, keyed by frame so two runs can be lined up."""
    return {int(m.group(1)): (float(m.group(3)), float(m.group(4)), float(m.group(5)))
            for m in samples if m.group(3)}


def summarise(samples, control):
    """What the driven run did, and how much of it the control run did NOT do.

    `control` is the same stage with the stick left alone. Everything the level does on its
    own -- the cutscene handoff, spawn settling, a scripted intro walk -- appears in both and
    cancels. What is left is the effect of the input, which is the only thing this can
    usefully assert.
    """
    driven = track(samples)
    quiet = track(control) if control else {}
    pos = list(driven.values())

    moved = 0.0
    for a, b in zip(pos, pos[1:]):
        moved += max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))

    # Biggest single step, reported so the teleport stays visible rather than hiding inside a
    # total. On the four stages measured it was 86 to 97 percent of `moved`.
    steps = [max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
             for a, b in zip(pos, pos[1:])]
    biggest = max(steps) if steps else 0.0

    # The divergence, at the last frame the two runs have in common. Frame-keyed rather than
    # index-keyed: a run that produces fewer samples must not silently compare frame 300
    # against frame 900.
    shared = sorted(set(driven) & set(quiet))
    effect = 0.0
    for f in shared:
        a, b = driven[f], quiet[f]
        effect = max(effect, abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))

    objectives = int(samples[-1].group(6))
    statuses = {m.group(7) for m in samples}
    return {
        "frames": int(samples[-1].group(1)),
        "players": int(samples[-1].group(2)),
        "reached_gameplay": len(pos) > 0,
        "distance": moved,
        "biggest": biggest,
        "walked": moved - biggest,
        "input_effect": effect,
        "compared": len(shared),
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
    # Live, because a full sweep is twenty-odd stages at roughly twenty seconds each and a
    # run that prints nothing for seven minutes is indistinguishable from a run that hung.
    # \r rewrites the in-progress line in place and \033[K clears whatever the longer
    # "running" text left behind; flush because stdout is block-buffered down a pipe, which
    # is exactly where this gets watched from.
    live = sys.stdout.isatty()
    eol = "\033[K" if live else ""   # clearing only means anything on a terminal
    # `spawn` is the biggest single step, which is the cutscene handoff on every stage
    # measured. `walked` is everything else. `input` is the gap between this run and the
    # control, and it is the only column the verdict reads.
    print(f"  {'stage':<12}{'gameplay':>9}{'spawn':>9}{'walked':>9}{'input':>9}"
          f"{'objs':>6}{'changed':>9}{'done':>6}{'secs':>7}", flush=True)
    for sid, name in todo:
        if live:
            print(f"  {name:<12}{'running':>9}", end="\r", flush=True)
        t0 = time.time()
        samples, err = run(sid, a.frames, not a.no_forward)
        if err:
            print(f"  {name:<12}{err}{eol}", flush=True)
            bad += 1
            continue
        if live:
            print(f"  {name:<12}{'control':>9}", end="\r", flush=True)
        control, cerr = run(sid, a.frames, False)
        secs = time.time() - t0
        if cerr:
            print(f"  {name:<12}control run: {cerr}{eol}", flush=True)
            bad += 1
            continue

        s = summarise(samples, control)
        exempt = sid in NO_INPUT_EXPECTED
        ok = s["reached_gameplay"] and (exempt or s["input_effect"] > 1.0)
        if not ok:
            bad += 1
        # An exemption that has quietly started responding is worth knowing about too: it means
        # either the stage changed or the reason recorded for it was wrong.
        if exempt and s["input_effect"] > 1.0:
            print(f"  {name:<12}now responds to the stick, but is listed as "
                  f"\"{NO_INPUT_EXPECTED[sid]}\"{eol}", flush=True)
        print(f"  {name:<12}{'yes' if s['reached_gameplay'] else 'NO':>9}"
              f"{s['biggest']:>9.0f}{s['walked']:>9.0f}"
              f"{s['input_effect']:>9.0f}"
              f"{s['objectives']:>6}"
              f"{'yes' if s['status_changed'] else 'no':>9}"
              f"{'yes' if s['complete'] else 'no':>6}"
              f"{secs:>7.1f}{eol}", flush=True)
    print()
    print(f"  {len(todo) - bad} of {len(todo)} reached gameplay AND responded to the stick",
          flush=True)
    for i, n in todo:
        if i in NO_INPUT_EXPECTED:
            print(f"  {n} is exempt: {NO_INPUT_EXPECTED[i]}", flush=True)
    if bad:
        print("  a zero `input` column means the scripted stick changed nothing: the run "
              "proves the level loads, not that input works", flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
