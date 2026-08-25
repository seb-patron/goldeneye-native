# Netplay

The goal: four or more people, each on their own machine, each full-screen, playing each other
over a WAN -- plus bots, in any mix, including nobody at all and two bots playing while you
watch.

## Status: the approach here is disputed inside this repository

This document argues for lockstep. [`docs/PLAYER_API.md`](PLAYER_API.md) section 14 argues
against it and was written first, from measurements. **That disagreement is unresolved, and
both documents are kept until it is.** Anyone building on this should read both.

The case against lockstep, none of which this document currently answers:

- **Streets (level 29) is nondeterministic across processes**, verified. At least one level
  cannot be lockstepped as it stands.
- **PAL and NTSC are compile-time constant sets**, not a scale factor, so a US and a EU client
  can never share a lockstep session.
- **Cross-platform bit-exactness is not realistically achievable.** The Perfect Dark port, with
  the same ancestry as ours, ships at `-Og` because `-O2` breaks the game.
- **Lockstep adds round-trip time to your own aim**, which an FPS tolerates far worse than the
  RTS designs the technique came from.

The counter-argument this document makes is that only inputs travel, twelve bytes a tick,
where world state at 60Hz over a domestic link is not shippable at all -- and that the
determinism audit it calls for in step 3 is exactly what would settle the first three points.
That audit has not been done. Until it is, treat what follows as a design under test rather
than a decision.

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

## What is not built

1. **A real transport.** `GeNetTransport` is two function pointers; a UDP implementation drops in
   without touching the session logic. Untested here because this session cannot compile (see
   below).
2. **Full-screen per machine.** GoldenEye multiplayer is split-screen: one machine, N viewports,
   all players local. Rendering a single viewport is the easy half -- the renderer already does
   per-player viewports. The real work is that game logic assumes every player is local.
3. **A determinism audit.** Lockstep is only as good as this. Anything reading wall-clock time,
   host RNG, or frame timing is a divergence waiting to happen, and the fingerprint will catch it
   but not locate it.

Item 2 is the largest, and it is the one that decides whether this is playable rather than merely
synchronised.

## Build status

This was written in a session where compilation could not run: every compiler invocation exits 1
with no diagnostics on either stream, including `-fsyntax-only` on a trivially valid file. A good
file and a deliberately broken one fail identically, so the exit code carries no information
about the code. **`ge_net.c` and `ge_bot_ai.c` are therefore unverified against a compiler** and
should be built before being trusted.
