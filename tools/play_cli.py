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
import math
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
RE_PATH = re.compile(r"^path\s+(\d+):\s+\((-?\d+)\s+(-?\d+)\)\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
RE_NOPATH = re.compile(r"^path\s+nothing reachable")
RE_TARGET = re.compile(r"^target\s+tag\s+(-?\d+)\s+#(\d+)\s+(\d+)\s+away,"
                       r"\s+turn\s+([+-]?\d+),\s+room\s+(-?\d+)")
RE_DOOR = re.compile(r"^Door\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
# ge_cli.c reports the nearest Door, Key AND Collectable this way (ge_cli.c:230-237, all three
# kinds through the same printf), but only Door was ever parsed here -- the other two landmarks
# were being sent every report and silently dropped. Same regex shape as RE_DOOR, generalised
# rather than copy-pasted twice, since the format is identical by construction (one format string
# for all three kinds server-side).
RE_LANDMARK = re.compile(r"^(Key|Collectable)\s+(?:tag\s+(-?\d+)\s+)?(\d+)\s+away,"
                         r"\s+turn\s+([+-]?\d+)(?:,\s+room\s+(\d+))?")
RE_NEAR = re.compile(r"^near\s+(\S+)\s+(\d+)\s+away,\s+turn\s+([+-]?\d+)")
# hp and alert are optional so an older binary's shorter line still matches rather than being
# dropped in silence. DYING is the game's death animation already running: the character is still
# in the world and still reported, and firing into it is the commonest way an automated player
# wastes a magazine and its attention.
RE_ENEMY = re.compile(r"^enemy\s+(?:#(\d+)\s+)?(\d+)\s+away,\s+turn\s+([+-]?\d+)"
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
        self.path_asked = False
        self.path_age = 0
        self.stalled = 0
        self.mapped = False
        self.shots = {}
        self.aimed = {}
        self.grabs = {}
        self.no_path = False
        self.tried_use = 0
        self.path_fine = False

    transcript = None

    rule = "-"

    def why(self, name, cmd):
        """Tag a decision with the rule that made it. A transcript of commands says what the
        player did; it never says why, and every wrong-rule bug so far has looked identical from
        the outside -- the player walking when it should have been shooting."""
        self.rule = name
        return cmd

    def send(self, cmd):
        if self.transcript is not None:
            self.transcript.write("> %s   [%s]\n" % (cmd, self.rule))
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
            self.stalled = 0
        else:
            self.stalled += 1

        # DRAW THE PLACE WHERE IT STOPS. A run that stalls is the only run worth looking at, and
        # the interesting report is the one at the wall rather than the one at the spawn --
        # which is the only one you get by driving there by hand, because driving there by hand
        # means walking into the same obstacle that stopped the player. Once, so the transcript
        # gets a picture instead of ten thousand identical lines.
        if self.stalled == 60 and not self.mapped:
            self.mapped = True
            return "map 90"

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
            return self.why("break-contact", self.turn_cmd(180 if b >= 0 else -180))

        # FIGHT BACK BEFORE ANYTHING ELSE. Distant guards are a fact of a carriage rather than an
        # emergency, so the plain case is bounded at 900 -- at 2000 something can always see you
        # and the player never gets round to the door two metres away. But once we are actually
        # being hit, whoever can see us IS the emergency however far off they are, because that is
        # exactly where the damage is coming from.
        reach = 2500 if under_fire else 900
        threats = [(d, b) for (d, b) in seen_by if d < reach]
        if threats:
            d, b = threats[0]

            # AN EMPTY CLIP IS A GAP, NOT A DECISION. The game reloads on its own -- measured on
            # Train, the clip went 1/93 to 7/87 with no command -- but the reload takes time, and
            # spending it standing in front of the guard that emptied us is how the health goes.
            # Turn away and put ground between us while it happens; the reserve is what says
            # whether there is any point waiting at all.
            clip = s.get("ammo_clip")
            reserve = s.get("ammo_reserve", 0)
            if clip == 0 and reserve > 0:
                self.queue.append("w 60")
                return self.turn_cmd(150 if b >= 0 else -150)

            # Ninety, not forty-five. A guard behind you is at a bearing near 180, and turning
            # in forty-five degree steps spends four reports with your back to the thing that is
            # shooting -- which is precisely the case where the seconds count.
            if abs(b) > 12:
                return self.why("fight", self.turn_cmd(max(-90, min(90, b))))
            return self.why("fight", "fire")

        # ANYTHING UNDERFOOT GETS PICKED UP FIRST. A collectable fifty units away costs a turn
        # and a step, and on Train it is the keycard a dead guard dropped -- which the door at
        # the end of the carriage wants. Deferring that behind the shooting meant a run spent
        # twenty-four rounds on a brake unit while standing next to the key it needed.
        # BOUNDED. Guards drop a hat as well as whatever they were carrying, and the report's
        # nearest Collectable is sometimes that hat -- reported at the same coordinates, and not
        # something a body can pick up. Left unbounded this rule owned an entire run: 237
        # decisions spent turning towards a hat while the health went to 2%.
        #
        # So try, and then stop trying. If walking over it were going to work it would have
        # worked in a handful of attempts, because that is all picking something up takes.
        for kind in ("Key", "Collectable"):
            item = s.get(kind)
            if item and item[0] < 160 and item[2] == s.get("room"):
                tries = self.grabs.get(kind, 0)
                if tries < 14:
                    self.grabs[kind] = tries + 1
                    if abs(item[1]) > 20:
                        return self.why("pickup-close", self.turn_cmd(max(-60, min(60, item[1]))))
                    return self.why("pickup-close", "w 20")

        # SHOOT WHAT THE MISSION POINTS AT.
        #
        # Objectives on Train are things to destroy, not places to stand: six brake units, tagged
        # 8 through 13. The objective line reports the LAST of them, so a player eighty units
        # from the first one is told its business is a quarter of a mile away and walks past it.
        # The target line names the nearest instead.
        #
        # Budgeted, because nothing in the report says a destroyed prop is destroyed -- the world
        # pack is static and the objective only flips once ALL six are gone. Spend a fixed number
        # of rounds on a target and then leave it alone; if it needed more than that, the run has
        # a bigger problem than this rule.
        tgt = s.get("target")
        if tgt:
            tag, _idx, tdist, tturn, _troom = tgt
            if tdist < 500 and self.shots.get(tag, 0) < 24:
                if abs(tturn) > 12:
                    return self.why("shoot-target", self.turn_cmd(max(-60, min(60, tturn))))
                # AIM DOWN, EVERY SHOT, AND KEEP GOING DOWN UNTIL IT COUNTS.
                #
                # The brake unit is mounted low on the wall beside the door, and firing level
                # puts every round in the panel above it -- the walkthrough says so and two
                # screenshots of the crosshair show it. Pitching once per target was not enough:
                # the view recentres, so one "down" early in a volley is spent long before the
                # rounds that matter.
                #
                # The pitch escalates because the right angle depends on how close we are, and
                # nothing in the report gives the target's height. Start shallow, deepen every
                # few shots, and let the damage reading say when it is right.
                # AIM MODE, NOT A GLANCE DOWN. The C-button look tilts a few degrees and
                # recentres; against a box mounted at knee height eighty units away it never got
                # the crosshair onto the target -- measured, with the object's own damage
                # counter: 559 readings, damage taken zero, across a run that fired two dozen
                # times. "snipe" holds the aim button with the stick down and fires, which is
                # what a person does for a small low target.
                n = self.shots.get(tag, 0)
                self.shots[tag] = n + 1
                return self.why("snipe", "snipe %d" % (10 + (n % 4) * 6))

        # WHAT THE DEAD WERE CARRYING. Guards drop what they hold, and on Train that includes the
        # key a locked door wants. Collecting it is only sensible once nothing is shooting, which
        # is exactly where this sits: every threat rule above has already declined to fire.
        #
        # Same room only. A key four rooms away is a fact about the level, not an errand, and
        # walking at one through three closed doors is how a run ends up back where it started.
        for kind in ("Key", "Collectable"):
            item = s.get(kind)
            if not item:
                continue
            d, bear, room = item
            # 400, not 1200. Something four hundred units off is worth a detour; something twelve
            # hundred away is a separate errand, and treating it as one is how a run ends up
            # walking away from its objective to fetch an item it cannot reach.
            if d > 400 or room != s.get("room"):
                continue
            # Counted here too. Checking a budget that only the other branch increments is not
            # a budget: this rule ran 206 times in a run that was supposed to allow 14.
            if self.grabs.get(kind, 0) >= 14:
                continue
            self.grabs[kind] = self.grabs.get(kind, 0) + 1
            if abs(bear) > 20:
                return self.why("pickup", self.turn_cmd(max(-60, min(60, bear))))
            self.queue.append("w %d" % max(30, min(90, int(d / 12))))
            return self.why("pickup", None)

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

        # FOLLOW GROUND, NOT A BEARING.
        #
        # The objective bearing points through whatever is between here and there, and on Train
        # what is between is a row of crates across the first carriage that no route line
        # mentions, because a crate is scenery rather than a navigation feature. The "path"
        # command searches actual standable ground and hands back the corners; steering at its
        # first corner is the difference between walking round the crates and walking into them.
        #
        # Asked for sparingly. The search is a few thousand standability queries, so it is worth
        # a fresh one when the current route is spent or something is in the way, and not worth
        # one every report.
        path = s.get("path") or []
        self.path_age += 1
        want_path = (not path) or path[0][2] < 140 or self.path_age > 25
        if not path and not self.path_asked:
            self.path_asked = True
            self.path_age = 0
            return "path"

        if path:
            # RECOMPUTED, NOT REMEMBERED. The waypoints are absolute coordinates; the range and
            # bearing that came with them were true at the moment of the search and are wrong the
            # instant the body moves or turns. Steering on the stored bearing makes the same turn
            # every report and never arrives -- measured: "a 15" forty times in a row, the player
            # rotating past the waypoint and back.
            wx, wz = path[0][0], path[0][1]
            px, _, pz = s.get("pos", (0, 0, 0))
            wd = math.hypot(wx - px, wz - pz)
            bearing = math.degrees(math.atan2(wx - px, wz - pz))
            wturn = (bearing - s.get("facing", 0) + 540) % 360 - 180
            if wd < 140:
                # Reached this corner. Drop it, and ask for a fresh search once they run out.
                s["path"] = path[1:]
                if not s["path"] and not self.path_asked:
                    self.path_asked = True
                    self.path_age = 0
                    return "path"
                return None
            if want_path and not self.path_asked:
                self.path_asked = True
                self.path_age = 0
                return "path"
            if abs(wturn) > 25:
                return self.why("follow-path", self.turn_cmd(max(-60, min(60, wturn))))
            return self.why("follow-path", "w %d" % max(30, min(90, int(wd // 12))))

        # WHEN THE FLOOR SEARCH GIVES UP, TRY THE HANDLE.
        #
        # "nothing reachable gets any nearer" is a statement about standable ground, and a closed
        # door has none behind it -- so a carriage divider looks exactly like a dead end to the
        # search. The report's Door line does not help here either: the world pack lists one door
        # on the whole level, 3,825 units away, while the thing 339 units ahead is reported only
        # as "object".
        #
        # Pressing the action button costs a keypress and is what a person does when they walk
        # into something that ought to open. Bounded, because if it were a door it would have
        # opened, and standing at a wall pressing use is the failure this whole file exists to
        # avoid repeating.
        if self.no_path and ahead_d < 420 and self.tried_use < 30:
            self.tried_use += 1
            self.no_path = False
            return self.why("try-door", "use 40")

        # BLOCKED IN FRONT IS NOT THE SAME AS BLOCKED ON THE WAY.
        #
        # "ahead" is a ray along the body's facing, not along the route. Standing at (-263, -37)
        # on tile 226305 facing north into the carriage wall, with the objective 80 degrees to
        # the left, the report says "wall 96 away" and the avoidance rule below takes the
        # clearest turn -- which was +20, further from the objective, so the next report said the
        # same thing and it did it again. Measured: five distinct floor tiles in a two-minute
        # run, health falling the whole time.
        #
        # A wall you are facing but not walking towards is not an obstacle. Turn to the route
        # first; then, pointed the right way, the sensor is answering the question that matters
        # and the avoidance below can be believed.
        if abs(obj_turn) > 30:
            return self.turn_cmd(max(-60, min(60, obj_turn)))

        # Something solid close enough to walk into: take the turn the report worked out, and
        # then WALK IT. A turn on its own leaves the player pointing at open ground and standing
        # still, so the next report sees the same obstacle and turns again -- which is how it
        # spent a whole run rotating beside one crate. Turn and go, as one plan.
        if ("wall" in what or "object" in what) and ahead_d < 250:
            clear = s.get("clearest", 0)
            room = s.get("clear_room", 0)
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
            for k in ("ahead_what", "ahead_dist", "clearest", "clear_room", "near", "enemies",
                      "door", "Key", "Collectable", "target"):
                # "path" survives on purpose: it is a plan, not an observation
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
            # NOT "room". The ahead line's last figure is how much CLEARANCE the turn buys, in
            # units; the player's actual room number comes from the you line. Storing both under
            # "room" meant every same-room test compared 5 against 400 and failed, so the player
            # never once picked up a key or a collectable -- including the keycard lying fifty
            # units away that the door at the end of the carriage wants.
            self.state["clear_room"] = int(m.group(4))
            return
        m = RE_TARGET.match(line)
        if m:
            self.state["target"] = tuple(int(g) for g in m.groups())
            return
        m = RE_PATH.match(line)
        if m:
            if int(m.group(1)) == 1:
                self.state["path"] = []
            self.state.setdefault("path", []).append(
                (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))
            self.path_asked = False
            return
        if RE_NOPATH.match(line):
            self.state["path"] = []
            self.path_asked = False
            self.no_path = True
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
            tail = (m.group(7) or "").strip()
            self.state.setdefault("enemies", []).append({
                "id":    int(m.group(1)) if m.group(1) is not None else -1,
                "dist":  int(m.group(2)),
                "turn":  int(m.group(3)),
                "hp":    int(m.group(4)) if m.group(4) is not None else None,
                "alert": int(m.group(6)) if m.group(6) is not None else None,
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
            # Now acted on: guards drop what they carry, and on Train that includes the key a
            # locked door wants. The room is kept alongside the bearing because a key in another
            # room is a fact about the level rather than an errand -- see decide().
            kind = m.group(1)
            self.state[kind] = (int(m.group(3)), int(m.group(4)),
                                int(m.group(5)) if m.group(5) is not None else -1)



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
