# Bots

## The finding

We do not need to build a bot AI system. GoldenEye already has one.

`src/bondaicommands.h` defines **250 AI opcodes**, and every setup file in the game carries
per-character bytecode lists written in them. The decompilation renders those lists as readable
instructions rather than hex, so Dam's first AI list reads:

```c
u8 UsetupdamZ_ai_0[] = {
    guard_start_patrol(0x00)
    jump_to_ai_list(0xfd, 0x0700)
    ai_list_end
};
```

That is a behaviour VM with a program counter, conditional branches, and subroutine jumps --
already running, already driving every guard in the campaign, already exercised by twenty
levels of shipped content.

## Why Perfect Dark is the reference

Perfect Dark is Rare's sequel in the same engine lineage, and its simulants are the most
successful bot system of that generation -- players still name them from memory. Looking at what
the simulants actually vary, and then at what GoldenEye's opcode table already exposes, the
overlap is close to total:

| Simulant concept | Native GoldenEye opcode |
| --- | --- |
| accuracy tier | `guard_set_accuracy_rating` |
| speed tier (SpeedSim, TurtleSim) | `guard_set_speed_rating` |
| TurtleSim's doubled shield | `guard_set_armour`, `guard_set_health_total` |
| awareness tier | `guard_set_hearing_scale` |
| PreySim's "attack the weak" | `if_chr_health_less_than`, `if_bond_item_total_ammo_less_than` |
| probabilistic behaviour | `if_random_seed_less_than` |
| bot-versus-bot targeting | `guard_try_running_to_chr_position` |
| evasion under fire | `guard_try_firing_roll`, `guard_try_hopping_sideways` |
| reacting to a squadmate dying | `if_guard_see_another_guard_die` |

The dials Rare exposed to players in Perfect Dark's simulant menu are, in several cases,
literally the same knobs GoldenEye already turns from script.

## The two design lessons worth stealing

**1. Skill is not one number.** A weak bot that only shoots worse feels like a strong bot with a
handicap. Rare varied speed, hearing, weapon-seeking priority and behavioural complexity
together, which is why a MeatSim plays like a novice rather than like an expert missing on
purpose. Our skill tiers vary five dials, not one.

**2. Most of the interesting bots are not harder -- they are different.** Perfect Dark shipped
opponents that refuse to shoot, that flee on contact, that hunt only the weak, that charge
suicidally. Several are *deliberately weak*. Variety of behaviour matters more to a match than
uniform competence, and this is the opposite of the instinct to make every bot as strong as
possible.

The JudgeSim deserves specific mention: it attacks whoever is currently winning. That keeps
matches close with no explicit rubber-banding anywhere in the system -- the balancing emerges
from a target-selection rule. It is a far more elegant answer than scaling difficulty, and it
is one line of targeting policy.

## What is in the repo

- `data/bots/skill_tiers.json` -- six graded tiers (meat, easy, normal, hard, perfect, dark)
- `data/bots/personalities.json` -- twelve behavioural archetypes, orthogonal to skill

A personality says what a bot *wants*; a skill tier says how well it executes. They compose:
a low-skill SpeedSim is still evasive, a high-skill one is brutal.

## Validation

`tools/gen_bot_archetypes.py` checks every archetype against the game's actual opcode table.

This matters more here than in the nuance layers. **An archetype naming an opcode that does not
exist is a bot that silently does nothing** -- the worst failure mode available to behaviour
code, because it looks implemented. The tool also enforces closed vocabularies for dials,
targeting and weapon policy, range-checks dial values, requires each archetype to state its
origin so borrowed design stays attributable, and rejects any archetype that names no opcodes at
all.

```bash
python3 tools/gen_bot_archetypes.py --out build/bots
```

Exit code is non-zero if anything fails to resolve.

## The assembler

`tools/asm_bot_ai.py` compiles each archetype into an actual AI list.

Both halves of the encoding are read from `bondaicommands.h` at run time rather than
transcribed: `#define <name>_ID 0xNN` gives the opcode byte, and the macro body gives the
operand layout -- a bare parameter is one byte, `CharArrayFrom16Rev(x)` is two, little-endian.
An opcode whose number or arity changes upstream therefore breaks the build here instead of
producing a bot that runs the wrong instruction.

Labels are ids, not offsets. The game's own lists declare targets with `label(n)` and jump to
`n`, so no address fixups are needed -- but the assembler still checks that every referenced
label is declared and none is declared twice, because a jump to a missing label is a bot that
silently falls through.

Each list is generated as the same loop shape the game's own guard lists use: set the dials,
then a sleep-and-poll main label, a perception check that branches to engage, and a return to
main. What varies per archetype is which dials are written, which perception checks are wired,
and what the engage branch does -- the same axis Perfect Dark's simulants vary along. Aggression
becomes a literal dice roll via `random_generate_seed` and `if_random_seed_greater_than`, so a
timid archetype breaks off more often than a committed one.

```bash
python3 tools/asm_bot_ai.py --out build/bots/ai
```

18 lists, 596 bytes total. Outputs are a `.bin` per archetype, a `bot_ai_lists.c` written with
the game's own macros so it can be read and compiled directly, and a manifest.

**Every list is round-tripped**: the emitted bytes are disassembled with the same instruction
set and must reproduce exactly what was assembled. Emitting bytes and trusting them is how you
ship a list whose operand is a byte out, which makes the interpreter read the next opcode from
the middle of an operand and do something arbitrary. This makes that a build failure.

The arity checking earned its keep immediately: the first run rejected every `guard_try_*` call
because they take exactly one operand -- the label to jump to when the action cannot be performed
 -- and the generator was passing two.

## Which bot path is the destination

Both, for different jobs. They compose rather than compete, and the two documents used to point
different ways, so to be explicit:

- **Player slot** (`ge_bot.c`, and the argument in `docs/PLAYER_API.md`) -- anything that must *be*
  a player: netplay peers, RL agents, co-op partners, bots filling out a four-player roster. One
  seam serves all of them, which is the whole point of that design.
- **Character** (`ge_bot_ai.c`, this document) -- opponents *beyond* the four player slots. The
  game has exactly four, and four players **and** four bots is not reachable through the slot
  path at all.

Neither replaces the other. A full match is humans and injected policies in the slots, plus
AI-driven characters alongside them.

## Doors: bots do not need to open them

Worth stating because it is the obvious next thing to build and it would be wasted work.

The AI instruction set has full door control -- `door_open`, `door_close`, `if_door_state_equal`,
`if_door_has_been_opened_before`, and lock control -- all addressed by object tag. But **doors are
almost entirely untagged**: Dam has 18 doors and 2 tags, Facility 46 and 7. Building bot
navigation on `door_open` would reach roughly a tenth of the doors in the game.

It is also unnecessary. `chraction.c:9138` is in the character *movement* path: it measures the
distance from the character to a door prop and, inside 200 units, chooses a swing direction and
calls `doorActivate`. **A character opens a door by walking near it**, exactly as the campaign's
own guards do.

So "the bot cannot get through a door" is not a door problem, it is a movement problem -- and the
movement problem was real: eight of eighteen archetypes could not move at all (see below).
`door_open` remains the right tool for the minority of *scripted* doors: gates, locked doors, and
anything a level opens as an event.

## Why an injected bot strafes instead of turning

the Mac build measured a route-following bot on Bunker 1 producing `spd=0.000` with a non-zero move
offset -- every unit of it sideways -- while its heading only showed Bond's idle sway. The bot had
been sidestepping at full deflection for its entire run. Injection was not the problem: with the
bot off the walk block never executes, with it on it runs every frame.

The stick itself is innocent. `bondview2.c:5245-5247` assigns **both** turn and strafe from the
same value:

```c
moveData.analogTurn   = moveData.controlStickXSafe;
moveData.analogStrafe = moveData.controlStickXSafe;
```

Both are then consumed, each behind its own flag, and both flags initialise to **0** (`5198`,
`5199`):

| effect | line | gate |
| --- | --- | --- |
| yaw | `6394` | `canNaturalTurn` |
| sideways speed | `6069` | `canTurnTank` |

That second gate is the answer, and its name is a lie -- the decomp carries a comment at `6067`
wondering why a tank property guards walking. Tracking every assignment to it settles what it
really means:

- `5384` -- `= 1`, unconditionally, on the **two-controller** path
- `5630`, `5776` -- tank only (`g_PlayerIsInTank == 1`)
- nowhere else

So on a 1.x style `canTurnTank` is never set outside a tank, and the assignment at `6069` never
runs. **The analog stick cannot strafe on 1.x.**

I first wrote that as "sideways movement is unreachable on 1.x, therefore the slot is provably on
2.x". That was wrong twice over and the corrections are worth keeping.

**Sideways movement is still reachable on 1.x**, just not from the stick. Lines `6056-6064` call
`bondviewUpdateSpeedSideways` from `digitalStepLeft`/`digitalStepRight` -- the C-buttons and D-pad,
set at `5659-5663` -- and they run *before* the `canTurnTank` gate, unconditionally. I had grepped
every read of `analogStrafe` and concluded I had found every way to move sideways, having never
grepped the writes to `speedsideways` itself. Two of the three ways in were through a function
call I never looked for.

**And 6069 sits in an `else`.** Its `if` is `g_PlayerIsInTank == 1` at `5955`. I read the else and
called it the whole picture, which is the third time on this one bug that I have described a
branch as though it were the only one.

What is actually established, and what is not:

- Established, by reading: the stick's X axis reaches strafe only where `canTurnTank` is set, and
  that is the two-controller path (`5384`) or a tank (`5630`, `5776`).
- Established, by running: this machine's config is `controls=5` -- **2.2 Galore, a two-controller
  style** -- and the port presents one physical gamepad as two N64 ports
  (`input: N64 ports connected = 2`). So the 2.x condition is real here and is the default, not a
  misconfiguration someone chose.
- **Not** established: that the injected slot is on that style at the moment it matters. A probe
  added to `gePortBotRouteInit` prints `control style = 0`, contradicting the config. The probe
  runs during level load, so it is most likely reading before the style is applied -- but until
  that is measured during a gameplay frame, the honest answer is that the two readings disagree
  and neither is confirmed.

If it does hold up, the fix is configuration -- put the slot on a 1.x style -- not a change to the
steering law. That is worth checking before anyone edits the follower.

### The second bug, which is ours

The 2.x path derives aim mode differently from the 1.x path, and the difference bites us.

1.x is style-aware (`5555-5566`): `aimButtons = L_TRIG | R_TRIG`, and Z_TRIG is `shootButtons`.
2.x is not -- it reads Z_TRIG raw (`5355` → `5365`):

```c
sp104 = (buttons & Z_TRIG) != 0;
...
g_CurrentPlayer->insightaimmode = sp104;
```

And `ge_player_api.c:96` maps `GE_IN_FIRE` to Z_TRIG on every non-swapped style. So on a 2.x
config **firing is indistinguishable from aiming**, `insightaimmode` goes true, and
`canNaturalTurn = !insightaimmode` (`5385`) goes false.

`ge_bot.c:98` fires in bursts, 8 ticks in every 60. On a 2.x slot that bot disables its own
steering 13% of the time, and would read as an intermittent turning fault rather than a firing
one. The route bot never sets `buttons` at all, so it is exempt -- which is why the two bots would
have disagreed if both had been measured.

Found by reading rather than by running, but the first half is structural: `canTurnTank` has four
assignments and three are tank-gated, so the 1.x conclusion does not depend on runtime state.

## What this does not yet do

1. Attaching a compiled list to a multiplayer character slot. The campaign path spawns
   characters from setup records; the arena path needs an equivalent.
2. Wiring target selection (`weakest`, `leader`, `last_attacker`, `fixed_rival`) to scoreboard
   and damage state, which the opcode table does not expose on its own.

Item 2 is the only part that needs genuinely new engine code. Item 1 is plumbing over machinery
that already runs.
