-- Example mod. Copy this directory, rename it, and edit.
--
-- Every directory under mods/ that contains a mod.lua is loaded at startup. This file's
-- body runs once at load; the three hooks below are optional and are called by the game.
--
-- Run the game with GETV_MODDIR=<path> to load mods from somewhere other than ./mods.

ge.log("hello from " .. _VERSION)
ge.log("stage id at load: " .. ge.stage())

local frames = 0

-- Called once per rendered frame, after the game has ticked, so positions read here are
-- this frame's rather than last frame's.
function onFrame(frame)
    frames = frames + 1

 -- Once a second at 60fps. Printing every frame would bury everything else in the log.
    if frame % 60 == 0 then
        local x, y, z = ge.player_pos(0)
        if x then
            ge.log(string.format("f%d  players=%d  p0=%.1f,%.1f,%.1f",
                                 frame, ge.player_count(), x, y, z))
        else
            ge.log(string.format("f%d  no player 0 yet", frame))
        end
    end
end

function onPlayerSpawn(player)
    ge.log("player " .. player .. " spawned")
end

function onWeaponFire(weapon)
    ge.log("weapon " .. weapon .. " fired")
end
