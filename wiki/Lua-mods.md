# Lua mods

Drop a directory under `mods/` with a `mod.lua` in it. It loads at startup. No rebuild.

```
mods/
  crt_screen/          <- the worked example. Read this one first.
    mod.lua
  my_mod/
    mod.lua
```

## Start with `crt_screen`

`mods/crt_screen` is the example, and it is a real feature rather than a toy: it is what
draws the scanlines, the aperture mask, the curved tube and the vignette. It ships enabled.

It is a mod on purpose. Untick it on the launcher's Mods page and the scanlines go away --
which demonstrates the whole system in one action, in a way that a mod printing a line to a
log never could. Copy the folder, rename it, and you have a working mod.

The tunables are five named constants at the top of its `mod.lua` with a comment each. Edit
one, restart, see the difference.

## Hooks

All three are optional. A mod that defines none of them still runs its chunk body once at
load, which is enough for one-shot configuration.

```lua
function onFrame(frame)          end -- once per rendered frame, after the game has ticked
function onPlayerSpawn(player)   end
function onWeaponFire(weapon)    end
```

## API

```lua
ge.log(text) -- prints with a [getv][lua] prefix
ge.stage() -- current stage id
ge.player_count() -- 1 to 4
ge.player_pos(i) -- x, y, z for player i (0-3), or nil if that slot is empty
ge.postfx{ ... } -- the post-process pass; see below
```

### `ge.postfx`

The first call that **writes** rather than reads. It sets the fullscreen pass the renderer
applies to the finished frame, and returns what was actually applied after clamping:

```lua
local fx = ge.postfx {
    crt      = true, -- master switch for the four CRT terms
    scanline = 0.28, -- 0..1
    mask     = 0.18, -- 0..1, aperture grille
    curve    = 0.025, -- 0..0.1, barrel distortion
    vignette = 0.22, -- 0..1
    lines    = 240, -- virtual scanline count, independent of window size
    fxaa     = false, -- edge antialiasing
}
```

Every field is optional and anything omitted keeps its current value, so a mod can change one
number without restating the rest. Called with no table it only reports, which makes
`ge.postfx().crt` a way to ask what is currently on.

Values are **read every frame, not latched at startup**, so this works from `onFrame()` too --
an effect can change while the game runs.

Brightness is compensated automatically for whatever the scanlines and mask absorb, so raising
either changes the texture of the image rather than dimming it.

## Example

```lua
ge.log("hello from " .. _VERSION)

function onFrame(frame)
    if frame % 60 == 0 then
        local x, y, z = ge.player_pos(0)
        if x then
            ge.log(string.format("f%d  %.1f,%.1f,%.1f", frame, x, y, z))
        end
    end
end
```

## Turning mods on and off

The launcher's **Mods** page scans the folder and lists what it finds, with a checkbox each,
so any number can be switched off at once. Enable All and Disable All are there for when the
list gets long, and Rescan picks up a folder dropped in while the launcher is open.

From a shell it is one variable:

```
GETV_MODS_OFF=spawn_logger,frame_counter
```

A **denylist**, not an allowlist, and deliberately so. The contract at the top of this page is
"drop a directory in and it loads"; an allowlist would quietly break it, because every newly
added mod would sit there disabled until someone remembered to list it. With a denylist,
unset means everything loads exactly as before and the only thing recorded is the decision to
switch something off.

Names match whole, so `hello` does not disable `hello_goldeneye`. Every skipped mod is named
in the log:

```
[getv][lua] skipping "frame_counter" (disabled)
[getv][lua] 2 mods active from "mods" (1 disabled)
```

That line exists so "I disabled it and it still ran" and "I enabled it and it did not" are
both answerable from the output.

## Notes

- Positions come from the game's stable player array, not the per-viewport current player,
  so they are correct in split screen.
- A script error disables that one mod and reports it once. A syntax error in somebody's mod
  should not look like a crash in GoldenEye.
- `GETV_MODDIR` points the loader somewhere other than `./mods`.
- A subdirectory without a `mod.lua` is skipped silently. It is not a mod, so it is not an
  error.
- The API is read-only for now. Write access, so a mod can move a player or spawn a prop, is
  the next step and is what turns this from telemetry into modding.
- Built without Lua, the hooks compile to empty functions and cost nothing.
