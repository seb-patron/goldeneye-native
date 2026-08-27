# Modding

There are four ways to change the game, in increasing order of how much you have to know.

## 1. The config file

`goldeneye.cfg` covers video, controls, cheats, rulesets and horde. No rebuild, no code. See
[Configuration](Configuration) and [Rulesets](Rulesets).

## 2. Lua scripts

Drop a `mod.lua` in `mods/` and you get `onFrame`, `onPlayerSpawn` and `onWeaponFire`, with a
read API into live game state. No rebuild. See [Lua mods](Lua-mods).

Lua is optional at build time. Without it the build omits scripting and the entry points
compile away.

## 3. Behaviour gates

Roughly 275 `GETV_*` environment gates, each defaulting to stock behaviour. They exist because
almost every behaviour change on this port started as a question of the form "is this actually
better", and the only way to answer that is to run the same binary both ways. A gate is what
makes that possible.

If you are adding one, keep the default as the existing behaviour. A gate whose default changes
what people see is a change wearing a gate as a disguise.

## 4. The source

This is the part an emulator cannot offer. The game is ordinary C. Everything under
`vendor/ge-decomp/src` is the decompiled game and everything under `getv/port` is the platform
layer, and both are yours to edit.

```
getv/port/        platform layer: renderer, audio, input, config, saves, paths
getv/patches/     this port's changes to the decompilation and to fetched sources
getv/tools/       measurement harnesses
tools/            asset generation and the third-party fetcher
vendor/           the decompilation and fetched upstream sources   (untracked)
```

## The patch workflow, which is the part that catches people

`vendor/` is untracked. The decompilation is cloned fresh and the port's changes to it live as
patches in `getv/patches/`. The same is true of the fifteen fetched third-party files.

So **editing a file under `vendor/` and committing is not possible**, and if you only edit it,
your change is one `rm -rf vendor/` away from being gone. Changes to those trees have to be
captured back into their patch:

```bash
tools/fetch-thirdparty.sh regen     # rewrite the patch from the working tree
tools/fetch-thirdparty.sh verify    # check pristine + patch == working tree
```

`verify` is the one that matters. It rebuilds pristine-plus-patch in a scratch directory and
compares byte for byte, so anything in your working tree not represented in the patch shows up
as a mismatch rather than as a surprise for the next person who clones.

## Texture packs

There is a texture-override path that reads replacement images out of a pack directory, named by
a content hash of the raw N64 texel bytes plus format, so the name is stable across runs and
independent of where the data sits in memory. A dump mode writes the baseline images, so a pack
starts as a copy of a dump with individual files replaced rather than a guessing game about
filenames.

It is off by default and **has not been run against a real pack**. It was written and reasoned
through, not measured. Treat it as untested.

## Where the seams are

[`docs/MODDING.md`](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/MODDING.md)
covers how the tree is arranged and where the extension points are.
[`docs/PLAYER_API.md`](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/PLAYER_API.md)
and
[`docs/ENEMY_API.md`](https://github.com/SegfaultEvan/goldeneye-native/blob/main/docs/ENEMY_API.md)
cover the input and character seams, which are the ones the bots and network play both use.
