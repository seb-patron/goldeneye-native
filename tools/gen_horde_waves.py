#!/usr/bin/env python3
"""Horde mode: a wave schedule per level, grounded in what the level can actually support.

Spawn POINTS are gen_horde_spawns.py. This is the schedule that uses them -- how many arrive, how
good they are, how long the player gets between waves, and how all three escalate.

EVERY NUMBER IS DERIVED FROM SOMETHING MEASURED, not chosen because it felt right:

  how many      capped by SPAWN POINTS ACTUALLY AVAILABLE on that level. Twelve enemies from four
                hidden arrival points means three materialising on top of each other, which reads
                as a bug. Small levels get small waves because that is what they can stage.
  how good      walks the REAL skill tiers -- meat(0), easy(1), normal(2), hard(3), perfect(4),
                dark(5) -- with their measured dials, rather than a difficulty multiplier. "Rank 3"
                means accuracy 70, speed 110, hearing 130, because that is what hard IS.
  how long      the break shrinks as rank rises, floored so it never becomes unplayable.
  how far       arrival points are drawn from across the whole distance band, so waves come from
                different parts of the level rather than one closet.

RANK IS HELD FOR SEVERAL WAVES BEFORE ADVANCING. Stepping the tier every wave means the player
never meets the same enemy twice and cannot learn one; six tiers over twenty waves gives roughly
three waves at each, which is enough to recognise what changed when it does.

AND A LEVEL THAT CANNOT STAGE A HORDE SAYS SO. A level with too few hidden, reachable arrival
points is reported as unsuitable rather than given a thin schedule -- the same rule the archetype
audit follows. A wave spawner that quietly degrades is worse than one that refuses.
"""
import argparse
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# Imported rather than redeclared: if the selector and the scheduler disagree about what counts as
# too few arrival points, the scheduler builds waves around points the selector already judged
# unusable, and nothing reports the contradiction.
from gen_horde_spawns import MIN_SPAWNS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level", default="train")
    ap.add_argument("--waves", type=int, default=20)
    ap.add_argument("--break-max", type=float, default=25.0, help="seconds between early waves")
    ap.add_argument("--break-min", type=float, default=8.0, help="floor, seconds")
    a = ap.parse_args()

    hp = os.path.join(a.levels, "%s.horde.json" % a.level)
    if not os.path.isfile(hp):
        sys.exit("no %s -- run tools/gen_horde_spawns.py --level %s first" % (hp, a.level))
    horde = json.load(open(hp, encoding="utf-8"))
    spawns = horde.get("spawns", [])

    tiers = sorted(json.load(open(os.path.join(ROOT, "build", "bots", "skill_tiers.json"),
                                  encoding="utf-8"))["archetypes"],
                   key=lambda x: x.get("rank", 99))
    personalities = json.load(open(os.path.join(ROOT, "build", "bots", "personalities.json"),
                                   encoding="utf-8"))["archetypes"]

    if len(spawns) < MIN_SPAWNS:
        print("%s: only %d hidden reachable arrival point(s) -- UNSUITABLE for horde."
              % (a.level, len(spawns)))
        print("   Reported rather than given a thin schedule: a spawner that quietly degrades")
        print("   puts enemies in front of the player, which reads as a bug in the game.")
        return 1

    n_sp = len(spawns)

    # Smallest stride >= 5 with no common factor with the point count -- see the note at the pick
    # site. 5 rather than 1 so consecutive waves do not simply walk the list in order, which would
    # march the arrivals up the level one door at a time.
    stride = next(s for s in range(5, 5 + n_sp + 1) if math.gcd(s, n_sp) == 1)

    waves = []
    for w in range(1, a.waves + 1):
        frac = (w - 1) / float(max(1, a.waves - 1))

        # Rank held in bands so each tier is met more than once.
        rank = min(len(tiers) - 1, int(frac * len(tiers)))
        tier = tiers[rank]

        # Count grows toward the level's staging capacity and never exceeds it: one enemy per
        # arrival point, so nothing has to share a tile.
        count = max(2, min(n_sp, int(round(2 + frac * (n_sp - 2)))))

        # Pressure rises as the break shrinks; floored so late waves stay playable.
        brk = round(max(a.break_min, a.break_max - frac * (a.break_max - a.break_min)), 1)

        # Every fifth wave layers a PERSONALITY over the tier -- a change of BEHAVIOUR rather than
        # of numbers, so the difficulty spike is something the player can read and adapt to instead
        # of just harder-shooting versions of the same guard. Cycling by index keeps it
        # deterministic; the same level always produces the same schedule, which matters when two
        # runs are being compared.
        special = None
        if w % 5 == 0 and personalities:
            special = personalities[(w // 5 - 1) % len(personalities)]["name"]

        # THE STRIDE MUST BE COPRIME WITH THE POINT COUNT OR WAVES REUSE DOORS ON A SHORT CYCLE.
        # A fixed stride of 3 into 12 points shares a factor of 3, so it reaches only 4 of the 12
        # starting offsets: waves 1, 5, 9, 13 and 17 all opened at the same tile, and waves 2, 6,
        # 10, 14, 18 at another. That is learnable by the third repeat, which throws away most of
        # the point of having twelve arrival points. Picking the stride coprime to n_sp makes the
        # offsets visit every point before any repeats.
        picks = [spawns[(w * stride + i) % n_sp] for i in range(count)]

        waves.append({
            "wave": w,
            "count": count,
            "skill": tier["name"],
            "rank": tier.get("rank"),
            "dials": tier.get("dials"),
            "personality": special,
            "break_after_s": brk if w < a.waves else 0.0,
            "spawn_tiles": [p["tile"] for p in picks],
        })

    total = sum(x["count"] for x in waves)
    dur = sum(x["break_after_s"] for x in waves)
    print("%s: %d arrival points, %d waves" % (a.level, n_sp, len(waves)))
    print("   %d enemies total, %.0fs of breaks (excluding fighting time)\n" % (total, dur))
    print("  wave  n  skill     acc/spd  break  personality  arrivals")
    for x in waves:
        d = x["dials"] or {}
        print("   %3d  %2d  %-8s %3s/%-4s %5.1fs  %-11s %s"
              % (x["wave"], x["count"], x["skill"], d.get("accuracy"), d.get("speed"),
                 x["break_after_s"], x["personality"] or "-",
                 ",".join(str(t) for t in x["spawn_tiles"][:5])))

    dest = os.path.join(a.levels, "%s.horde_waves.json" % a.level)
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({"level": a.level, "arrival_points": n_sp,
                   "totals": {"enemies": total, "break_seconds": dur},
                   "waves": waves}, fh, indent=1)
    print("\n-> %s" % dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
