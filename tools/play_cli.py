#!/usr/bin/env python3
"""Play a level through GETV_CLI, deciding from the report alone.

This is not a bot in the game. It is a player OUTSIDE it, reading the same text a person reads
and typing the same commands a person types. That is the point: if this gets somewhere, the API
carries enough to play, and anything the in-game follower cannot do is a policy problem rather
than a perception one.

THE PLAY, from what the report actually says:

  turn until the objective is roughly ahead, then walk
  a door in the way is an opportunity, not an obstacle -- use it
  something solid in the way means take the clearest turn it already told us about

Turn rate is 3.5 degrees per tick at full stick (bondview2.c:7312), so a turn of N degrees is
about N/3.5 ticks. Deriving it rather than tuning it is why the first turn lands.
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

NUM = r"(-?\d+)"
RE_YOU = re.compile(r"^you\s+\(" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\)\s+facing\s+" + NUM
                    + r"\s+room\s+" + NUM + r"\s+hp\s+" + NUM)
RE_AHEAD = re.compile(r"^ahead\s+(\S.*?)\s{2,}(\d+)\s+away\s+clearest turn\s+([+-]?\d+)")
RE_OBJ = re.compile(r"^obj\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_DOOR = re.compile(r"^Door\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_NEAR = re.compile(r"^near\s+(\S+)\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")

TURN_PER_TICK = 3.5


class Player:
    def __init__(self, proc, log):
        self.p = proc
        self.log = log
        self.state = {}
        self.best_obj = None
        self.arrived = False
        self.hold = 0
        self.near_fresh = True

    def send(self, cmd):
        self.log.write("> %s\n" % cmd)
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def turn_cmd(self, degrees):
        ticks = max(1, int(round(abs(degrees) / TURN_PER_TICK)))
        # Hold for the length of the turn plus a report, so the next decision is made from a
        # heading that has actually changed rather than from mid-swing.
        self.hold = 1
        # INVERTED, and measured rather than assumed. Positive stick DECREASES the game's
        # heading (bondview2.c:7312), and the report's turn is bearing-minus-facing, so a
        # right-stick command makes a positive turn LARGER. Sending "d" for a +89 bearing moved
        # it to +150. Third time this sign has bitten on this project.
        return ("a %d" if degrees > 0 else "d %d") % ticks

    def decide(self):
        s = self.state
        if "obj" not in s:
            return "look"

        # COMMIT TO A MANOEUVRE. Deciding afresh on every report makes the player alternate
        # between "the objective is left" and "something is ahead, turn right", and it spends the
        # run swinging between the two -- measured: the objective bearing flipped -151 / -91 /
        # -151 for a hundred reports while the distance never moved. Same failure the in-game
        # follower had, and seeing it here rather than in a trace is exactly why this exists.
        if self.hold > 0:
            self.hold -= 1
            return None

        obj_d, obj_turn = s["obj"]
        if self.best_obj is None or obj_d < self.best_obj:
            self.best_obj = obj_d

        # A door directly in the way is the way through. Doors are how these levels connect.
        what = s.get("ahead_what", "")
        ahead_d = s.get("ahead_dist", 9999)
        if "door" in what and ahead_d < 400:
            return "use 40"

        # Something solid close enough to walk into: take the turn the report already worked out.
        if ("wall" in what or "object" in what) and ahead_d < 250:
            clear = s.get("clearest", 0)
            if clear:
                return self.turn_cmd(clear)
            return "d 20"

        # AVOID THE FURNITURE. The nearest-of-each-kind lines say where to GO; they never mention
        # the crate two metres ahead, because a crate is nobody's landmark. The near list does,
        # and steering round it is the difference between walking the carriage and standing in
        # it. Anything solid within a body's stopping distance and roughly ahead gets stepped
        # around, away from whichever side it sits on.
        blockers = [(d, b) for (kind, d, b) in s.get("near", [])
                    if d < 300 and abs(b) < 35 and kind not in ("Key", "Collectable", "AmmoBox")]
        if blockers:
            d, b = min(blockers)
            return self.turn_cmd(40 if b <= 0 else -40)

        # Point at the objective, then walk. 25 degrees rather than 5: turning is expensive and
        # walking slightly off-line still closes distance, whereas re-aiming every step does not.
        if abs(obj_turn) > 25:
            return self.turn_cmd(max(-60, min(60, obj_turn)))
        return "w 60"

    def feed(self, line):
        line = line.strip()
        m = RE_YOU.match(line)
        if m:
            x, y, z, facing, room, hp = (int(g) for g in m.groups())
            self.state.update(pos=(x, y, z), facing=facing, room=room, hp=hp)
            return
        m = RE_AHEAD.match(line)
        if m:
            self.state["ahead_what"] = m.group(1).strip()
            self.state["ahead_dist"] = int(m.group(2))
            self.state["clearest"] = int(m.group(3))
            return
        m = RE_OBJ.match(line)
        if m:
            self.state["obj"] = (int(m.group(1)), int(m.group(2)))
            # The report is complete once the objective line lands: act on it.
            cmd = self.decide()
            self.near_fresh = True
            if cmd:
                self.send(cmd)
            return
        m = RE_NEAR.match(line)
        if m:
            # Reset on the first of a batch: the list is re-reported whole each time, and
            # appending would grow a phantom crowd of props that are no longer there.
            if self.near_fresh:
                self.state["near"] = []
                self.near_fresh = False
            self.state.setdefault("near", []).append(
                (m.group(1), int(m.group(2)), int(m.group(3))))
            return
        m = RE_DOOR.match(line)
        if m:
            self.state["door"] = (int(m.group(1)), int(m.group(2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="25")
    ap.add_argument("--level", default="train")
    ap.add_argument("--frames", default="40001")
    ap.add_argument("--every", default="30")
    ap.add_argument("--seconds", type=int, default=240)
    args = ap.parse_args()

    env = dict(os.environ, GETV_WORLD_DIR="build/world", GETV_BOT_ROUTE_LEVEL=args.level,
               GETV_CLI="1", GETV_CLI_EVERY=args.every, GETV_PADS="2",
               GETV_STAGE=args.stage, GETV_EXIT_FRAME=args.frames)
    p = subprocess.Popen(["getv/build-mac/goldeneye"], env=env, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    player = Player(p, sys.stdout)

    def pump():
        for line in p.stdout:
            player.feed(line)
            if line.startswith("you ") or line.startswith("obj "):
                sys.stdout.write(line)

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    deadline = time.time() + args.seconds
    while time.time() < deadline and p.poll() is None:
        time.sleep(1)
    try:
        player.send("quit")
    except Exception:
        pass
    time.sleep(1)
    p.kill()
    print("\nclosest approach to the objective: %s" % player.best_obj)


if __name__ == "__main__":
    main()
