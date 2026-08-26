-- A bot that walks a real route, written entirely in Lua.
--
-- This exists to show that the world and player APIs are a platform rather than an internal
-- convenience. Everything here is available to any mod pack: ask which objectives the level has,
-- read the route to one, see what is usually dangerous on the way, and drive a player slot.
-- Nothing below reaches into the game -- it reads knowledge, reads state, posts input, which is
-- the same shape the C bot and a network peer have.
--
-- It is deliberately the SAME steering law as getv/port/src/ge_bot_route.c, constants included,
-- because a second implementation that quietly disagreed would be worse than none.

local SLOT        = tonumber(os.getenv("GE_ROUTE_BOT_SLOT") or "0")
local WALK        = 60 -- N64 counts; the walk deadzone subtracts about 5
local ARRIVE      = 120 -- world units; must stay outside the turning circle
local TURN_GAIN   = 3.0
local ALIGN_DEG   = 60.0
local STICK_MAX   = 80
local MOVE_EPS    = 1.5 -- below this, movement is noise rather than direction

-- Threat-hold, mirroring ge_bot_route.c exactly. Wider than ARRIVE on purpose: the question is
-- not "is a guard standing on the waypoint" but "is anyone walking to it", and a belief is a
-- destination.
local THREAT_RADIUS = 300
-- There MUST be a cap. Waiting for a route to clear sounds prudent and deadlocks, because nothing
-- about waiting makes a guard change its mind, and a frozen bot looks exactly like a crashed one.
local MAX_HOLD      = 180 -- frames; three seconds at 60Hz, then commit to the plan

local objective, step, steps = nil, 0, 0
local held                   = 0 -- frames spent waiting on the current waypoint
local heading, haveHeading   = 0.0, false
local px, pz, havePrev       = 0.0, 0.0, false
local announced              = false

local function norm180(a)
    while a > 180 do a = a - 360 end
    while a < -180 do a = a + 360 end
    return a
end

-- Pick the first objective that actually HAS a route. Most do not: 48 of the game's 80
-- objectives complete on a flag rather than at a place, so they have no target to walk to. A bot
-- sent to one of those stands still and looks broken when it is merely unrouted.
local function pickObjective()
    local n = ge.objectives()
    for i = 0, n - 1 do
        local ob = ge.objective(i)
        if ob and ob.steps > 0 then return i, ob end
    end
    return nil, nil
end

-- Events arrive here rather than being polled. For a learning agent these are the episode
-- boundaries: level_change and player_spawn start a run, player_gone ends one, and missing a
-- boundary is worse than missing a frame of observation because it corrupts the whole episode.
--
-- One hook for every event type on purpose -- the set grows as game-side publish sites land, and
-- a mod written today keeps working when it does. Do NOT post input from here: this fires inside
-- the frame hook, so the tick it would post for is still being assembled.
function onEvent(name, a, b, c)
    if name == "level_change" then
 -- A new level invalidates the route entirely, so forget it and re-pick.
        objective, step, steps = nil, 0, 0
        haveHeading, havePrev, announced = false, false, false
        ge.log(string.format("route_bot: level changed (stage %d), route reset", a))
    elseif name == "player_gone" and a == SLOT then
        ge.log("route_bot: our slot went away -- episode over")
        haveHeading, havePrev = false, false
    elseif name == "guard_near" and a == SLOT then
        ge.log(string.format("route_bot: guard %d within %d units", b, c))
    end
end

function onFrame(frame)
    local level = ge.world()
    if not level then return end -- no extracted knowledge for this stage

    if not objective then
        local i, ob = pickObjective()
        if not i then
            if not announced then
                ge.log("route_bot: " .. level .. " has no routable objective")
                announced = true
            end
            return
        end
        objective, steps, step = i, ob.steps, 0
        ge.log(string.format("route_bot: %s objective %d, %d steps to %.0f,%.0f,%.0f",
                             level, i, ob.steps, ob.x, ob.y, ob.z))
    end

    local st = ge.player_state(SLOT)
    if not st or not st.x then return end

 -- Heading has to be dead-reckoned: the game cannot yet report facing, so the direction the
 -- bot moved is the direction it is pointing, near enough, while it is moving. When the angle
 -- accessor lands, st.angle appears in the table and this block should use it instead.
    if st.angle then
        heading, haveHeading = st.angle, true
    elseif havePrev then
        local mx, mz = st.x - px, st.z - pz
        if mx * mx + mz * mz > MOVE_EPS * MOVE_EPS then
            heading = math.deg(math.atan(mx, mz))
            haveHeading = true
        end
    end
    px, pz, havePrev = st.x, st.z, true

    if step >= steps then return end

 -- FOLLOW THE ROUTE, waypoint by waypoint. The first version of this fetched the step and
 -- then walked at the objective's final position instead, which ignored the route entirely
 -- and would have walked into whatever wall lay between. It did that because the API had no
 -- way to ask where a waypoint is -- ge.waypoint(id) exists now because writing this found
 -- that gap.
    local s = ge.route_step(objective, step)
    if not s then return end
    local wp = ge.waypoint(s.to)
    if not wp then return end

 -- AIM AT GROUND A BODY FITS ON, WHICH IS NOT ALWAYS THE PAD.
 --
 -- The route's waypoints are the game's own nav pads. On Train they run about 70 units off the
 -- walkable centre, and the engine's own walkability test refuses 18 of the level's 46 steps as
 -- a result -- yet the same steps are clear a little to one side. ge.route_lane finds the
 -- smallest sideways correction that opens the step, so the bot walks the floor rather than the
 -- pad line. Inert unless GETV_ROUTE_LANE is set, in which case it returns the pad unchanged.
    local ax, az, off = ge.route_lane(st.x, st.z, wp.x, wp.z)
    if off ~= 0 then wp.x, wp.z = ax, az end

    local dx, dz = wp.x - st.x, wp.z - st.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist <= ARRIVE then
        step = step + 1
        held = 0 -- the next waypoint gets its own patience, not this one's leftovers
        if step >= steps then
            ge.log(string.format("route_bot: arrived at objective %d", objective))
        end
        return
    end

 -- IS ANYONE CONVERGING ON WHERE WE ARE ABOUT TO STAND?
 --
 -- ge.threat_at counts living enemies whose last-known-target position is near the waypoint,
 -- which is a different question from how many are near it now -- a waypoint can be empty and
 -- lethal because three guards are walking to it. With no enemy source installed this is always
 -- 0 and the bot behaves exactly as it did before, so the policy is inert rather than wrong on
 -- a build whose game-side shim has not landed.
    local threat = ge.threat_at(wp.x, wp.y, wp.z, THREAT_RADIUS)
    if threat > 0 and held < MAX_HOLD then
        held = held + 1
        if held % 30 == 1 then
            ge.log(string.format("route_bot: holding, waypoint %d contested by %d (%d/%d)",
                                 s.to, threat, held, MAX_HOLD))
        end
 -- Post neutral rather than posting nothing. A slot that goes quiet falls back to whatever
 -- was held, so the bot would keep walking while believing it had stopped.
        ge.post_input(SLOT, 0, 0, 0)
        return
    end
    if held > 0 then
        ge.log(string.format("route_bot: advancing on waypoint %d after %d frames held",
                             s.to, held))
        held = 0
    end

    if not haveHeading then
 -- No heading yet: walk forward to make one. Steering on an unknown heading sends the bot
 -- somewhere random and then estimates from that.
        ge.post_input(SLOT, 0, WALK, 0)
        return
    end

    local bearing = math.deg(math.atan(dx, dz))
    local err = norm180(bearing - heading)

    local sx = err * TURN_GAIN
    if sx > STICK_MAX then sx = STICK_MAX end
    if sx < -STICK_MAX then sx = -STICK_MAX end

 -- Forward speed scaled DOWN by heading error. This is necessary, not decoration: turning
 -- radius is speed over turn rate, about 114 units at full speed, so a bot that walks flat out
 -- while turning cannot get inside a 120-unit arrival radius and orbits its own waypoint.
 -- tools/routesim.py measures that as 29 of 61 routes failed with this removed.
    local align = 1.0 - math.abs(err) / ALIGN_DEG
    if align < 0 then align = 0 end

    if not ge.post_input(SLOT, math.floor(sx), math.floor(WALK * align), 0) then
 -- Refused means the post was for a tick that had already run. Worth saying once: in
 -- netplay the same condition is a desync, and a bot that silently stops acting looks
 -- like a policy bug rather than a timing one.
        if not announced then
            ge.log("route_bot: post refused -- posting into the past")
            announced = true
        end
    end

    if frame % 120 == 0 then
        local near = ge.guards_near(st.x, st.y, st.z, 700)
        ge.log(string.format("route_bot: step %d/%d dist=%.0f err=%.0f guards_near=%d",
                             step, steps, dist, err, #near))
    end
end
