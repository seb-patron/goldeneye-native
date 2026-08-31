# Cheats

GoldenEye has its own cheat system. This port exposes it **by name**, through the `cheats`
configuration key or the launcher. These are not GameShark codes, and the difference is not
pedantry.

## Why not GameShark codes

A GameShark code is a raw N64 RDRAM address. This port has no RDRAM. It has native pointers at
addresses the loader picks, so the published code lists cannot be applied the way an emulator
applies them. Mapping the codes back to symbols was measured at a 1.3% resolve rate against
this decompilation, which is a dead end rather than a hard problem.

It turned out not to matter. Twenty-four of the famous codes cluster one byte apart starting at
`0x80069652`, and

```
gameshark_address - 0x80069650  ==  the CHEAT_ID enum ordinal
```

exactly, gaps included. `0x80069650` is the retail address of the game's own cheat-flag array,
and the skipped addresses land precisely on enum members the published lists never name. A
coincidence does not reproduce that.

So the well-known cheats are just the game's own cheat flags. Setting them by name is
layout-independent, survives relinking and recompilation, and stays correct under mods that move
the array. None of which an address list can do.

## Using them

```
cheats = dk_mode, paintball, infinite_ammo
```

or:

```bash
./build-mac/goldeneye --cheats=dk_mode,paintball
```

Names are comma-separated and surrounding whitespace is trimmed. An unknown name is reported as
an error and the rest of the list still applies. Cheats accumulate, and there is no syntax for
turning one off, so remove it from the list instead.

`--list-cheats` prints the full set.

## Which take effect immediately

Six apply straight from the config file: `dk_mode`, `infinite_ammo`, `paintball`, `no_radar`,
`enemy_rockets` and `extra_mp_chars`. The rest set the flag but need toggling in-game to apply,
and `--list-cheats` marks which is which rather than leaving you to guess.

Related, and separate: `roster = 64` unlocks the full multiplayer character list, and
`unlock_all = 1` shows every mission on the file-select screen. `roster = 33` is not settable
because it is derived from the save rather than being a flag.

Full detail is in
[`docs/CHEATS.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/CHEATS.md).
