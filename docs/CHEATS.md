# Cheats

GoldenEye has its own cheat system. This port exposes it by name through the `cheats`
configuration key. These are not GameShark codes, and the difference matters.

## Why not GameShark codes

A GameShark code is a raw N64 RDRAM address. This port has no RDRAM - it has native pointers at
addresses the loader chooses - so the published code lists cannot be applied here the way an
emulator or a recompilation applies them. Mapping the codes back to symbols was measured at a
1.3% resolve rate against this decompilation, so the address route is a dead end.

It turned out not to be needed. Twenty-four of the famous codes cluster one byte apart starting at
`0x80069652`, and

    gameshark_address - 0x80069650 == the CHEAT_ID enum ordinal

exactly, gaps included. `0x80069650` is the retail address of the game's own cheat-flag array. The
skipped addresses land precisely on enum members the published lists never name, which a
coincidence would not reproduce.

So the well-known cheats are simply the game's own cheat flags, and this port sets them by name.
That is layout-independent, survives relinking and recompilation, and stays correct under mods
that move the array - none of which an address list can do.

## Using them

```
cheats = dk_mode, paintball, infinite_ammo
```

or on the command line:

```bash
./build-mac/goldeneye --cheats=dk_mode,paintball
```

Names are comma-separated; surrounding whitespace is trimmed. An unknown name is reported as an
error and the rest of the list still applies. Cheats accumulate - there is no syntax for turning
one off, so remove it from the list instead.

Run `./build-mac/goldeneye --list-cheats` for the full table with ids and status.

## `[live]` versus `[flag]`

`--list-cheats` marks every cheat one way or the other, and the distinction is the difference
between a cheat that works from your config file and one that does not.

**`[live]`** - the game reads this flag directly while it runs, through `cheatIsActive()`, at the
point where the effect happens. Setting the flag at startup reaches every one of those call sites,
so the cheat takes effect straight from the configuration file. Six qualify:

| Cheat | Where it is read |
|---|---|
| `extra_mp_chars` | Handled specially - see `roster` below |
| `infinite_ammo` | `lv.c` |
| `dk_mode` | `chr.c`, `chr_b.c` |
| `paintball` | `explosion.c` |
| `no_radar` | `radar.c` |
| `enemy_rockets` | `prop.c` |

**`[flag]`** - everything else. The flag is genuinely set, and the game's own interface honours it,
but the *effect* is applied once inside the cheat turn-on switch: granting weapons, multiplying
health, and so on. That code needs a player context which does not exist when the configuration is
read, at the very start of `main()`. Toggle the cheat in-game to actually get it.

Each `[flag]` cheat prints a line explaining this when you set it, so nothing is silently
half-applied and nothing claims to work that was not observed to work.

The distinction was measured, not guessed. An earlier version of the table assumed a bare flag
write was enough for more cheats than it is; `cheats=line_mode` produced a byte-identical frame to
baseline. The current list comes from enumerating every live `cheatIsActive()` consumer in the
source tree.

## Lifetime

Cheats set from the configuration file apply to the session you boot into and are cleared when you
leave the stage. The game's own `cheatDisableAllCheats()` runs on stage unload and clears every
cheat carrying the toggle mask. That is retail lifetime, not a defect.

## The names

Accepted by the `cheats` key:

```
extra_mp_chars invincibility all_guns max_ammo
line_mode 2x_health 2x_armor invisibility
infinite_ammo dk_mode extra_weapons tiny_bond
paintball 10x_health magnum laser
golden_gun silver_pp7 gold_pp7 bond_phase
no_radar turbo_mode debug_pos fast_animation
slow_animation enemy_rockets 2x_rocket_launcher
2x_grenade_launcher 2x_rcp90 2x_throwing_knife
2x_hunting_knife 2x_laser
```

Two names that appear in a naive grep of the source - `CHEAT_MARQUIS` and `CHEAT_ENEMYSHIELDS` -
are not exposed. Both occur only inside commented-out Perfect Dark leftovers, and neither exists
in the cheat enum at all.

## `roster`

Separate from `cheats`, but the same mechanism underneath.

```
roster = 64
```

`8` is the shipped default and does nothing. `64` unlocks the full multiplayer character list; it
sets the same flag as `cheats = extra_mp_chars` and writes the selectable-character count
directly. That value is sticky - the character-select screen recomputes the roster every frame but
short-circuits the whole block when the count is already at the unlocked value.

`33` is refused with an explanation rather than faked. It is derived from the save file (complete
Cradle on Agent) and is recomputed every frame for any value other than 64, so writing it would be
overwritten before the first frame finished and the setting would appear to do nothing.

## Caveat if you are editing the source

The cheat ids in `getv/port/src/ge_config.c` are **hard-coded numbers transcribed by hand from the
`CHEAT_IDS` enum in `vendor/ge-decomp/src/bondconstants.h`.** They are not `#include`d, because
that file is platform-layer code and pulling in a game header would drag `<ultra64.h>` into the
port build.

This means the table is only correct as long as that enum does not change. If the enum ever gains
or loses a member, every ordinal after the insertion point shifts by one and the table silently
applies the *wrong cheat* - no compile error, no warning, just a different cheat than the one you
named. Re-check the table against the enum after any change to the decompilation.

The same trap exists in the binding and action tables in `getv/port/src/port_os.c`, which are
positional against their own enums for the same reason.
