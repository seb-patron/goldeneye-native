# Lua mods

Drop a directory under `mods/` with a `mod.lua` in it. It loads at startup. No rebuild.

```
mods/
  my_mod/
    mod.lua
```

## Hooks

All three are optional. A mod that defines none of them still runs its chunk body once at
load, which is enough for one-shot configuration.

```lua
function onFrame(frame)          end   -- once per rendered frame, after the game has ticked
function onPlayerSpawn(player)   end
function onWeaponFire(weapon)    end
```

## API

```lua
ge.log(text)            -- prints with a [getv][lua] prefix
ge.stage()              -- current stage id
ge.player_count()       -- 1 to 4
ge.player_pos(i)        -- x, y, z for player i (0-3), or nil if that slot is empty
```

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

## Notes

- Positions come from the game's stable player array, not the per-viewport current player,
  so they are correct in split screen.
- A script error disables that one mod and reports it once. A syntax error in somebody's mod
  should not look like a crash in GoldenEye.
- `GETV_MODDIR` points the loader somewhere other than `./mods`.
- The API is read-only for now. Write access, so a mod can move a player or spawn a prop, is
  the next step and is what turns this from telemetry into modding.
- Built without Lua, the hooks compile to empty functions and cost nothing.
