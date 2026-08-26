#!/usr/bin/env python3
"""Record where each level ACTUALLY puts the player, by booting it and asking the game.

Why this exists

gen_level_routes.py has to pick a start node for every route, and it has no way to know where
the player spawns -- the setup's start pads are inside the ROM data the extractor reads, but
which pad is chosen, and where the engine finally places the body after the stan query, is a
runtime decision. So it guesses, and marks the guess with "spawn_is_assumed": true.

The guesses are wrong, and a route from the wrong start is worse than no route: every step is
well-formed, every distance is plausible, and the bot walks confidently into a wall a thousand
units from anything on its path. Measured before this tool existed:

    bunker1   assumed (274, 35, 304)      actual (-1381, 340, 2284)     off by ~2,400
    bunker2   assumed (-1293, 100, 1557)  actual (-3404, 167, 4539)     off by ~3,600
    dam       assumed (-1393, -5, -689)   actual (20198, 60, 16902)     off by ~26,000

Dam IS the interesting one. 26,000 units is not a wrong pad in the right level, it is a
different coordinate space -- the graph's extent is nowhere near those numbers. Fixing the spawn
alone will not fix Dam; its whole waypoint set needs checking against world coordinates first.

The game is the only authority here, which is why this boots it rather than reading the assets.

USAGE

    python3 tools/dump_spawns.py                 # every solo level, to build/levels/spawns.json
    python3 tools/dump_spawns.py --stages 9,33   # just these

Each level is booted headless with GETV_EXIT_FRAME so the run ends on a fixed frame and two
invocations are comparable. GETV_STATEAPI prints the state readout; the first line for slot 0
is the spawn, before anything has had a chance to move the player.
"""
import argparse, json, os, re, subprocess, sys

# The 20 solo campaign levels. MP-only stages (31, 38, 45, 46, 48, 50) are excluded: they have
# no solo setup at all and boot into the deliberate "no setup data" refusal. Stage 0 is not a
# level, and 21/90 silently alias Bunker 1's background.
SOLO_STAGES = {
    9: "bunker1", 20: "silo",    22: "statue",  23: "control",  24: "archives",
    25: "train",  26: "frigate", 27: "bunker2", 28: "aztec",    29: "streets",
    30: "depot",  32: "egypt",   33: "dam",     34: "facility", 35: "runway",
    36: "surface", 37: "jungle", 39: "caverns", 41: "cradle",   43: "surface2",
}

STATE = re.compile(r"\[getv\]\[state\] p0 .*?pos=\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\).*?ang=(-?[\d.]+).*?room=(-?\d+)")


def boot(binary, stage, frames):
    env = dict(os.environ, GETV_STATEAPI="1", GETV_STAGE=str(stage),
               GETV_EXIT_FRAME=str(frames))
    try:
        out = subprocess.run([binary], env=env, capture_output=True, text=True,
                             timeout=300).stdout
    except subprocess.TimeoutExpired:
        return None, "timed out"
    m = STATE.search(out)
    if not m:
        if "has no setup data" in out:
            return None, "no setup data"
        return None, "no state line"
    x, y, z, ang, room = m.groups()
    return {"pos": [float(x), float(y), float(z)],
            "angle": float(ang), "room": int(room)}, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("getv", "build-mac", "goldeneye"))
    ap.add_argument("--out", default=os.path.join("build", "levels", "spawns.json"))
    ap.add_argument("--stages", default="")
    # 301 is a k*60+1 frame, which is the project's rule for a comparable measurement. It is
    # also comfortably past the point where the player exists but before any scripted opening
    # camera has handed control over and could move them.
    ap.add_argument("--frames", type=int, default=301)
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        sys.exit("no binary at %s -- build it first" % args.binary)

    wanted = SOLO_STAGES
    if args.stages:
        ids = {int(s) for s in args.stages.split(",") if s.strip()}
        wanted = {k: v for k, v in SOLO_STAGES.items() if k in ids}

    spawns, failed = {}, {}
    for stage, name in sorted(wanted.items()):
        rec, err = boot(args.binary, stage, args.frames)
        if rec is None:
            failed[name] = err
            print("  %-10s stage %-3d FAILED: %s" % (name, stage, err))
            continue
        rec["stage"] = stage
        spawns[name] = rec
        print("  %-10s stage %-3d pos=(%8.1f %6.1f %8.1f) ang=%5.1f room=%d"
              % (name, stage, rec["pos"][0], rec["pos"][1], rec["pos"][2],
                 rec["angle"], rec["room"]))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump({"source": "measured by booting the game, not read from assets",
                   "frame": args.frames, "spawns": spawns, "failed": failed}, f, indent=2)
    print("\n%d level(s) measured, %d failed -> %s" % (len(spawns), len(failed), args.out))
    if failed:
        # Loudly, because a silently short table looks like a complete one to whatever reads it.
        print("FAILED: %s" % ", ".join(sorted(failed)))


if __name__ == "__main__":
    main()
