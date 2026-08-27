-- Level Atlas: what the extraction knows, asked from Lua.
--
-- This is the demonstration that the level data is reachable from a mod. Everything below is a
-- question a modder would actually ask, and none of it was answerable before the prop API:
-- where the keys are, which door is nearest, what is in this room, what an objective wants.
--
-- Enable with GETV_MODS=level_atlas.

local printed = false

-- The slot the atlas reports on. Matches the ge.player_state(0) below rather than being a second
-- opinion about which player this is.
--
-- Defined because the sensing section referenced SLOT before anything set it, and an undefined
-- global in Lua is nil rather than an error: luac -p passes it, and it fails at runtime inside
-- luaL_checkinteger with a message about argument 1 rather than about the mod. Syntax checking
-- does not catch this class of bug.
local SLOT = 0

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

    -- ================================================================= SENSING
    --
    -- The interaction half. Everything above answers "what does this level contain"; this answers
    -- "what is against me, and who is looking" -- which is what a bot actually acts on.
    --
    -- Skipped rather than guessed when the build reports no heading. Assuming a facing would print
    -- a confident reading of the wrong direction, which is worse than printing nothing.
    if st.angle == nil then
        ge.log("  sensing: this build reports no heading -- skipped")
        return
    end

    -- What is ahead, and how far. The BODY test rather than the ray: a ray fits through gaps a
    -- player does not, so the ray version can call a corridor clear that cannot be entered.
    local c = ge.sense_ahead_body(st.x, st.z, st.angle, 300)
    local what = {}
    if c.wall   then what[#what + 1] = "WALL"   end
    if c.door   then what[#what + 1] = "DOOR"   end
    if c.object then what[#what + 1] = "OBJECT" end
    if c.body   then what[#what + 1] = "BODY"   end
    ge.log(string.format("  ahead: %s%s",
                         c.clear and "clear" or table.concat(what, " "),
                         c.clear and "" or string.format(" at %.0fu", c.distance)))

    -- THE DOOR BIT DOES NOT MEAN "A DOOR IS IN FRONT OF YOU".
    --
    -- geSenseLine reports WALL, DOOR and OBJECT together whenever the ray merely grazes a doorway
    -- edge, so the bit says what the line touched, not what is ahead. A door bit alongside a wall
    -- or object bit is a grazed edge, not an open doorway -- and "I cannot tell" and "there is a
    -- door" lead to opposite actions, so only an isolated door bit is reported as a door.
    --
    -- Confirming against the prop table (a real door, within the engine's own 200 units, and near
    -- the bearing to the target) is what a bot should do before acting on this. The atlas
    -- deliberately does not repeat that check here: a second, subtly different door test living in
    -- a mod is how two answers to one question start disagreeing.
    if c.door and not c.wall and not c.object then
        ge.log("    a DOOR alone -- an obstacle to a planner, an opportunity to a bot with a hand")
    elseif c.door then
        ge.log("    door bit set ALONGSIDE wall/object -- that is a GRAZED EDGE, not a door ahead;"
               .. " confirm against the prop table before acting on it")
    end

    -- Which way is clear, as a TURN rather than a bearing: the useful question is how far to
    -- turn, not where north is.
    --
    -- THE BODY VERSION, not ge.clearest_heading. A line has no width, so a gap narrower than
    -- the player passes the line test and the sweep hands it back as the best way out -- and a
    -- reader who acts on it walks into the one direction it cannot fit through, with the report
    -- insisting it chose correctly. The sensor lies; the policy is fine.
    --
    -- The second return is how far that heading is actually clear for. Printed, because "turn
    -- +60" with 40 units behind it and "turn +60" with 300 are different advice and the bearing
    -- alone cannot tell them apart.
    local best, room = ge.clearest_heading_body(st.x, st.z, st.angle, 90, 300)
    local turn = best - st.angle
    while turn > 180 do turn = turn - 360 end
    while turn < -180 do turn = turn + 360 end
    ge.log(string.format("  clearest: %+.0f deg, clear for %.0fu%s", turn, room or 0,
                         math.abs(turn) < 1 and " (straight ahead is fine)" or ""))

    -- Am I against something, as opposed to predicting one? History, not geometry.
    -- Three states, not two: "not stuck" is not the same claim as "moving freely" -- a
    -- stationary player is neither, and with no data yet the honest answer is "no data at
    -- all" rather than a guess at one or the other.
    local stuck, travel = ge.is_stuck(SLOT, 8)
    local contact
    if stuck then
        contact = "STUCK -- commanded to move and did not"
    elseif travel < 1 then
        contact = "stationary"
    else
        contact = "moving freely"
    end
    ge.log(string.format("  contact: %s (%.0fu travelled recently)", contact, travel))

    -- Could see me, versus is looking at me. THE GAP IS THE NUMBER THAT MATTERS: many with a line
    -- and few looking is a room you can cross.
    local could, looking = ge.watchers(SLOT)
    ge.log(string.format("  seen by: %d could (line of sight), %d actually looking", could, looking))
    if could > 0 and looking == 0 then
        ge.log("    nobody is facing you -- that is a room you can cross")
    end

    -- What could be acted on without moving.
    local use = ge.usable(st.x, st.y, st.z)
    if #use == 0 then
        ge.log("  usable: nothing within reach")
    else
        for _, u in ipairs(use) do
            local kind = u.door and "door" or u.pickup and "pickup" or u.switch and "switch"
                         or "unknown"
            ge.log(string.format("  usable: %s at %.0fu (prop %d)", kind, u.distance, u.prop))
        end
    end
end
