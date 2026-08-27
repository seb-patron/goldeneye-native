# Bots

## We did not write a bot AI

GoldenEye already has one. `src/bondaicommands.h` defines **250 AI opcodes**, and every setup
file in the game carries per-character bytecode lists written in them. The decompilation renders
those lists as readable instructions rather than hex, so Dam's first AI list reads:

```c
u8 UsetupdamZ_ai_0[] = {
    guard_start_patrol(0x00)
    jump_to_ai_list(0xfd, 0x0700)
    ai_list_end
};
```

That is a behaviour virtual machine with a program counter, conditional branches and subroutine
jumps. It is already running, already driving every guard in the campaign, and already
exercised by twenty levels of shipped content. Writing a second AI alongside it would have meant
maintaining something less tested than the one already in the binary.

So the bots drive the game's own machine rather than replacing it, with navigation, door
handling and an arbiter layered on top.

## The seam

`ge_player_api` posts controller input per slot per tick, and **refuses a post for a tick that
has already run**. That refusal was written for bots, and it turns out to be the same
constraint network play has. Which means a bot, a remote player and a training agent are all
the same kind of thing: something that supplies input for a slot before its tick runs.

| want | how |
| --- | --- |
| a bot opponent | bot policy fills the slot |
| co-op with a bot | bot policy fills the second slot |
| watching two bots play | policies fill both, camera follows one |
| a player on another machine | their input arrives over the wire |
| a training agent | agent posts into a slot |

Nothing in the game distinguishes them, and that is deliberate. It is also why network play is
close to the bots in the code: it plugs into the same seam.

## What is in the tree

- `ge_bot.c` and `ge_bot_ai.c`, the policy and the bridge into the game's AI lists
- `ge_bot_nav.c`, navigation
- `ge_bot_doors.c`, door handling, which is its own problem in a game with this many of them
- `ge_bot_route.c`, routing
- `ge_bot_arbiter.c`, which decides between competing intents

They are called once per rendered frame from the port's render path, and are inert unless
turned on.

The arbiter, the policy, the intent layer and the sense layer all have unit tests, because they
are ordinary logic that does not need a window to exercise. That is the part of a bot system
worth testing: the decisions, not the drawing.

## Turning them on

Bots are behind `GETV_*` gates rather than a config key for now, which is honest about their
maturity. `docs/BOTS.md` in the repository carries the current gates and what each one does,
alongside the reasoning about which Perfect Dark simulant behaviours map onto opcodes GoldenEye
already has.

## Why Perfect Dark is the reference

Perfect Dark is Rare's sequel in the same engine lineage, and its simulants are the most
successful bot system of that generation. Players still name them from memory. Comparing what
the simulants actually vary against what GoldenEye's opcode table already exposes, the overlap
is close to total, which is a strong hint that the ceiling here is high and mostly a matter of
using what is already present.
