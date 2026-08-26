#!/usr/bin/env python3
"""Executable specification for the lockstep session in getv/port/src/ge_net.c.

Why this exists

Lockstep has exactly one correctness property: every machine must apply the SAME inputs for the
same tick. If that holds, the simulations agree. If it does not, they diverge silently -- players
end up standing in different places on different screens with nothing reporting an error, which
is the single worst failure mode in netplay because there is nothing to see until it is far too
late.

That property is a statement about the session algorithm, not about C. So it is modelled here
and tested against latency, jitter and packet loss, which is not something a compiler would have
told us anyway. ge_net.c implements this algorithm; if the two disagree, one of them is wrong and
this file says which behaviour was intended.

Run:  python3 tools/netsim.py
Exits non-zero if any invariant fails.
"""

import random
import sys

RING = 64
DEFAULT_DELAY = 3


# How many past inputs each datagram carries alongside the current one.
#
# Lockstep does not want retransmit-with-acknowledgements: a round trip to recover a lost input
# costs more than the input delay it was meant to fit inside, so the recovery arrives too late to
# be useful. Redundancy is the standard answer instead -- every packet repeats the last K inputs,
# so a single loss is covered by the very next packet with no round trip at all. Inputs are tiny,
# which is what makes paying for them repeatedly affordable.
DEFAULT_REDUNDANCY = 8


class Machine:
    """One participant. Mirrors ge_net.c: a per-slot ring, publish-with-delay, stall until every
    acting slot has input for the tick about to run."""

    def __init__(self, local_slot, slots, delay):
        self.local = local_slot
        self.slots = dict(slots)          # slot -> "local" | "remote" | "bot" | "empty"
        self.delay = delay
        self.tick = 0
        self.ring = {s: {} for s in slots}
        self.applied = {}                 # tick -> {slot: input}
        self.stalls = 0
        self.late = 0
        self.dups = 0                     # redundant copies of inputs already held
        self.published = {}               # our own slot: tick -> value, for redundant resends

        # Prime the pipeline. Input is only ever published for tick+delay, so the first `delay`
        # ticks would never receive input from anybody -- including from this machine itself --
        # and the session would deadlock before it started. Those ticks are seeded with neutral
        # input on every machine, which is safe precisely because every machine seeds them
        # identically: agreement is preserved because nobody had a choice about them.
        for t in range(delay):
            for slot in slots:
                self.store(slot, t, 0)

    def store(self, slot, tick, value):
        # The C keeps a fixed ring and rejects an entry whose tick no longer matches the slot,
        # which is how it notices the ring wrapped. Modelled the same way.
        self.ring[slot][tick % RING] = (tick, value)

    def get(self, slot, tick):
        entry = self.ring[slot].get(tick % RING)
        if entry is None or entry[0] != tick:
            return None
        return entry[1]

    def deliver(self, slot, tick, value):
        # A redundant copy of something already held is not late, it is the scheme working.
        # Counting it as late would swamp the signal the late counter exists to give -- with a
        # window of 8 it reports tens of thousands of "late" inputs on a perfect link, which
        # would read as a link problem when nothing is wrong at all.
        if self.get(slot, tick) is not None:
            self.dups += 1
            return

        # Strictly less than: input for the tick ABOUT TO RUN is still usable, because that tick
        # has not been simulated yet. Rejecting it (tick <= self.tick) throws away every input
        # that arrives exactly on time, which silently caps the session at the primed window and
        # then stalls forever on any link where latency reaches the delay.
        if tick < self.tick:
            # The tick it belonged to has already run; unusable, and counted so a rising number
            # says "the delay is too low for this link" rather than nothing at all.
            self.late += 1
            return
        self.store(slot, tick, value)

    def begin(self, local_input):
        """Returns True if the tick can run, False to stall."""
        future = self.tick + self.delay
        self.store(self.local, future, local_input)
        self.published[future] = local_input

        for slot, kind in self.slots.items():
            if kind in ("empty", "bot"):
                continue
            if self.get(slot, self.tick) is None:
                self.stalls += 1
                return False, future

        frame = {}
        for slot, kind in self.slots.items():
            if kind == "empty":
                continue
            v = self.get(slot, self.tick)
            if v is not None:
                frame[slot] = v
        self.applied[self.tick] = frame
        self.tick += 1
        return True, future


def run(n_players, delay, latency, jitter, loss, steps, seed, bots=(),
        redundancy=DEFAULT_REDUNDANCY):
    rng = random.Random(seed)
    slots = {}
    for s in range(4):
        if s < n_players:
            slots[s] = "remote"
        elif s in bots:
            slots[s] = "bot"
        else:
            slots[s] = "empty"

    machines = []
    for i in range(n_players):
        cfg = dict(slots)
        cfg[i] = "local"
        machines.append(Machine(i, cfg, delay))

    inflight = []      # (arrive_step, target_machine_index, slot, tick, value)

    for step in range(steps):
        # deliver
        due = [m for m in inflight if m[0] <= step]
        inflight = [m for m in inflight if m[0] > step]
        for _, target, slot, window in due:
            for tick, value in window:
                machines[target].deliver(slot, tick, value)

        for i, m in enumerate(machines):
            # A deterministic per-(slot,tick) input so any disagreement between machines is a
            # real disagreement rather than two machines rolling different dice.
            local_input = (i * 7919 + m.tick * 104729) & 0xFFFF
            ok, future = m.begin(local_input)
            # Publish even when stalled. ge_net.c stores and sends the local input before it
            # checks readiness, and that ordering matters: if a stalled machine stopped
            # publishing, every machine waiting on a peer would go quiet and the session would
            # deadlock permanently the first time anything arrived late.
            # One datagram carrying the last `redundancy` inputs. A lost packet is covered by the
            # next one, with no acknowledgement and no round trip.
            window = [(t, m.published[t])
                      for t in range(max(0, future - redundancy + 1), future + 1)
                      if t in m.published]
            for j in range(len(machines)):
                if j == i:
                    continue
                if rng.random() < loss:
                    continue
                arrive = step + max(1, int(rng.gauss(latency, jitter)))
                inflight.append((arrive, j, i, window))

    return machines


def check(name, machines, expect_progress=True):
    """The invariant: for every tick both machines simulated, they applied identical inputs."""
    problems = []
    common = None
    for m in machines:
        ticks = set(m.applied)
        common = ticks if common is None else (common & ticks)
    common = common or set()

    base = machines[0]
    for t in sorted(common):
        for m in machines[1:]:
            if m.applied[t] != base.applied[t]:
                problems.append("tick %d: %s applied %r, %s applied %r"
                                % (t, "m0", base.applied[t], "m%d" % machines.index(m),
                                   m.applied[t]))
                break

    progressed = len(common)
    stalls = sum(m.stalls for m in machines)
    late = sum(m.late for m in machines)
    dups = sum(m.dups for m in machines)

    ok = not problems and (progressed > 0 or not expect_progress)
    print("  %-44s %-5s agreed=%-5d stalls=%-5d late=%-5d dup=%d"
          % (name, "OK" if ok else "FAIL", progressed, stalls, late, dups))
    for p in problems[:3]:
        print("      %s" % p)
    return ok


class Node:
    """A machine during session establishment, mirroring ge_net_udp.c.

    The tick protocol assumes a full mesh -- every machine sending to every other. That
    assumption is only true if the handshake actually builds one, which is a separate problem
    from the one the tick model tests, and the place the transport originally got it wrong: a
    star leaves joiners with no path to each other, so every tick stalls on a peer they cannot
    hear.
    """

    def __init__(self, addr, is_host, want=0):
        self.addr = addr
        self.is_host = is_host
        self.want = want
        self.slot = 0 if is_host else None
        self.peers = {}            # addr -> slot, never including ourselves
        self.started = False
        self.greeted = set()

    def remember(self, addr, slot):
        if addr == self.addr:
            return None            # never ourselves
        if addr in self.peers:
            return self.peers[addr]
        if slot is None:
            slot = 1
            taken = set(self.peers.values()) | {self.slot}
            while slot in taken:
                slot += 1
        self.peers[addr] = slot
        return slot

    def table_for(self, target_addr):
        """Host's view, built per recipient: everyone except the recipient, host included."""
        entries = [(self.slot, self.addr)]
        for a, s in self.peers.items():
            if a != target_addr:
                entries.append((s, a))
        return entries


def run_handshake(n_players, loss, steps, seed, join_delay=5):
    rng = random.Random(seed)
    host = Node("host", True, want=n_players)
    joiners = [Node("j%d" % i, False) for i in range(1, n_players)]
    nodes = {n.addr: n for n in [host] + joiners}
    wire = []      # (arrive_step, to_addr, kind, payload, from_addr)

    def send(frm, to, kind, payload=None):
        if rng.random() < loss:
            return
        wire.append((len(wire) * 0 + step + 2, to, kind, payload, frm))

    for step in range(steps):
        due = [m for m in wire if m[0] <= step]
        wire[:] = [m for m in wire if m[0] > step]

        for _, to, kind, payload, frm in due:
            n = nodes[to]
            if kind == "HELLO":
                n.remember(frm, None)
            elif kind == "JOIN" and n.is_host:
                slot = n.remember(frm, None)
                players = len(n.peers) + 1
                start = players >= n.want
                # Re-send to EVERYONE: earlier joiners must learn about later ones.
                for a in list(n.peers):
                    send(n.addr, a, "PEERS", (n.table_for(a), n.peers[a], start))
                if start:
                    n.started = True
            elif kind == "PEERS" and not n.is_host:
                entries, my_slot, start = payload
                n.slot = my_slot
                for s, a in entries:
                    if s == my_slot:
                        continue
                    n.remember(a, s)
                for a in n.peers:
                    if a not in n.greeted:
                        send(n.addr, a, "HELLO")
                        n.greeted.add(a)
                if start:
                    n.started = True

        # joiners retry JOIN until they are in a session
        for j in joiners:
            if not j.started and step % join_delay == 0:
                send(j.addr, "host", "JOIN")

    return host, joiners


def check_handshake(name, n_players, loss, seed):
    host, joiners = run_handshake(n_players, loss, 400, seed)
    nodes = [host] + joiners
    problems = []

    if not all(n.started for n in nodes):
        problems.append("not all machines started: %s"
                        % [n.addr for n in nodes if not n.started])

    # Full mesh: everyone knows everyone else, and nobody knows themselves.
    for n in nodes:
        if n.addr in n.peers:
            problems.append("%s has itself as a peer" % n.addr)
        if len(n.peers) != n_players - 1:
            problems.append("%s knows %d peers, needs %d"
                            % (n.addr, len(n.peers), n_players - 1))

    # Everyone must agree who holds which slot, or lockstep compares the wrong players.
    view = {}
    for n in nodes:
        view[n.addr] = n.slot
    for n in nodes:
        for a, s in n.peers.items():
            if a in view and view[a] != s:
                problems.append("%s thinks %s is slot %d, %s says %d"
                                % (n.addr, a, s, a, view[a]))

    ok = not problems
    print("  %-44s %-5s mesh=%s slots=%s"
          % (name, "OK" if ok else "FAIL",
             [len(n.peers) for n in nodes], [n.slot for n in nodes]))
    for p in problems[:3]:
        print("      %s" % p)
    return ok


def run_drop(mode, n_players=3, dead=2, die_at=40, detect_after=(6, 20), steps=200,
             delay=DEFAULT_DELAY):
    """A peer vanishes mid-session. Do the survivors still agree?

    This is subtler than it looks. Machines notice a departure at DIFFERENT moments -- timeouts
    fire independently, and a machine that was already stalling notices later than one that was
    not. If each survivor drops the slot the instant IT notices, they disagree about every tick
    between the two detections, and handling the disconnect causes exactly the divergence it was
    meant to prevent.

    mode "immediate": drop the slot on local detection. Expected to diverge.
    mode "agreed":    the lowest surviving slot proposes a drop tick, everyone applies it there.
    """
    slots = {s: ("remote" if s < n_players else "empty") for s in range(4)}
    machines = []
    for i in range(n_players):
        cfg = dict(slots)
        cfg[i] = "local"
        machines.append(Machine(i, cfg, delay))

    alive = [i for i in range(n_players) if i != dead]
    proposer = min(alive)
    inflight = []
    drop_msgs = []

    for step in range(steps):
        for _, target, slot, window in [x for x in inflight if x[0] <= step]:
            for tick, value in window:
                machines[target].deliver(slot, tick, value)
        inflight = [x for x in inflight if x[0] > step]

        for _, target, slot, at_tick in [x for x in drop_msgs if x[0] <= step]:
            machines[target].pending_drop = (slot, at_tick)
        drop_msgs = [x for x in drop_msgs if x[0] > step]

        running = list(range(n_players)) if step < die_at else alive

        for i in running:
            m = machines[i]

            # An agreed drop takes effect at ITS tick, identically on every machine.
            pd = getattr(m, "pending_drop", None)
            if pd is not None and m.tick >= pd[1]:
                m.slots[pd[0]] = "empty"
                m.pending_drop = None

            if i in alive:
                idx = alive.index(i)
                detect_step = die_at + detect_after[idx % len(detect_after)]

                # RELAY: on noticing the departure, hand every other survivor the dead peer's
                # inputs we still hold. Nobody can have SIMULATED past the highest tick anybody
                # HOLDS, so once the survivors pool what they have, every one of them can reach
                # the same tick -- which is what makes a common drop tick reachable rather than
                # a deadlock. Bounded: it happens once, over the redundancy window.
                if mode == "relay" and step == detect_step:
                    # The whole ring, not the ticks ahead of us. What the other survivor is
                    # missing is precisely what we have ALREADY APPLIED, which sits below our
                    # current tick -- scanning forward finds nothing and relays an empty set.
                    held = [entry for entry in m.ring[dead].values() if entry is not None]
                    for j in alive:
                        if j != i:
                            inflight.append((step + 1, j, dead, held))
                    m._relayed = True

                if mode == "immediate" and step == detect_step:
                    m.slots[dead] = "empty"
                elif mode == "relay" and i == proposer and step == detect_step + 4:
                    # After the relays have landed, the highest tick anyone holds is the same
                    # everywhere, so the drop tick is both agreed and reachable.
                    # Highest tick anyone holds for the dead slot, read off the whole ring after
                    # the relays have pooled everything.
                    # Purely the highest tick HELD, never seeded with the current tick. Seeding
                    # it puts the drop one tick beyond reach: a machine stalled at tick T would
                    # be told to drop at T+1, which it can only get to by simulating T, which
                    # needs the very input nobody has.
                    top = -1
                    for tk, _v in m.ring[dead].values():
                        if tk > top:
                            top = tk
                    at = top + 1
                    m.pending_drop = (dead, at)
                    for j in alive:
                        if j != i:
                            drop_msgs.append((step + 2, j, dead, at))
                elif mode == "agreed" and i == proposer and step == detect_step:
                    # Only the lowest surviving slot may propose, so two machines cannot
                    # announce conflicting drop ticks for the same peer.
                    at = m.tick + delay + 2
                    m.pending_drop = (dead, at)
                    for j in alive:
                        if j != i:
                            drop_msgs.append((step + 2, j, dead, at))

            local_input = (i * 7919 + m.tick * 104729) & 0xFFFF
            ok, future = m.begin(local_input)

            window = [(t, m.published[t])
                      for t in range(max(0, future - DEFAULT_REDUNDANCY + 1), future + 1)
                      if t in m.published]
            for j in range(n_players):
                if j == i:
                    continue
                if j == dead and step >= die_at:
                    continue        # it is gone; nobody can reach it any more
                # The asymmetry that causes the divergence. A departing machine's final packets
                # do not stop everywhere at once: they reach some survivors and not others. So
                # one survivor holds the dead peer's input for a few more ticks than the other
                # does, and if each drops the slot when IT notices, they simulate those ticks
                # from different input sets.
                if i == dead and step >= die_at - 2 and j != alive[0]:
                    continue
                inflight.append((step + 1, j, i, window))

    return [machines[i] for i in alive]


def check_drop(name, mode, expect_agree, min_ticks=0, note=""):
    ms = run_drop(mode)
    common = None
    for m in ms:
        t = set(m.applied)
        common = t if common is None else (common & t)
    common = common or set()

    disagreements = 0
    base = ms[0]
    for t in sorted(common):
        for m in ms[1:]:
            if m.applied[t] != base.applied[t]:
                disagreements += 1
                break

    agreed = disagreements == 0
    ok = (agreed == expect_agree) and len(common) >= min_ticks
    tail = note
    if not tail and not expect_agree:
        tail = "(divergence expected, and found)"
    print("  %-44s %-5s ticks=%-5d disagreeing=%-4d %s"
          % (name, "OK" if ok else "FAIL", len(common), disagreements, tail))
    return ok


def main():
    print("lockstep session model -- invariant: identical inputs applied per tick\n")
    allok = True

    # Ideal link: delivery inside the delay window, nothing lost.
    allok &= check("2 players, latency 1, no loss",
                   run(2, DEFAULT_DELAY, 1, 0.0, 0.0, 400, seed=1))
    allok &= check("4 players, latency 1, no loss",
                   run(4, DEFAULT_DELAY, 1, 0.0, 0.0, 400, seed=2))

    # Latency at the edge of the delay window, then past it. Past it the machines must STALL,
    # never disagree -- a stall is a playability problem, a disagreement is a correctness one.
    allok &= check("4 players, latency 3 (== delay)",
                   run(4, DEFAULT_DELAY, 3, 0.0, 0.0, 400, seed=3))
    allok &= check("4 players, latency 8 (> delay, must stall not diverge)",
                   run(4, DEFAULT_DELAY, 8, 1.0, 0.0, 400, seed=4))

    # Jitter and loss. Loss is the interesting one: this protocol has no retransmit, so a lost
    # input stalls the tick forever. That is the correct SAFE behaviour and the test asserts
    # agreement, not progress -- see the note printed below.
    allok &= check("4 players, jitter 2",
                   run(4, 6, 3, 2.0, 0.0, 400, seed=5))
    # Loss, with and without redundancy. This pair is the whole argument for the scheme: with a
    # single input per packet a lost datagram stalls the session forever, and with a window of
    # past inputs the very next packet covers it.
    allok &= check("4 players, 2% loss, NO redundancy (stalls)",
                   run(4, 6, 2, 1.0, 0.02, 400, seed=6, redundancy=1),
                   expect_progress=False)
    allok &= check("4 players, 2% loss, redundancy 8",
                   run(4, 6, 2, 1.0, 0.02, 400, seed=6))
    allok &= check("4 players, 20% loss, redundancy 8",
                   run(4, 6, 2, 1.0, 0.20, 400, seed=9))
    allok &= check("4 players, 40% loss, redundancy 8",
                   run(4, 8, 3, 1.0, 0.40, 400, seed=10))

    # Bots occupy slots without networking, and must not make machines disagree.
    allok &= check("2 players + 2 bot slots",
                   run(2, DEFAULT_DELAY, 1, 0.0, 0.0, 400, seed=7, bots=(2, 3)))

    # Larger delay buys tolerance of a worse link, which is the tuning knob the late counter is
    # meant to inform.
    allok &= check("4 players, latency 8, delay 12",
                   run(4, 12, 8, 2.0, 0.0, 400, seed=8))

    # Disconnection. A peer vanishing is not a transport problem: survivors notice at different
    # moments, and dropping the slot on local detection makes the SURVIVORS disagree.
    print("\ndisconnection -- a peer vanishes; do the survivors still agree?\n")
    allok &= check_drop("drop on local detection", "immediate", expect_agree=False)
    allok &= check_drop("drop at an agreed tick (stalls)", "agreed", expect_agree=True,
                        note="agrees only by stopping -- see note below")
    allok &= check_drop("relay held inputs, then drop", "relay", expect_agree=True,
                        min_ticks=150, note="agrees AND keeps running")
    print("\n  Read those three rows together; they are the whole argument.")
    print("  Dropping on local detection keeps running and DIVERGES: the departing machine's")
    print("  last packets reach one survivor and not the other, so they simulate a few ticks")
    print("  from different input sets. Agreeing a drop tick fixes the divergence and stalls,")
    print("  because a machine waiting on the departed peer cannot reach a tick in its own")
    print("  future. Relaying first fixes both: nobody can have SIMULATED past the highest tick")
    print("  anybody HOLDS, so once survivors pool what they have they all reach the same tick,")
    print("  and a drop set at exactly (highest held + 1) is reachable by every one of them.")
    print("  That +1 is exact. Setting it from the current tick instead puts it one beyond")
    print("  reach and the session stalls at 43 ticks looking like it agreed.")

    # The tick protocol assumes a full mesh. These check the handshake actually builds one --
    # a separate problem, and the place the transport originally got it wrong.
    print("\nsession establishment -- invariants: full mesh, agreed slots, everyone starts\n")
    allok &= check_handshake("2 players, no loss", 2, 0.0, seed=20)
    allok &= check_handshake("3 players, no loss", 3, 0.0, seed=21)
    allok &= check_handshake("4 players, no loss", 4, 0.0, seed=22)
    allok &= check_handshake("4 players, 10% loss", 4, 0.10, seed=23)
    allok &= check_handshake("4 players, 30% loss", 4, 0.30, seed=24)

    print("\nredundancy is what makes loss survivable, and the pair above is the argument:")
    print("  2% loss with one input per packet stalls the session permanently; the same link")
    print("  with a window of 8 runs clean. No acknowledgements and no round trip -- a lost")
    print("  packet is simply covered by the next one, which is why this beats retransmit here:")
    print("  a round trip to recover an input costs more than the delay it had to fit inside.")
    print("\nlate vs dup: 'late' is an input that was needed and arrived too late, and a rising")
    print("  count means the delay is too low for the link. 'dup' is the redundancy doing its")
    print("  job. Conflating them makes a healthy link look broken.")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
