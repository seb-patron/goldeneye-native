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

That is a behaviour VM with a program counter, conditional branches, and subroutine jumps -- already running, already driving every guard in the campaign, already exercised by twenty
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

## What this does not yet do

The archetypes are specifications, not implementations. Nothing yet compiles them into AI lists
or attaches them to multiplayer characters. The remaining work is:

1. An assembler from archetype JSON to AI-list bytecode.
2. Attaching a compiled list to a multiplayer character slot -- the campaign path spawns
   characters from setup records, and the arena path will need an equivalent.
3. Wiring target selection (`weakest`, `leader`, `last_attacker`, `fixed_rival`) to
   scoreboard and damage state, which the opcode table does not expose on its own.

Item 3 is the only one that needs new engine code. The other two are assembly and plumbing over
machinery that already runs.
