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
# weapon/ammo_clip/ammo_reserve are the last three fields ge_cli.c:173 sends on this line and were
# never captured -- re.match only needs the pattern to match from the START of the string, so the
# old, shorter pattern matched successfully every report and silently threw the trailing fields
# away rather than failing loudly. Same fix as the Key/Collectable landmarks below: capture what
# is already being sent. Optional (the \s+... )? group so a report from an OLDER binary without
# these fields still matches rather than breaking outright.
RE_YOU = re.compile(r"^you\s+\(" + NUM + r"\s+" + NUM + r"\s+" + NUM + r"\)\s+facing\s+" + NUM
                    + r"\s+room\s+" + NUM + r"\s+hp\s+" + NUM + r"%?"
                    + r"(?:\s+weapon\s+" + NUM + r"\s+ammo\s+" + NUM + r"/" + NUM + r")?")
RE_AHEAD = re.compile(r"^ahead\s+(\S.*?)\s{2,}(\d+)\s+away\s+clearest turn\s+([+-]?\d+)\s*\((\d+) room\)")
RE_OBJ = re.compile(r"^obj\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_DOOR = re.compile(r"^Door\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
# ge_cli.c reports the nearest Door, Key AND Collectable this way (ge_cli.c:230-237, all three
# kinds through the same printf), but only Door was ever parsed here -- the other two landmarks
# were being sent every report and silently dropped. Same regex shape as RE_DOOR, generalised
# rather than copy-pasted twice, since the format is identical by construction (one format string
# for all three kinds server-side).
RE_LANDMARK = re.compile(r"^(Key|Collectable)\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_NEAR = re.compile(r"^near\s+(\S+)\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_ENEMY = re.compile(r"^enemy\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)(\s+SEES YOU)?")

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
        self.queue = []
        self.enemy_fresh = True

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

        # A queued follow-up runs before anything is reconsidered: that is what makes turn-then-go
        # one decision instead of two that argue with each other.
        if self.queue:
            return self.queue.pop(0)

        obj_d, obj_turn = s["obj"]
        if self.best_obj is None or obj_d < self.best_obj:
            self.best_obj = obj_d

        # SHOOT BACK. The player walked the first carriage at 100% health and reached the second
        # at 22%, because it had a loaded PP7 and never used it. Guards do not stop shooting
        # because you are busy navigating, and a route that ends in a corpse is not a route.
        #
        # Only at something that can see us: a guard facing elsewhere is not spending ammunition
        # on us and shooting it just makes noise. Nearest first, since it is doing the damage.
        # 900, not 2000. At 2000 there is ALWAYS a guard somewhere in a carriage that can see
        # you, so fighting outranks everything forever and the player never opens the door two
        # metres in front of it -- measured: it stood at a door 96 units away trading shots for a
        # whole run. Distant guards are a fact of the level, not an emergency; the ones close
        # enough to be doing real damage are.
        threats = sorted((d, b) for (d, b, sees) in s.get("enemies", []) if sees and d < 900)
        if threats:
            d, b = threats[0]
            if abs(b) > 12:
                return self.turn_cmd(max(-45, min(45, b)))
            return "fire"

        # A door directly in the way is the way through. Doors are how these levels connect.
        what = s.get("ahead_what", "")
        ahead_d = s.get("ahead_dist", 9999)
        if "door" in what and ahead_d < 400:
            return "use 40"

        # Something solid close enough to walk into: take the turn the report worked out, and
        # then WALK IT. A turn on its own leaves the player pointing at open ground and standing
        # still, so the next report sees the same obstacle and turns again -- which is how it
        # spent a whole run rotating beside one crate. Turn and go, as one plan.
        if ("wall" in what or "object" in what) and ahead_d < 250:
            clear = s.get("clearest", 0)
            room = s.get("room", 0)
            if clear:
                self.queue.append("w %d" % max(40, min(120, room // 4)))
                return self.turn_cmd(clear)
            # Nothing clear anywhere: back off rather than press on. Pressing is what wedges a
            # body into a corner it then cannot sense its way out of.
            self.queue.append("d 26")
            return "s 40"

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
            x, y, z, facing, room, hp, weapon, clip, reserve = m.groups()
            x, y, z, facing, room, hp = int(x), int(y), int(z), int(facing), int(room), int(hp)
            self.state.update(pos=(x, y, z), facing=facing, room=room, hp=hp)
            # None on a report from an older binary without these fields (see RE_YOU); left unset
            # rather than defaulted to 0, so "no ammo data yet" cannot be confused with "no ammo".
            if weapon is not None:
                self.state.update(weapon=int(weapon), ammo_clip=int(clip),
                                  ammo_reserve=int(reserve))
            return
        m = RE_AHEAD.match(line)
        if m:
            self.state["ahead_what"] = m.group(1).strip()
            self.state["ahead_dist"] = int(m.group(2))
            self.state["clearest"] = int(m.group(3))
            self.state["room"] = int(m.group(4))
            return
        m = RE_OBJ.match(line)
        if m:
            self.state["obj"] = (int(m.group(1)), int(m.group(2)))
            # The report is complete once the objective line lands: act on it.
            cmd = self.decide()
            self.near_fresh = True
            self.enemy_fresh = True
            if cmd:
                self.send(cmd)
            return
        m = RE_ENEMY.match(line)
        if m:
            if self.enemy_fresh:
                self.state["enemies"] = []
                self.enemy_fresh = False
            self.state.setdefault("enemies", []).append(
                (int(m.group(1)), int(m.group(2)), bool(m.group(3))))
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
            return
        m = RE_LANDMARK.match(line)
        if m:
            # Stored, not acted on. Adding a decide() rule ("go get the key when blocked by a
            # locked door") needs a measured case where the run actually needed one -- this
            # project's standing practice is grounding a policy in a failure that was observed,
            # not one that seems plausible. What was a clear, unjustified gap is that the report
            # already sends this and it was being thrown away; that half is fixed here. The
            # decision half is left to whoever has a run that shows it is needed.
            kind = m.group(1).lower()
            self.state[kind] = (int(m.group(2)), int(m.group(3)))



# THE MAC PATH WAS THE ONLY PATH. This tool could not run at all on Windows or Linux --
# subprocess.Popen would raise FileNotFoundError immediately on "getv/build-mac/goldeneye", every
# time, on every platform that is not mac. Not a config gap noticed later: the tool never ran here
# even once before this. Resolved by platform first, with an explicit --exe escape hatch for a
# custom build location, rather than guessing a single new hardcoded path and reproducing the same
# problem for whoever is on the third platform.
_DEFAULT_EXE = {
    "win32":  "getv/build-windows/goldeneye.exe",
    "darwin": "getv/build-mac/goldeneye",
    "linux":  "getv/build-linux/goldeneye",
}


def default_exe():
    return _DEFAULT_EXE.get(sys.platform, _DEFAULT_EXE["linux"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="25")
    ap.add_argument("--level", default="train")
    ap.add_argument("--frames", default="40001")
    ap.add_argument("--every", default="30")
    ap.add_argument("--seconds", type=int, default=240)
    ap.add_argument("--exe", default=None,
                    help="path to the goldeneye binary; default picked from sys.platform "
                         "(%s)" % default_exe())
    ap.add_argument("--world-dir", default="build/world")
    args = ap.parse_args()

    exe = args.exe or default_exe()
    if not os.path.isfile(exe):
        sys.exit("no binary at %s -- build it first, or pass --exe" % exe)

    env = dict(os.environ, GETV_WORLD_DIR=args.world_dir, GETV_BOT_ROUTE_LEVEL=args.level,
               GETV_CLI="1", GETV_CLI_EVERY=args.every, GETV_PADS="2",
               GETV_STAGE=args.stage, GETV_EXIT_FRAME=args.frames)
    # MinGW's runtime DLLs, or a Windows launch dies with STATUS_DLL_NOT_FOUND and -- because it
    # is a GUI-subsystem binary -- produces no output and sets no exit code, so a run that never
    # happened looks exactly like one that printed nothing. Cost an hour once already, elsewhere
    # in this project (tools/bench_windows.ps1); no reason to pay it again here.
    if sys.platform == "win32":
        mingw = r"C:\msys64\mingw64\bin"
        if os.path.isdir(mingw) and mingw not in env.get("PATH", ""):
            env["PATH"] = mingw + os.pathsep + env.get("PATH", "")
    p = subprocess.Popen([exe], env=env, stdin=subprocess.PIPE,
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
