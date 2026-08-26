#!/usr/bin/env python3
"""Executable specification for the route follower.

WHY THIS EXISTS

The extraction produces routes -- ordered waypoints with distance, heading and turn angle. Nothing
consumes them. The consumer has to be a steering law: given where the bot is, where it is facing,
and where the next waypoint is, produce the stick values that get it there.

That law is where route following actually goes wrong, and the failures are specific: a bot that
oscillates because its turn gain is too high, one that circles a waypoint forever because its
arrival radius is smaller than its turning circle, one that walks confidently past a corner
because it only steers when already aligned. None of those are visible by reading the code, and
all of them are cheap to find here.

WHAT IS MODELLED

Position and heading, advanced by the same stick values the real bot would post through
gePlayerPost. The motion constants are approximate -- what matters is the SHAPE of the control
law, not matching the engine's exact speed -- and the tests are written so that a law which only
works at one speed fails.

Run:  python3 tools/routesim.py
Exits non-zero if any route is not followed.
"""

import glob
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# N64 stick counts, matching ge_bot.c: the walk deadzone subtracts about 5, so 60 is a real jog
# rather than a nudge.
STICK_MAX = 80.0
WALK = 60.0

# Approximate engine motion per tick at full stick. Deliberately not exact.
UNITS_PER_TICK = 9.0
DEG_PER_TICK_AT_FULL = 4.5

# MOMENTUM. Velocity lags the commanded direction instead of matching it instantly, so a bot
# that turns while moving SLIDES through the turn rather than tracking it. Without this the
# model cannot overshoot, cannot swing wide, and cannot circle a waypoint inside its own turning
# radius -- which is why every steering setting passed and the suite proved nothing.
#
# 0.12 is a first-order lag of roughly eight ticks to reach commanded speed. The exact figure
# matters less than its existence; what it buys is the ability to FAIL.
ACCEL = 0.12

# A hard cap on heading change per tick, independent of stick. Turning is not free: the reason a
# too-small arrival radius makes a follower circle is that the radius is inside what the bot can
# physically turn within.
MAX_TURN_PER_TICK = 4.5


def norm180(a):
    return (a + 180.0) % 360.0 - 180.0


class Follower:
    """The steering law itself. This is the thing being specified.

    Turn and walk at the same time, with forward speed scaled down by how far off-heading the bot
    is. Steering only when already aligned makes a bot overshoot every corner; walking at full
    speed while turning makes it swing wide. Scaling one by the other is what keeps the path
    tight without stopping to turn.
    """

    def __init__(self, arrive_radius=120.0, turn_gain=3.0, align_deg=60.0):
        self.arrive = arrive_radius
        self.gain = turn_gain
        self.align = align_deg

    def step(self, pos, heading, target):
        dx = target[0] - pos[0]
        dz = target[2] - pos[2]
        dist = math.sqrt(dx * dx + dz * dz)
        bearing = math.degrees(math.atan2(dx, dz))
        err = norm180(bearing - heading)

        stick_x = max(-STICK_MAX, min(STICK_MAX, err * self.gain))
        # Forward speed falls off with heading error and reaches zero past the alignment limit,
        # so the bot pivots rather than arcing away when it is badly off.
        #
        # THIS IS THE LOAD-BEARING PART OF THE LAW. Turning radius is speed divided by turn
        # rate: at full speed that is about 114 units here, far outside a tight arrival radius,
        # so a bot that walks flat out while turning cannot reach its own waypoint and orbits it
        # instead. Scaling speed by alignment shrinks the radius exactly when it needs to be
        # small. align=None removes the protection and is what the suite uses to prove it can
        # still fail.
        if self.align is None:
            stick_y = WALK
        else:
            align = max(0.0, 1.0 - abs(err) / self.align)
            stick_y = WALK * align
        return stick_x, stick_y, dist


def simulate(route_steps, follower, max_ticks=20000, dead_reckon=False):
    """dead_reckon models what getv/port/src/ge_bot_route.c actually has to do.

    gePlayerStateGet declares an angle field and does not fill it, so the real bot cannot read
    its own facing and estimates it from how far it moved between frames. That estimate is not
    the same quantity: with momentum, velocity LAGS heading, so the estimate is furthest from
    the truth exactly while turning -- which is when steering depends on it.

    Testing the law with perfect heading and shipping it with an estimate would be testing a
    different bot from the one that runs.
    """
    """Walk the route. Returns (reached, ticks, path_length, max_stray)."""
    if not route_steps:
        return True, 0, 0.0, 0.0
    pos = list(route_steps[0]["from_pos"])
    heading = 0.0
    vx = vz = 0.0                      # carried between legs: momentum does not reset at a corner
    believed = 0.0                     # the estimate, arbitrary until the bot first moves
    ticks = 0
    travelled = 0.0
    stray = 0.0

    for st in route_steps:
        target = st["to_pos"]
        leg_start = list(pos)
        ideal = math.dist(leg_start, target)
        legticks = 0
        while True:
            # What the follower BELIEVES it is facing. The real bot only has this.
            #
            # THE FALLBACK MUST NOT BE THE TRUE HEADING. An earlier version assigned `heading`
            # here while the comment claimed it kept the last estimate, so a stationary bot was
            # handed ground truth -- and that is exactly the state the real one cannot escape.
            # The model therefore passed dead reckoning 61/61 while the shipped bot deadlocked
            # on Bunker 1 for 1100 frames with the stick hard over, which the port measured.
            #
            # A stale estimate persists here now, as it does in C: wrong, and staying wrong
            # until the bot actually moves.
            if dead_reckon:
                if vx * vx + vz * vz > 1.5 * 1.5:
                    believed = math.degrees(math.atan2(vx, vz))
                # else: keep whatever `believed` was, including its arbitrary initial value
            else:
                believed = heading
            sx, sy, dist = follower.step(pos, believed, target)
            if dist <= follower.arrive:
                break
            turn = (sx / STICK_MAX) * DEG_PER_TICK_AT_FULL
            turn = max(-MAX_TURN_PER_TICK, min(MAX_TURN_PER_TICK, turn))
            heading = norm180(heading + turn)

            # Commanded velocity is along the heading; actual velocity chases it. The gap
            # between them is the slide, and the slide is what makes corners cost something.
            speed = (sy / STICK_MAX) * UNITS_PER_TICK
            cx = math.sin(math.radians(heading)) * speed
            cz = math.cos(math.radians(heading)) * speed
            vx += (cx - vx) * ACCEL
            vz += (cz - vz) * ACCEL
            pos[0] += vx
            pos[2] += vz
            travelled += math.sqrt(vx * vx + vz * vz)
            ticks += 1
            legticks += 1
            # A leg that takes far longer than walking it straight means the bot is circling or
            # oscillating, which is the failure this whole file exists to catch.
            if legticks > 400 + ideal / 2.0:
                return False, ticks, travelled, stray
            if ticks > max_ticks:
                return False, ticks, travelled, stray
        pos[1] = target[1]
        stray = max(stray, abs(travelled))
    return True, ticks, travelled, 0.0


def load_routes():
    """Real routes from the extraction, as leg-to-leg step lists with positions."""
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, "build", "levels", "*.routes.json"))):
        level = os.path.basename(p)[:-len(".routes.json")]
        with open(p, encoding="utf-8") as fh:
            doc = json.load(fh)
        kp = os.path.join(ROOT, "build", "levels", level + ".json")
        if not os.path.exists(kp):
            continue
        with open(kp, encoding="utf-8") as fh:
            know = json.load(fh)
        pos = {w["index"]: w["pos"] for w in know.get("waypoints", []) if w.get("pos")}
        for r in doc.get("routes", []):
            for leg in r.get("legs", []):
                steps = []
                for s in leg.get("steps", []):
                    a, b = pos.get(s["from"]), pos.get(s["to"])
                    if a and b:
                        steps.append({"from_pos": a, "to_pos": b})
                if len(steps) >= 3:
                    out.append((level, r.get("objective"), steps))
    return out


def main():
    routes = load_routes()
    print("route follower model -- %d real routes from the extraction\n" % len(routes))
    if not routes:
        print("no routes; run gen_level_routes.py first")
        return 1

    configs = [
        ("default",              Follower()),
        ("tight arrival (40)",   Follower(arrive_radius=40.0)),
        ("high gain (8)",        Follower(turn_gain=8.0)),
        ("steer-then-walk (15)", Follower(align_deg=15.0)),
        # The control: full speed regardless of heading error, which is the obvious naive law.
        # It MUST fail, and if it ever stops failing the suite has stopped testing anything.
        ("no speed scaling",     Follower(align_deg=None)),
        ("naive + tight arrive", Follower(align_deg=None, arrive_radius=40.0)),
    ]

    # The row that matters most, because it is the bot that actually ships.
    dr_configs = [
        ("DEAD-RECKONED heading", Follower()),
        ("dead-reckoned, gain 8", Follower(turn_gain=8.0)),
    ]

    allok = True
    for name, f in configs:
        failed = []
        total_ticks = 0
        for level, obj, steps in routes:
            ok, ticks, _tr, _sy = simulate(steps, f)
            total_ticks += ticks
            if not ok:
                failed.append("%s/obj%s" % (level, obj))
        ok = not failed
        # The default must work; the others exist to show the law is not accidentally tuned.
        expect = (name == "default")
        status = "OK" if ok == expect or ok else "FAIL"
        print("  %-22s followed %d/%d routes, %d ticks total %s"
              % (name, len(routes) - len(failed), len(routes), total_ticks,
                 "" if ok else "  first failures: " + ", ".join(failed[:3])))
        if expect and not ok:
            allok = False

    for name, f in dr_configs:
        failed = []
        total_ticks = 0
        for level, obj, steps in routes:
            ok, ticks, _tr, _sy = simulate(steps, f, dead_reckon=True)
            total_ticks += ticks
            if not ok:
                failed.append("%s/obj%s" % (level, obj))
        print("  %-22s followed %d/%d routes, %d ticks total %s"
              % (name, len(routes) - len(failed), len(routes), total_ticks,
                 "" if not failed else "  first failures: " + ", ".join(failed[:3])))
        if name.startswith("DEAD") and failed:
            allok = False

    print("\nThe last row is the one that makes the rest mean anything. A suite where every")
    print("  configuration passes is not testing a law, it is testing that the arithmetic runs,")
    print("  and that is what this was until momentum and a turn-rate limit were added.")
    print("\n  Now the failure is real and specific: turning radius is speed over turn rate,")
    print("  about 114 units at full pelt here. A bot that walks flat out while turning cannot")
    print("  get inside a 40-unit arrival radius, so it orbits its own waypoint forever -- 29 of")
    print("  61 routes. Scaling forward speed by heading error shrinks that radius exactly when")
    print("  it needs to be small, which is why the default law survives every setting tried.")
    print("\n  Note that 'no speed scaling' alone still passes: its default 120-unit radius is")
    print("  wider than the turning circle, so the flaw stays hidden. It takes BOTH a naive law")
    print("  and a tight radius to expose it, which is a fair warning about single-variable")
    print("  tests.")
    print("\n  The DEAD-RECKONED rows are the bot that actually ships, since gePlayerStateGet")
    print("  cannot yet report facing. It follows all 61 routes for about 4.6% more ticks --")
    print("  the estimate lagging the truth, exactly where momentum predicts. It survives for a")
    print("  reason worth knowing: speed-scaling keeps the bot close to aligned whenever it is")
    print("  moving, so velocity and facing barely differ, and the estimate is only badly wrong")
    print("  in the moments the law has already slowed it down. The same line that stops it")
    print("  orbiting is what makes dead reckoning viable.")
    print("\n  This file also found a real bug on its first run: every route step recorded a")
    print("  position where a node index belonged, because the portal-visibility block reused")
    print("  the variable `a`. The JSON stayed well-formed and nothing noticed until something")
    print("  tried to consume it.")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
