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
# hp and alert are optional so an older binary's shorter line still matches rather than being
# dropped in silence. DYING is the game's death animation already running: the character is still
# in the world and still reported, and firing into it is the commonest way an automated player
# wastes a magazine and its attention.
RE_ENEMY = re.compile(r"^enemy\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)"
                      r"(?:,\s+hp\s+(-?\d+)/(-?\d+))?"
                      r"(?:,\s+alert\s+(\d+))?"
                      r"(\s+SEES YOU|\s+DYING)?")

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
        self.reports = 0
        self.hurt_at = -999

    transcript = None

    def send(self, cmd):
        if self.transcript is not None:
            self.transcript.write("> %s\n" % cmd)
            self.transcript.flush()
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

        # WHO CAN SEE US, AND ARE WE BEING HIT.
        #
        # Two different questions, and the answers call for different play. Being SEEN is the
        # earlier signal -- the guard has us in its cone and is about to start -- and being hit is
        # the late one. Acting on the first is what keeps the health bar full; the run that
        # prompted this reached the second carriage at 22% by treating them as the same thing.
        #
        # A DYING guard is not a threat and not a target. The death animation runs for a while
        # after its health reaches zero and it is reported the whole time, so shooting it is a
        # magazine and several seconds spent on something already resolved.
        enemies = [e for e in s.get("enemies", []) if not e["dying"]]
        seen_by = sorted((e["dist"], e["turn"]) for e in enemies if e["sees"])
        under_fire = (self.reports - self.hurt_at) <= 3
        hp = s.get("hp", 100)

        # BREAK CONTACT WHEN LOSING. Trading shots is only sensible while we are winning the
        # trade. Hurt, under fire and outnumbered means turn away from the nearest one and put
        # distance and geometry between us -- their cone is a fact we can act on, and a corridor
        # gives plenty to stand behind.
        if seen_by and under_fire and hp <= 40 and len(seen_by) > 1:
            _, b = seen_by[0]
            self.queue.append("w 80")
            return self.turn_cmd(180 if b >= 0 else -180)

        # FIGHT BACK BEFORE ANYTHING ELSE. Distant guards are a fact of a carriage rather than an
        # emergency, so the plain case is bounded at 900 -- at 2000 something can always see you
        # and the player never gets round to the door two metres away. But once we are actually
        # being hit, whoever can see us IS the emergency however far off they are, because that is
        # exactly where the damage is coming from.
        reach = 2500 if under_fire else 900
        threats = [(d, b) for (d, b) in seen_by if d < reach]
        if threats:
            d, b = threats[0]
            if abs(b) > 12:
                return self.turn_cmd(max(-45, min(45, b)))
            return "fire"

        # A door directly in the way is the way through. Doors are how these levels connect.
        what = s.get("ahead_what", "")
        ahead_d = s.get("ahead_dist", 9999)
        # CROSS-CHECK THE DOOR AGAINST THE REPORT'S OWN LANDMARK LINE.
        #
        # "ahead" is a bitmask of what the ray touched, and on Train it reads "wall door object"
        # at a spot where the nearest actual door -- by the report's own Door line, in the same
        # report -- is 4,935 units away. The mask's door bit fires on the crate at (-373, -80).
        # Believing it cost 184 "use" commands in one run against a crate, at 88% health and
        # falling, while the way round sat at "clearest turn +60" in the same four lines.
        #
        # So a door is only a door when both halves of the report agree it is there. When they
        # disagree the mask is treated as the obstacle it also claims, which is what the code
        # below already knows how to walk around.
        door_d = s["door"][0] if s.get("door") else None
        door_real = door_d is not None and door_d <= max(400, ahead_d * 2)
        if "door" in what and ahead_d < 400 and door_real:
            # NOT WHILE IT IS BEING WATCHED. A door is a pause: the animation plays, the doorway
            # funnels us, and anything with eyes on that spot gets a free shot at a player who is
            # standing still in it. Clear the room first, then walk through.
            #
            # The watcher has to be close enough to be the reason, though. Gated only on "can
            # anything see us", this refused every door on the level to a guard 1,800 units away
            # and the player stood at its spawn at full health for a whole run -- which is not
            # caution, it is paralysis. And refusing a door has to mean DOING something about the
            # guard: a door worth guarding is worth the fight, so this engages at a longer reach
            # than the plain case rather than waiting for the guard to lose interest.
            watching = sorted((e["dist"], e["turn"]) for e in enemies
                              if e["sees"] and e["dist"] < 1400)
            if watching:
                d, bear = watching[0]
                if abs(bear) > 12:
                    return self.turn_cmd(max(-45, min(45, bear)))
                return "fire"
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

        # avoid the furniture. The nearest-of-each-kind lines say where to GO; they never mention
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

        # A REPORT IS A SNAPSHOT, NOT AN ACCUMULATION.
        #
        # Lines are only printed when there is something to say: no obstacle ahead means no
        # "ahead" line, no guard in range means no "enemy" line. Merging each report into the
        # previous one therefore keeps whatever was last true forever. Measured on Train: the
        # player stood at (-282, -37) issuing "use 40" a hundred and eighty times against a door
        # it had walked past long before, because the last "ahead" line it ever saw said door and
        # nothing since had contradicted it. Clearing on the frame marker means an absent line
        # reads as absent rather than as unchanged.
        if line.startswith("--- f"):
            for k in ("ahead_what", "ahead_dist", "clearest", "near", "enemies",
                      "door", "Key", "Collectable"):
                self.state.pop(k, None)
            self.near_fresh = True
            self.enemy_fresh = True
            return
        m = RE_YOU.match(line)
        if m:
            x, y, z, facing, room, hp, weapon, clip, reserve = m.groups()
            x, y, z, facing, room, hp = int(x), int(y), int(z), int(facing), int(room), int(hp)
            prev_hp = self.state.get("hp")
            if prev_hp is not None and hp < prev_hp:
                self.hurt_at = self.reports          # the report we last took damage on
            self.state.update(pos=(x, y, z), facing=facing, room=room, hp=hp)
            self.reports += 1
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
            tail = (m.group(6) or "").strip()
            self.state.setdefault("enemies", []).append({
                "dist":  int(m.group(1)),
                "turn":  int(m.group(2)),
                "hp":    int(m.group(3)) if m.group(3) is not None else None,
                "alert": int(m.group(5)) if m.group(5) is not None else None,
                "sees":  tail == "SEES YOU",
                "dying": tail == "DYING",
            })
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



# the mac path was the only path. This tool could not run at all on Windows or Linux --
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
    ap.add_argument("--transcript", help="write every report line and command sent here")
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

    # The transcript is every line the player saw, in the order it saw them, with the commands it
    # sent interleaved. The console stays a summary -- position and objective -- because a full
    # report every frame is unreadable at speed. Diagnosing off the summary is how an afternoon
    # went into a stale "ahead" reading that the summary never showed: the console had no "ahead"
    # line in it, so it looked like the game had stopped sending one.
    transcript = open(args.transcript, "w", encoding="utf-8") if args.transcript else None
    if transcript is not None:
        player.transcript = transcript

    def pump():
        for line in p.stdout:
            if transcript is not None:
                transcript.write(line)
                transcript.flush()
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
