-- Level Atlas: what the extraction knows, asked from Lua.
--
-- This is the demonstration that the level data is reachable from a mod. Everything below is a
-- question a modder would actually ask, and none of it was answerable before the prop API:
-- where the keys are, which door is nearest, what is in this room, what an objective wants.
--
-- Enable with GETV_MODS=level_atlas.

local printed = false

function onFrame(frame)
 -- Once, and late enough that the level is placed. Asking at frame 1 gets a half-built world
 -- and prints a confident, wrong atlas.
    if printed or frame < 600 then return end
    printed = true

 -- ge.world() returns the level NAME as a string, not a table.
    local lvl = ge.world()
    if not lvl then
        ge.log("level_atlas: no world loaded -- set GETV_WORLD_DIR and GETV_BOT_ROUTE_LEVEL")
        return
    end

    ge.log(string.format("=== %s: %d props, %d objectives ===",
                         lvl, ge.prop_count(), ge.objectives()))

    for _, kind in ipairs({ "Door", "Key", "Collectable", "AmmoBox", "Armour", "Cctv", "Alarm" }) do
        local n = ge.prop_count(kind)
        if n > 0 then ge.log(string.format("  %-12s %d", kind, n)) end
    end

    local st = ge.player_state(0)
    if not st or not st.x then
        ge.log("  no player yet -- positional questions skipped")
        return
    end

 -- "Where is the nearest key?"
    local key = ge.prop_near("Key", st.x, st.y, st.z)
    if key then
        ge.log(string.format("  nearest key: %.0f,%.0f,%.0f  room %d  node %d",
                             key.x, key.y, key.z, key.room, key.node))
    else
        ge.log("  nearest key: this level has none")
    end

 -- "Which door leads out of here?"
    local door = ge.prop_near("Door", st.x, st.y, st.z)
    if door then
        local dx, dz = door.x - st.x, door.z - st.z
        ge.log(string.format("  nearest door: %.0f units away, room %d, node %d",
                             math.sqrt(dx * dx + dz * dz), door.room, door.node))
    end

 -- "What is in this room?"
    if st.room and st.room >= 0 then
        local here = ge.props_in_room(st.room)
        local tally = {}
        for _, p in ipairs(here) do tally[p.kind] = (tally[p.kind] or 0) + 1 end
        local parts = {}
        for k, n in pairs(tally) do parts[#parts + 1] = string.format("%s x%d", k, n) end
        ge.log(string.format("  room %d holds: %s", st.room,
                             #parts > 0 and table.concat(parts, ", ") or "nothing placed"))
    end

 -- "What does the first objective want, and where is it?"
 -- Objectives are 0-indexed here, matching the game's own numbering rather than Lua's.
    for i = 0, ge.objectives() - 1 do
        local obj = ge.objective(i)
        if obj then
 -- steps == 0 means the objective exists and cannot be routed to, which is worth
 -- saying out loud: it is the difference between "done" and "unreachable".
            ge.log(string.format("  objective %d: difficulty %d, %d target(s), %s",
                                 obj.index, obj.difficulty, obj.targets,
                                 obj.steps > 0
                                   and string.format("%d step(s) to %.0f,%.0f,%.0f",
                                                     obj.steps, obj.x, obj.y, obj.z)
                                   or "NO ROUTE"))
        end
    end
end
