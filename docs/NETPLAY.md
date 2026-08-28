# Netplay

The goal: four or more people, each on their own machine, each full-screen, playing each other
over a WAN -- plus bots, in any mix, including nobody at all and two bots playing while you
watch.

## The seam already exists

`ge_player_api` posts controller input per slot per tick, and **refuses a post for a tick that
has already run**. That refusal was written for bots, but it is exactly the netplay failure it
resembles. Which means a bot, a remote player and an RL agent are the same kind of thing: something
that supplies input for a slot before its tick runs.

That is the whole architecture. One seam serves:

| want | how |
| --- | --- |
| WAN player on another machine | their input arrives over the wire |
| co-op with a bot | bot policy fills the second slot |
| watching two bots play | policies fill both slots, camera follows one |
| RL agent training | agent posts into a slot |

Nothing about the game distinguishes them, and that is deliberate.

## Lockstep, not state replication

Only inputs travel; every machine simulates the same thing from them. A tick of input is twelve
bytes. The world state is not something we could ship at 60Hz over a domestic link.

The cost is that lockstep is unforgiving: **every machine must simulate identically**, and one
that does not diverges silently, with players gradually standing in different places on
different screens and nothing reporting an error. `gePlayerSeedFingerprint()` already exists, so
`ge_net.c` exchanges it once a second and prints a real message when two machines disagree.
Catching divergence at the moment it starts is the difference between a bug you can find and one
you cannot.

## Input delay, not rollback

Every machine acts on input captured a few ticks ago, which is what buys the network time to
deliver it. Default is 3 ticks -- 50ms at 60Hz, which covers most domestic links.

Rollback hides more latency and would need full save/restore of game state on every mispredict.
That is a far larger change, and not worth reaching for before measuring. The `inputs_late`
counter is there to make that measurable: a rising late count means the delay is too low for the
link, which is a tunable rather than a bug.

## Where the four-slot cap bites

The game has exactly four player slots. "Four players **and** four bots" is not reachable through
the input seam -- the slots are full.

That is what `ge_bot_ai.c` is for. Those bots are **characters**, not players: AI-list bytecode
driving a `chr`, the same entity every campaign guard is. The two paths compose -- four humans in
the slots, plus AI-driven characters beyond them -- and they are genuinely different mechanisms
rather than two ways of doing one thing. See `docs/BOTS.md`.

A bot slot is treated as ready without waiting for the network, because it is simulated
identically on every machine. **That is only true while bot policy stays deterministic**, which
is a real constraint on anything added to `ge_bot.c`: no wall-clock, no unsynchronised RNG, no
frame-rate-dependent decisions.

## What is built

- `ge_net.h` / `ge_net.c` -- the session: input ring per slot, publish-with-delay, stall until
  every acting slot has input for the tick, fingerprint exchange, and counters for stalls, late
  inputs and desyncs.
- The transport is deliberately **not** in there. Knowing when a tick is ready, when to stall and
  when the machines have diverged is not a socket concern, and keeping it out means the hard part
  is testable with no I/O at all -- `geNetDeliver()` takes a datagram directly.

- `ge_net_udp.c` -- the socket half, plus the handshake that has to happen first: a host binds a
  port and assigns slots, joiners send JOIN until an ASSIGN lands (a single join packet is exactly
  the thing UDP loses). Handshake traffic is consumed in the transport so the session layer never
  learns a session had to be negotiated.

### Session-relative tick numbering

Everything on the wire counts from **zero at session open**, not from `gePlayerTick()`.

This was a real flaw, caught while writing the transport. The game tick is per-machine: two
players who joined a minute apart are thousands of ticks apart, so "tick 900" would mean
different moments on each machine and the session would compare inputs that were never meant to
line up. It would have presented as a desync while being purely a setup bug -- the worst kind to
debug, because the fingerprint check would fire and point at the simulation rather than at the
handshake.

Each machine records where its own game clock stood at session open and converts at the boundary.
Nothing else in either file needs to know.

## What is not built

1. **Full-screen per machine.** GoldenEye multiplayer is split-screen: one machine, N viewports,
   all players local. Rendering a single viewport is the easy half -- the renderer already does
   per-player viewports. The real work is that game logic assumes every player is local.
2. **Cross-architecture float agreement** -- see the audit below. This is the one real
   determinism risk left, and it is not something static analysis can settle.
3. **A live discovery source.** `ge_discovery.c` exists and parses a session description into
   slots and endpoints, but the only source is the static one. A Nakama source is a small adapter
   returning 0 while its request is in flight -- see below.

Item 1 is now the largest, and decides whether this is playable rather than merely synchronised.

### Item 1, mapped precisely rather than estimated

The architecture was walked end to end before writing anything, because the failure mode here
is silent divergence, not a crash -- the same discipline the float-determinism audit below
already uses. Three findings, in the order they change the shape of the work:

**`geNetTickBegin()` was not called from anywhere. It is now.** This section is kept because
the finding is what shaped the work: the transport could complete a handshake and assign slots,
but nothing drove it tick by tick, and `gePortNetInit()`/`gePortNetPoll()` were referenced only
in a launcher comment. `0011-netplay-tick-integration.patch` closes that: `gePortNetInit` runs
at boot and `gePortNetTick` gates the retrace handler's tick and render pass. Verified with two
real processes over UDP -- real handshake, real synchronised input exchange, clean shutdown.

Two things came out of wiring it, both worth knowing before trusting a session:

- A joiner's own local slot was left `GE_SLOT_HARDWARE`, which reads `joyGetButtons()` indexed
  by slot, so slot 1 read an empty physical port locally while port 0's real input went out on
  the wire. That is a guaranteed desync the instant anyone presses anything. Fixed by claiming
  the local slot as `GE_SLOT_INJECTED` too.
- A multi-million-line runaway in `gePortNetPoll`'s drain loop was hit once and never
  reproduced. A 256-packet cap bounds the consequence; it is not a fix for whatever caused it.

**It is wired, not finished.** Roughly half of automated no-human-input trials still desync:
host fingerprint clean, joiner disagrees, nothing pressed on either side. Same machine, same
binary, so this is not the cross-architecture float risk the determinism audit flags as its one
open item. Do not describe network play as working. `geNetLocalSlot()` has
been added (`ge_net.c`/`ge_net.h`) since nothing exposed which slot is this machine's own; every
other piece of "am I local" plumbing already threads slot numbers through correctly (verified by
reading `geNetTickBegin`'s publish/drain/post sequence), so this was the one real gap in that
part.

**The render loop and the world-simulation tick are one interleaved loop, not two.**
`lv.c:769-937`, the loop `getPlayerCount()` bounds for split-screen, does not separate cleanly
into "render calls" and "sim calls" by position -- they alternate. `propsTick()` (`lv.c:825`)
ticks the whole active-prop list and already tolerates being called more than once per frame (it
gates its once-only bookkeeping on `get_player_position_in_shuffled(get_cur_playernum()) == 0`,
`chrprop.c:2553`), but `propsTickPlayer()` (`lv.c:845`) has no such guard and does genuinely
per-player work -- `objTickPlayer`/`weaponTickPlayer` pickup and collision checks
(`chrprop.c:2820-2859`) that must run once per **real** player, not once per **locally rendered**
viewport. So a full-screen-per-machine implementation cannot simply bound this loop to 1 -- doing
that would silently stop checking pickups/collisions for the three players this machine doesn't
render, and their state would diverge from what their own machines compute. The reload-input
handling at `lv.c:839-843` and the infinite-ammo cheat at `lv.c:911-925` are the same shape:
per-real-player simulation state sitting between render calls in the same loop body. Rendering
one full-screen viewport per machine is correct; simulating one player's worth of world per frame
is not, and the two currently can't be separated by changing a single loop bound.

**A working stall actually requires gating a whole game frame, not just an input queue.**
`geNetTickBegin()` already does the right thing when a peer's input hasn't arrived -- it returns
0 without posting anything (`ge_net.c:496-499`), and the doc above is exact about why that
ordering matters. But `ge_playback()` (`ge_player_api.c:258`, the per-tick playback hook) runs
unconditionally every frame regardless of what it's told, and increments its own tick counter
(`ge_player_api.c:318`) either way. So "stall" only works if *nothing downstream runs that
frame* -- and the actual per-frame boundary that would need to gate is
`vendor/ge-decomp/src/boss.c:576-937`, the `OS_SC_RETRACE_MSG` block that calls
`joyConsumeSamplesWrapper()` (`boss.c:594`) and then the entire per-player tick/render loop. That
block also drives audio timing, animation and `waitForNextFrame()`'s own pacing, none of which
were written with "sometimes skip this frame" in mind. This is a correctness requirement, not an
optimisation -- without it, `inputs_late`/stalling exists in the API but does not actually
protect the simulation from running ahead on stale input.

None of this is a rejection of item 1 -- it is what makes it item 1. The tick-glue and the
loop-splitting are both well-contained (a handful of functions, one file each), but the frame-
stall gate touches the core retrace loop that every subsystem's timing assumes runs every call,
and getting it wrong produces exactly the silent, un-crashing divergence this document spends the
most words warning about. It also cannot be verified the way most of this port's other work can
-- a single machine has no peer to stall against, so confidence here needs either a second
machine running the same session, or a `netsim.py`-style model of the frame-gate specifically
(the existing one models the transport, not this). Held for a dedicated pass rather than folded
into an already-long session.

### Mesh topology -- already built, in the transports, and this document said otherwise

The "not built" entry above was **stale**, and badly so. Both transports build a full mesh already:

- `ge_net_udp.c` -- `GE_NET_MSG_PEERS`, `ge_udp_send_table`. Its header comment has said
  **"full mesh, not A star"** the whole time.
- `ge_net_enet.c` -- `GE_MSG_PEERS`, `ge_en_send_table`, `ge_en_handle_peers`, including a
  *"only the lower slot dials"* rule so a pair does not end up with two connections.

Anyone reading this file and believing it would build a second one. That is what happened: the
session-level roster below was written against this document rather than against the code.

### The session-level roster -- written, then removed

A `GE_NET_MSG_ASSIGN` roster was added at the session layer, with opaque addresses and a `connect`
callback on `GeNetTransport`. It has been **reverted**, and the reasoning is worth keeping so
nobody adds it again from this document.

It duplicated the transports for the mesh purpose and had no consumer: neither transport supplied a
`connect` callback, which was the only reason the two mechanisms never fought. The one thing it
appeared to add -- a bot slot -- turned out to exist already, since `GE_NET_SLOT_BOT` has been in
`GeNetSlotKind` all along.

Two ways to do one job is what this file elsewhere calls worse than none, and dead machinery that
looks live is worse than absent. It also very nearly shipped a crash: both transports fill
`GeNetTransport` field by field from an uninitialised local, so every field the struct gained was
stack garbage, and `ge_net.c` calls the optional callbacks it finds non-NULL. Both now `memset`
first, which is the one part of that work worth keeping.

Three decisions in it are worth not undoing:

**Addresses are opaque to the session layer.** A `GeNetPeerInfo` carries a length and a blob that
`ge_net.c` never parses. An IPv4 `sockaddr`, an IPv6 one and an `ENetAddress` are different shapes,
and a session that knew the difference would need changing to gain a transport -- which is the same
reason `GeNetTransport` exists at all.

**One roster is broadcast verbatim, and "local" is patched on arrival.** The roster says `REMOTE`
for every slot *including the one receiving it*. Applying that literally makes every machine mark
**itself** remote and stall forever waiting for input it is supposed to be producing. Whichever
slot is ours is forced `LOCAL` in `ge_net_apply_roster`. Sending a tailored roster per machine
would remove the special case and cost more: N messages to keep consistent, and a class of bug
where two machines disagree because they were told different things.

**A failed dial is named, not fatal.** Half a mesh still plays -- the slots that connected run, and
the one that did not appears as a stall on a numbered slot, which is findable. Aborting would turn
one bad link into no game and no explanation.

`getv/port/tests/test_net_roster.c` specifies all of it with no sockets, driving the session
through `geNetDeliver`. 34 assertions, including that a joiner's own slot survives as `LOCAL`, that
a repeated roster does not redial, that bots are never dialled, and that a truncated or absurd
`ASSIGN` is refused rather than clamped.

## What to adopt rather than write

Most of this problem is solved. What follows is what was checked, on what terms, and what it
should and should not be used for.

| project | licence | what it does | verdict |
| --- | --- | --- | --- |
| [ENet](https://github.com/lsalzman/enet) | MIT | reliable UDP in pure C | **adopt** -- replaces our transport |
| [GGPO](https://github.com/pond3r/ggpo) | MIT | rollback netcode | later, only if input delay proves insufficient |
| [Nakama](https://github.com/heroiclabs/nakama) | Apache 2.0 | lobbies, matchmaking, accounts, chat | **adopt for discovery** |
| [Colyseus](https://docs.colyseus.io/) | MIT | authoritative room server (Node) | lighter alternative to Nakama |

**ENet replaces `ge_net_udp.c`.** It has done connection setup and teardown, timeouts,
sequencing, fragmentation and per-channel reliability since 2002. None of that is where this
project's interesting problems are, and every line of hand-rolled equivalent is a line that can
be subtly wrong on a link nobody tested. `tools/fetch_enet.sh` fetches it pinned; it is
gitignored like every other third-party dependency here.

**ENet does not replace `ge_net.c`.** Deciding when a tick is ready, when to stall, and when the
machines have diverged is lockstep logic, not transport. That stays ours, and the model in
`tools/netsim.py` is its specification.

### The lobby server, and the line not to cross

A lobby server is exactly the kind of thing not to write: accounts, friends, lobbies,
matchmaking, presence and reconnection are a large amount of unglamorous work that Nakama already
does, self-hosted, under Apache 2.0.

But it should be used **only to help machines find each other**. Gameplay stays peer-to-peer.
Routing sixty-hertz input through a server adds a hop to the one thing that must arrive inside
the input delay, and turns every player's latency into the sum of two links instead of one. The
division is:

- **server** -- who is playing, which stage, which slot, and each other's addresses
- **peers** -- every tick of input, directly, over ENet

That also keeps the server off the critical path entirely: if it goes down mid-match, the match
carries on, because nothing in a running session depends on it.

**GGPO is deliberately not adopted yet.** Rollback hides more latency than input delay, but needs
full save/restore of game state on every mispredict -- a far larger change. Input delay is
measurable first (`inputs_late` exists for exactly that), and rollback is the answer only if the
measurement says so.

## Disconnection: why the obvious fixes fail, and what works

A peer vanishing is not a transport problem. ENet reports the disconnection and `ge_net_enet.c`
frees the slot -- but **freeing it on local detection makes the survivors diverge from each
other**, which is the failure the handling was meant to prevent.

The model reproduces it. A departing machine's last packets do not stop everywhere at once: they
reach one survivor and not another. So one holds the dead peer's input for a few more ticks than
the other, and if each drops the slot when *it* notices, they simulate those ticks from different
input sets:

```
tick 41:  slot 0 applied {0: 47542, 1: 55461, 2: 63380}
          slot 1 applied {0: 47542, 1: 55461}
```

The obvious fix -- agree a drop tick, apply it everywhere -- **does not work as first written**, and
the tick counts say so: 181 ticks progressed on local detection against 41 with the "fix". It
agrees because it stops.

Two constraints pull against each other:

- The drop tick cannot be in a stalled machine's future. A machine waiting on the departed peer
  cannot reach a tick it needs that peer's input to get to, so the drop never applies and the
  session deadlocks.
- The drop tick cannot be moved earlier either. A machine that already **simulated** tick T
  holding the dead peer's input has diverged from one that did not, and no later agreement
  repairs a tick that has already run.

### What works: relay, then drop

The way out is an observation about what a machine can possibly have done: **nobody can have
simulated past the highest tick anybody holds.** So if the survivors pool what they hold for the
departed slot, they all converge on the same set, all become able to reach the same tick, and a
drop set just past it is reachable by every one of them.

1. On noticing a departure, each survivor **relays** the inputs it still holds for that slot.
   Bounded -- once, over the ring.
2. After the relays settle, the **lowest surviving slot** names the drop tick. A rule every
   machine computes identically, so two of them cannot announce different ticks for the same peer.
3. The tick is exactly **highest-held + 1**, and everyone applies it there.

That `+ 1` is exact, and reading it off the current tick instead is a trap worth naming: it puts
the drop one tick beyond reach, because a machine stalled at T is told to drop at T+1, which it
can only get to by simulating T, which needs the very input nobody has. It presents as a session
that agrees perfectly and stops dead -- 43 ticks, zero disagreements, looking entirely healthy.

Measured across the three behaviours:

| approach | ticks progressed | disagreements |
| --- | --- | --- |
| drop on local detection | 181 | **2** |
| drop at an agreed tick | 41 | 0 (agrees by stopping) |
| **relay, then drop** | **191** | **0** |

`geNetPeerLost()` implements this, and `ge_net_enet.c` calls it on ENet's disconnect event rather
than freeing the slot itself.

## Determinism audit

`tools/determinism_audit.py` scans the simulation and port sources for the things that break
lockstep: wall-clock reads, host RNG, frame-delta logic, threads, float-mode flags. It classifies
every hit by **where it lives**, because that is the distinction that matters -- a clock read in
the mixer or the renderer is fine, since two machines may render at different rates all day
without disagreeing about where anybody is standing. The same call inside movement or AI is
fatal.

The result across 347 files is better than expected:

**Zero simulation-path hits at medium or high severity.** GoldenEye's simulation reads neither
the wall clock nor a host random source.

Two things were then verified by reading rather than trusting the scan:

- **The RNG is the game's own.** `port_random.c` is a faithful transcription of the N64's
  `random.s` -- a 64-bit PRNG with a hardcoded seed, not `rand()`. It is deterministic and
  reproducible across machines by construction.
- **The fingerprint watches exactly that.** `gePlayerSeedFingerprint()` returns the low 32 bits of
  `g_randomSeed`, so the desync check is reading the single value that best summarises whether
  two simulations have diverged.

Every wall-clock hit outside presentation code turned out to be diagnostics -- the one in
`port_input.c` is a rate limiter that prints controller-detection output at most eight times.

So the simulation is structurally suited to lockstep, which is a significant de-risking of
everything above.

### The risk that remains

**Floating point across architectures.** The game is full of `f32` math, and a static scan cannot
prove that an ARM Mac and an x86 PC produce bit-identical results from it. Same-architecture play
is low risk; Mac-versus-Windows cross-play is not, and no amount of reading will settle it -- it
needs two machines running the same session and comparing fingerprints.

That is worth knowing before anyone assumes cross-platform WAN play is free. The fingerprint
exchange will detect it immediately if it happens, which is the right place to find out.

## Acceptance test for the transport

`tools/netsim.py` is the specification, so the transport has a concrete target rather than a
vague one: run it against the model's 20% and 40% loss scenarios and require **progress**, not
merely agreement. Agreement is easy -- a session that stalls forever agrees perfectly.

## Build status

This was written in a session where compilation could not run: every compiler invocation exits 1
with no diagnostics on either stream, including `-fsyntax-only` on a trivially valid file. A good
file and a deliberately broken one fail identically, so the exit code carries no information
about the code. **`ge_net.c` and `ge_bot_ai.c` are therefore unverified against a compiler** and
should be built before being trusted.
