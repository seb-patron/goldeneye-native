/* Lua mod scripting.
 *
 * Why a script host at all. Every knob this port has added so far is a GETV_ environment
 * gate or a goldeneye.cfg key, and there are around 275 of them. That surface is fine for
 * toggling behaviour someone already wrote in C, but it cannot express behaviour nobody
 * wrote: it has no way to say "when this player spawns, put them over there", or "count
 * how often that weapon fires". A mod that needs new logic currently needs a rebuild of
 * the whole tree, which is a 900-plus translation unit compile and an asset pipeline run.
 * Lua removes the rebuild from that loop.
 *
 * Why Lua specifically. It is one small ANSI C library (32 objects, 388K static on
 * arm64), it is MIT licensed so it does not disturb the licence position documented in
 * docs/LICENSING.md, and it has no build system requirements of its own -- the sources
 * compile with the same flags as everything else here. deps/ is not tracked (see
 * .gitignore), so the source is fetched by tools/fetch_lua.sh rather than vendored, the
 * same arrangement SDL2 already uses.
 *
 * What this is not. It is not a path to rewriting the game in Lua, and the API below is
 * deliberately small and read-mostly. The game's own state stays authoritative; scripts
 * observe it and are handed explicit events. Everything here is off unless mods are
 * present, and a script error disables that one mod rather than taking the process down,
 * because a syntax error in somebody's mod must not look like a crash in GoldenEye.
 *
 * Layout, matching the structure requested in the design notes:
 *
 *     mods/
 *       goldeneye_camera/
 *         mod.lua
 *
 * Each mod.lua is loaded in its own right and may define any of the hooks:
 *
 *     function onFrame(frame)          end
 *     function onPlayerSpawn(player)   end
 *     function onWeaponFire(weapon)    end
 *
 * A mod that defines none of them is still useful: the chunk body itself runs once at
 * load, which is enough for one-shot configuration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GE_WITH_LUA)

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#ifndef GE_LUA_MAX_MODS
#define GE_LUA_MAX_MODS 32
#endif

/* One lua_State shared by every mod, not one per mod. Two reasons: the hook dispatch
 * below has to walk the loaded mods every frame and a single state keeps that to one
 * registry lookup per mod, and mods that want to cooperate can do so through a shared
 * global table. The cost is that one mod can stamp on another's globals, which is the
 * normal trade every scripting host of this shape makes. Each mod's hooks are held in the
 * registry by reference rather than by name, so a later mod defining onFrame does not
 * silently replace an earlier mod's onFrame. */
static lua_State *ge_L;

struct ge_mod {
    char name[64];
    int  ref_frame;        /* LUA_NOREF when the mod does not define the hook */
    int  ref_spawn;
    int  ref_fire;
    int  ref_event;        /* onEvent(name, a, b, c) -- the bus in ge_event.h */
    int  disabled;         /* set after a runtime error, so it fires once not every frame */
};

static struct ge_mod ge_mods[GE_LUA_MAX_MODS];
static int ge_mod_count;
static int ge_mod_off_count;    /* found on disk but disabled via GETV_MODS_OFF */
static int ge_lua_ready;

/* ---------------------------------------------------------------- game accessors
 *
 * Read as plain externs, the same way gePortStateDump() in objective_status.c does, so
 * that adding a script host does not pull game headers into the port layer or the port's
 * headers into the game. If one of these signatures ever drifts, the link fails loudly
 * rather than the script quietly reading the wrong memory. */
extern int getPlayerCount(void);
extern int bossGetStageNum(void);

#include "ge_postfx.h"
#include "ge_player_api.h"
#include "ge_world_api.h"
#include "ge_world_levels.h"    /* generated: stage number -> extractor level name */
#include "ge_event.h"

static int ge_l_log(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    printf("[getv][lua] %s\n", s);
    return 0;
}

static int ge_l_stage(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) bossGetStageNum());
    return 1;
}

static int ge_l_player_count(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) getPlayerCount());
    return 1;
}

/* ge.player_pos(i) -> x, y, z   (nil when that player does not exist)
 *
 * g_playerPointers is the stable array; g_CurrentPlayer is not usable here because it is
 * swapped as each viewport is drawn, so in split screen it names whichever player was
 * rendered last. That distinction already cost a wrong reading once, in the co-op state
 * dump, and it is repeated here so the script API cannot inherit the same mistake. */
static int ge_l_player_pos(lua_State *L)
{
    /* Implemented game-side, next to gePortStateDump(), because that is where struct
     * player is visible. Reaching into the struct from here would mean hardcoding the
     * byte offset of pos, which differs between the N64 layout and this one and would
     * rot silently the first time a field above it changed. */
    extern int gePortPlayerPos(int idx, float *out);
    lua_Integer idx = luaL_checkinteger(L, 1);
    float pos[3];

    if (idx < 0 || idx > 3) {
        return luaL_error(L, "player index %d out of range 0..3", (int) idx);
    }
    if (!gePortPlayerPos((int) idx, pos)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number) pos[0]);
    lua_pushnumber(L, (lua_Number) pos[1]);
    lua_pushnumber(L, (lua_Number) pos[2]);
    return 3;
}

/* ge.postfx{ crt = true, scanline = 0.28, ... } -> the whole table back
 *
 * The first piece of the API that WRITES rather than reads, and the reason the CRT ships as
 * a mod: mods/crt_screen is a real consumer of a general post-process pass rather than a
 * special case inside the renderer.
 *
 * Every field is optional and unmentioned fields keep their current value, so a mod can set
 * one number without having to restate the other six. Called with no table it just reports,
 * which is what makes `print(ge.postfx().crt)` a usable way to see what is active. */
static int ge_l_postfx(lua_State *L)
{
    GePostfx fx = *gePostfxGet();

    if (lua_gettop(L) >= 1 && lua_istable(L, 1)) {
        /* A flag accepts a boolean or a number, because `crt = 1` is what someone coming
         * from goldeneye.cfg will write and refusing it would be pedantry. */
#define GE_FX_FLAG(name, dst)                                                       \
        do {                                                                        \
            lua_getfield(L, 1, name);                                               \
            if (!lua_isnil(L, -1)) {                                                \
                (dst) = lua_isboolean(L, -1) ? lua_toboolean(L, -1)                 \
                                             : (lua_tonumber(L, -1) != 0);          \
            }                                                                       \
            lua_pop(L, 1);                                                          \
        } while (0)
#define GE_FX_NUM(name, dst)                                                        \
        do {                                                                        \
            lua_getfield(L, 1, name);                                               \
            if (!lua_isnil(L, -1)) { (dst) = (float) lua_tonumber(L, -1); }         \
            lua_pop(L, 1);                                                          \
        } while (0)

        GE_FX_FLAG("crt",      fx.crt);
        GE_FX_FLAG("fxaa",     fx.fxaa);
        GE_FX_NUM ("scanline", fx.scanline);
        GE_FX_NUM ("mask",     fx.mask);
        GE_FX_NUM ("curve",    fx.curve);
        GE_FX_NUM ("vignette", fx.vignette);
        GE_FX_NUM ("lines",    fx.lines);
#undef GE_FX_FLAG
#undef GE_FX_NUM

        gePostfxSet(&fx);
        fx = *gePostfxGet();      /* read back, so the caller sees the clamped values */
    }

    lua_newtable(L);
    lua_pushboolean(L, fx.crt);       lua_setfield(L, -2, "crt");
    lua_pushboolean(L, fx.fxaa);      lua_setfield(L, -2, "fxaa");
    lua_pushnumber(L, fx.scanline);   lua_setfield(L, -2, "scanline");
    lua_pushnumber(L, fx.mask);       lua_setfield(L, -2, "mask");
    lua_pushnumber(L, fx.curve);      lua_setfield(L, -2, "curve");
    lua_pushnumber(L, fx.vignette);   lua_setfield(L, -2, "vignette");
    lua_pushnumber(L, fx.lines);      lua_setfield(L, -2, "lines");
    return 1;
}

/* ---------------------------------------------------------------- world knowledge
 *
 * The two API seams, handed to scripts. This is what makes the mod host a platform rather than
 * a way to tint the screen: a mod pack can now ask where the objectives are, which waypoints
 * lead to them and what is usually dangerous on the way -- and it can drive a player slot.
 *
 * The world data is loaded on first use, from the stage the game reports. A script never has to
 * be told which level it is in.
 */
static int ge_world_ready(void)
{
    extern int bossGetStageNum(void);
    int stage, i;

    if (geWorldLoaded()) { return 1; }
    stage = bossGetStageNum();
    for (i = 0; i < GE_WORLD_STAGE_COUNT; i++) {
        if (ge_world_stage_names[i].stage == stage) {
            return geWorldLoad(ge_world_stage_names[i].level);
        }
    }
    return 0;
}

/* ge.world() -> level name, or nil when this stage has no extracted knowledge.
 *
 * Four levels have no background model and several have no routed objectives, so a script must
 * cope with knowing nothing. Returning nil rather than an empty string makes that a condition a
 * script can test rather than a value it might use by accident. */
static int ge_l_world(lua_State *L)
{
    if (!ge_world_ready()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, geWorldLevel());
    return 1;
}

/* ge.objectives() -> count */
static int ge_l_objectives(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) (ge_world_ready() ? geWorldObjectiveCount() : 0));
    return 1;
}

/* ge.objective(i) -> table, or nil. steps == 0 means it exists but cannot be routed to. */
static int ge_l_objective(lua_State *L)
{
    GeWorldObjective ob;
    int i = (int) luaL_checkinteger(L, 1);
    if (!ge_world_ready() || !geWorldObjective(i, &ob)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, ob.index);          lua_setfield(L, -2, "index");
    lua_pushinteger(L, ob.min_difficulty); lua_setfield(L, -2, "difficulty");
    lua_pushinteger(L, ob.targets);        lua_setfield(L, -2, "targets");
    lua_pushinteger(L, ob.steps);          lua_setfield(L, -2, "steps");
    lua_pushnumber(L, (lua_Number) ob.tx); lua_setfield(L, -2, "x");
    lua_pushnumber(L, (lua_Number) ob.ty); lua_setfield(L, -2, "y");
    lua_pushnumber(L, (lua_Number) ob.tz); lua_setfield(L, -2, "z");
    return 1;
}

/* ge.route_step(objective, n) -> table, or nil past the end of the route. */
static int ge_l_route_step(lua_State *L)
{
    GeWorldStep st;
    int obj = (int) luaL_checkinteger(L, 1);
    int n   = (int) luaL_checkinteger(L, 2);
    if (!ge_world_ready() || !geWorldRouteStep(obj, n, &st)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, st.from);                 lua_setfield(L, -2, "from");
    lua_pushinteger(L, st.to);                   lua_setfield(L, -2, "to");
    lua_pushnumber(L, (lua_Number) st.distance); lua_setfield(L, -2, "distance");
    lua_pushnumber(L, (lua_Number) st.heading);  lua_setfield(L, -2, "heading");
    lua_pushnumber(L, (lua_Number) st.turn);     lua_setfield(L, -2, "turn");
    lua_pushinteger(L, st.threats);              lua_setfield(L, -2, "threats");
    return 1;
}

/* ge.waypoint(id) -> table, or nil. A route step names waypoints; this says where they are. */
static int ge_l_waypoint(lua_State *L)
{
    GeWorldWaypoint w;
    int id = (int) luaL_checkinteger(L, 1);
    if (!ge_world_ready() || !geWorldWaypointById(id, &w)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, w.id);            lua_setfield(L, -2, "id");
    lua_pushinteger(L, w.room);          lua_setfield(L, -2, "room");
    lua_pushnumber(L, (lua_Number) w.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, (lua_Number) w.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, (lua_Number) w.z); lua_setfield(L, -2, "z");
    return 1;
}

/* ge.waypoint_near(x, y, z) -> table, or nil. Turns a position into something routable. */
static int ge_l_waypoint_near(lua_State *L)
{
    GeWorldWaypoint w;
    float x = (float) luaL_checknumber(L, 1);
    float y = (float) luaL_checknumber(L, 2);
    float z = (float) luaL_checknumber(L, 3);
    if (!ge_world_ready() || !geWorldNearestWaypoint(x, y, z, &w)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, w.id);              lua_setfield(L, -2, "id");
    lua_pushinteger(L, w.room);            lua_setfield(L, -2, "room");
    lua_pushnumber(L, (lua_Number) w.x);   lua_setfield(L, -2, "x");
    lua_pushnumber(L, (lua_Number) w.y);   lua_setfield(L, -2, "y");
    lua_pushnumber(L, (lua_Number) w.z);   lua_setfield(L, -2, "z");
    return 1;
}

/* ge.guards_near(x, y, z, radius) -> array of tables, nearest first.
 *
 * STATIC PLACEMENT, NOT LIVE POSITIONS. This says where guards start and usually are, which is
 * what a route planner wants. It is not who is shooting at you; that needs live character state
 * the port cannot yet read. Naming it guards_near rather than enemies would be a lie in the
 * other direction, so the docstring carries the distinction instead. */
static int ge_l_guards_near(lua_State *L)
{
    GeWorldGuard g[16];
    int n, i;
    float x = (float) luaL_checknumber(L, 1);
    float y = (float) luaL_checknumber(L, 2);
    float z = (float) luaL_checknumber(L, 3);
    float r = (float) luaL_optnumber(L, 4, 700.0);

    lua_newtable(L);
    if (!ge_world_ready()) { return 1; }
    n = geWorldGuardsNear(x, y, z, r, g, (int) (sizeof g / sizeof g[0]));
    for (i = 0; i < n; i++) {
        lua_newtable(L);
        lua_pushinteger(L, g[i].chrnum);          lua_setfield(L, -2, "chrnum");
        lua_pushinteger(L, g[i].room);            lua_setfield(L, -2, "room");
        lua_pushnumber(L, (lua_Number) g[i].x);   lua_setfield(L, -2, "x");
        lua_pushnumber(L, (lua_Number) g[i].y);   lua_setfield(L, -2, "y");
        lua_pushnumber(L, (lua_Number) g[i].z);   lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ge.player_state(slot) -> table, or nil for an empty slot.
 *
 * Only the fields the game can actually report are present. Position is there; angle, health,
 * weapon and score are not, because those accessors do not exist yet. A field is ABSENT rather
 * than zero on purpose -- a script can test for it, where a zero would be indistinguishable
 * from a real reading of zero health. */
static int ge_l_player_state(lua_State *L)
{
    GePlayerState st;
    int slot = (int) luaL_checkinteger(L, 1);
    if (!gePlayerStateGet(slot, &st) || !st.present) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    if (st.fields & GE_ST_POSITION) {
        lua_pushnumber(L, (lua_Number) st.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, (lua_Number) st.y); lua_setfield(L, -2, "y");
        lua_pushnumber(L, (lua_Number) st.z); lua_setfield(L, -2, "z");
    }
    if (st.fields & GE_ST_ANGLE)  { lua_pushnumber(L, (lua_Number) st.angle);
                                    lua_setfield(L, -2, "angle"); }
    if (st.fields & GE_ST_ROOM)   { lua_pushinteger(L, st.room);
                                    lua_setfield(L, -2, "room"); }
    if (st.fields & GE_ST_HEALTH) { lua_pushnumber(L, (lua_Number) st.health);
                                    lua_setfield(L, -2, "health");
                                    lua_pushnumber(L, (lua_Number) st.armour);
                                    lua_setfield(L, -2, "armour"); }
    return 1;
}

/* ge.post_input(slot, stick_x, stick_y, buttons) -> true if accepted.
 *
 * A mod pack can drive a player slot, which is how a bot written entirely in Lua becomes
 * possible. It goes through the same seam as the C bots and a network peer, so it inherits the
 * same tick discipline: posts are for the NEXT tick, and a post for a tick that has already run
 * is refused rather than silently applied late. A false return is that refusal and is worth
 * reporting, not swallowing.
 *
 * Claiming the slot on first use is deliberate: two things posting into one slot would fight,
 * and the claim makes the conflict visible instead of producing a bot that stutters. */
static int ge_l_post_input(lua_State *L)
{
    GePlayerInput in;
    int slot = (int) luaL_checkinteger(L, 1);
    int sx   = (int) luaL_optinteger(L, 2, 0);
    int sy   = (int) luaL_optinteger(L, 3, 0);
    unsigned int btn = (unsigned int) luaL_optinteger(L, 4, 0);

    if (slot < 0 || slot >= GE_MAX_SLOTS) { lua_pushboolean(L, 0); return 1; }
    if (sx > 80) { sx = 80; } if (sx < -80) { sx = -80; }
    if (sy > 80) { sy = 80; } if (sy < -80) { sy = -80; }

    gePlayerApiInit();
    if (gePlayerSource(slot) != GE_SLOT_INJECTED) { gePlayerClaim(slot, GE_SLOT_INJECTED); }

    memset(&in, 0, sizeof in);
    in.stick_x = (signed char) sx;
    in.stick_y = (signed char) sy;
    in.buttons = btn;
    lua_pushboolean(L, gePlayerPost(slot, gePlayerTick() + 1, &in, 1));
    return 1;
}

static const luaL_Reg ge_api[] = {
    { "log",          ge_l_log },
    { "stage",        ge_l_stage },
    { "player_count", ge_l_player_count },
    { "player_pos",   ge_l_player_pos },
    { "postfx",       ge_l_postfx },
    /* World knowledge and slot control: the platform surface. */
    { "world",         ge_l_world },
    { "objectives",    ge_l_objectives },
    { "objective",     ge_l_objective },
    { "route_step",    ge_l_route_step },
    { "waypoint",      ge_l_waypoint },
    { "waypoint_near", ge_l_waypoint_near },
    { "guards_near",   ge_l_guards_near },
    { "player_state",  ge_l_player_state },
    { "post_input",    ge_l_post_input },
    { NULL, NULL }
};

/* ---------------------------------------------------------------- loading */

static int ge_take_hook(lua_State *L, const char *name)
{
    int ref;

    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops the function */
    /* Clear the global so the next mod's chunk starts from a clean slate and cannot
     * inherit this mod's hook by accident. */
    lua_pushnil(L);
    lua_setglobal(L, name);
    return ref;
}

static void ge_load_mod(const char *dir, const char *name)
{
    char path[512];
    struct ge_mod *m;

    if (ge_mod_count >= GE_LUA_MAX_MODS) {
        printf("[getv][lua] mod limit %d reached, ignoring \"%s\"\n", GE_LUA_MAX_MODS, name);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s/mod.lua", dir, name);

    if (luaL_loadfile(ge_L, path) != LUA_OK) {
        printf("[getv][lua] %s: %s\n", name, lua_tostring(ge_L, -1));
        lua_pop(ge_L, 1);
        return;
    }
    if (lua_pcall(ge_L, 0, 0, 0) != LUA_OK) {
        printf("[getv][lua] %s: %s\n", name, lua_tostring(ge_L, -1));
        lua_pop(ge_L, 1);
        return;
    }

    m = &ge_mods[ge_mod_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->ref_frame = ge_take_hook(ge_L, "onFrame");
    m->ref_spawn = ge_take_hook(ge_L, "onPlayerSpawn");
    m->ref_fire  = ge_take_hook(ge_L, "onWeaponFire");
    m->ref_event = ge_take_hook(ge_L, "onEvent");
    ge_mod_count++;

    printf("[getv][lua] loaded \"%s\"%s%s%s\n", name,
           m->ref_frame != LUA_NOREF ? " onFrame" : "",
           m->ref_spawn != LUA_NOREF ? " onPlayerSpawn" : "",
           m->ref_fire  != LUA_NOREF ? " onWeaponFire" : "");
}

/* dirent is POSIX and both hosts have it; there is no Windows branch here yet, which is
 * deliberate -- docs/PORTING.md tracks the platform layer, and adding a half-tested
 * FindFirstFile path now would claim support that has not been run. */
#include <dirent.h>
#include <sys/stat.h>

/* GETV_MODS_OFF -- a comma-separated list of mod directory names NOT to load.
 *
 * A DENYLIST rather than an allowlist, deliberately. The documented contract (wiki/Lua-mods.md)
 * is "drop a directory under mods/ with a mod.lua in it and it loads at startup", and an
 * allowlist would quietly break that: every newly dropped mod would be off until someone
 * remembered to add it. With a denylist the contract holds, unset means "load everything"
 * exactly as before, and turning a mod off is the only thing that has to be recorded.
 *
 * Names are matched whole, so "hello" does not disable "hello_goldeneye". Leading and
 * trailing spaces are ignored so a hand-edited goldeneye.cfg with ", " separators works. */
static int ge_mod_disabled(const char *name)
{
    static const char *list = (const char *) 1;
    const char *p;
    size_t n;

    if (list == (const char *) 1) list = getenv("GETV_MODS_OFF");
    if (list == NULL || *list == '\0') return 0;

    n = strlen(name);
    p = list;
    while (*p != '\0') {
        const char *end;
        size_t len;

        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        end = p;
        while (*end != '\0' && *end != ',') end++;
        len = (size_t) (end - p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;

        if (len == n && strncmp(p, name, n) == 0) return 1;
        p = (*end == ',') ? end + 1 : end;
    }
    return 0;
}

/* Does this directory actually hold a mod? Without the check, every stray subdirectory under
 * mods/ reaches luaL_loadfile and reports "cannot open .../mod.lua" as though something were
 * broken. A directory with no mod.lua is not an error, it is not a mod. */
static int ge_dir_has_mod(const char *dir, const char *name)
{
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s/mod.lua", dir, name);
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

/* Declared here because the definition sits ~75 lines below its first use. GCC demotes an
 * implicit declaration to a warning and Clang makes it an error, so this compiled on the
 * Surface and not here -- the same shape as the <sys/stat.h> and self_path bugs in
 * ge_launcher.cpp. Declaring it keeps the definition next to its siblings. */
static void ge_lua_on_event(GeEventType type, int a, int b, int c, void *user);

void gePortLuaInit(void)
{
    const char *dir = getenv("GETV_MODDIR");
    DIR *d;
    struct dirent *e;

    if (ge_lua_ready) return;
    if (dir == NULL || *dir == '\0') dir = "mods";

    d = opendir(dir);

    /* Relative paths are resolved against the working directory, which is not where the mods
     * are. A distributed folder launched from a shortcut, or from a terminal anywhere other
     * than the install directory, has a working directory with no mods/ in it, so every mod
     * silently failed to load -- the same cause as the config file being missed in
     * ge_config.c, and found the same way, by running the staged dist from elsewhere.
     *
     * GETV_EXEDIR is published by ge_config.c, which is the one place that sees argv[0].
     * Second, not first: an explicit relative GETV_MODDIR should still mean what the user
     * typed when they are standing in the right directory. */
    if (d == NULL && dir[0] != '/' && dir[0] != '\\' &&
        !(dir[0] != '\0' && dir[1] == ':')) {
        const char *exedir = getenv("GETV_EXEDIR");
        if (exedir != NULL && *exedir != '\0') {
            static char alt[1024];
            snprintf(alt, sizeof(alt), "%s/%s", exedir, dir);
            d = opendir(alt);
            if (d != NULL) dir = alt;
        }
    }

    if (d == NULL) return;      /* no mods directory is the normal case, and is silent */

    ge_L = luaL_newstate();
    if (ge_L == NULL) {
        closedir(d);
        printf("[getv][lua] out of memory creating interpreter\n");
        return;
    }
    luaL_openlibs(ge_L);

    /* The API goes in a global table named `ge`. Short, and unlikely to collide with
     * anything a mod author would reach for. */
    luaL_newlib(ge_L, ge_api);
    lua_setglobal(ge_L, "ge");

    /* Subscribe once, here rather than per mod: the bridge iterates mods itself, so one
     * subscription serves all of them and a mod loading later needs no extra wiring. */
    geEventSubscribe(ge_lua_on_event, NULL);

    while ((e = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;

        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!ge_dir_has_mod(dir, e->d_name)) continue;
        if (ge_mod_disabled(e->d_name)) {
            /* Named, not silent. "I disabled it and it still ran" and "I enabled it and it
             * did not" are the two questions this line has to be able to answer. */
            printf("[getv][lua] skipping \"%s\" (disabled)\n", e->d_name);
            ge_mod_off_count++;
            continue;
        }
        ge_load_mod(dir, e->d_name);
    }
    closedir(d);

    if (ge_mod_count == 0) {
        if (ge_mod_off_count > 0) {
            printf("[getv][lua] 0 mods active from \"%s\" (%d disabled)\n",
                   dir, ge_mod_off_count);
        }
        lua_close(ge_L);
        ge_L = NULL;
        return;
    }
    ge_lua_ready = 1;
    printf("[getv][lua] %d mod%s active from \"%s\"",
           ge_mod_count, ge_mod_count == 1 ? "" : "s", dir);
    if (ge_mod_off_count > 0) printf(" (%d disabled)", ge_mod_off_count);
    printf("\n");
}

/* ---------------------------------------------------------------- dispatch
 *
 * A mod that raises an error is reported once and then skipped for the rest of the run.
 * The alternative -- reporting every frame -- turns one bad line into tens of thousands
 * of lines of output and buries whatever else was being measured. */
static void ge_call(struct ge_mod *m, int ref, int argc)
{
    if (lua_pcall(ge_L, argc, 0, 0) != LUA_OK) {
        printf("[getv][lua] %s disabled: %s\n", m->name, lua_tostring(ge_L, -1));
        lua_pop(ge_L, 1);
        m->disabled = 1;
    }
    (void) ref;
}

void gePortLuaFrame(int frame)
{
    int i;

    if (!ge_lua_ready) return;
    for (i = 0; i < ge_mod_count; i++) {
        struct ge_mod *m = &ge_mods[i];
        if (m->disabled || m->ref_frame == LUA_NOREF) continue;
        lua_rawgeti(ge_L, LUA_REGISTRYINDEX, m->ref_frame);
        lua_pushinteger(ge_L, (lua_Integer) frame);
        ge_call(m, m->ref_frame, 1);
    }
}

/* The event bus, bridged to mods as onEvent(name, a, b, c).
 *
 * One hook rather than one per event type: the type set grows as game-side publish sites land,
 * and a mod written today should keep working when it does. A mod that cares about one event
 * tests the name, which is cheaper than this file gaining a field and a dispatcher per type.
 *
 * This runs INSIDE the frame hook, so a mod must not post input from here -- the tick it would
 * post for is still being assembled, and gePlayerPost would rightly refuse it. mods/route_bot
 * posts from onFrame for exactly that reason. */
static void ge_lua_on_event(GeEventType type, int a, int b, int c, void *user)
{
    int i;
    (void) user;
    if (!ge_lua_ready) { return; }
    for (i = 0; i < ge_mod_count; i++) {
        struct ge_mod *m = &ge_mods[i];
        if (m->disabled || m->ref_event == LUA_NOREF) continue;
        lua_rawgeti(ge_L, LUA_REGISTRYINDEX, m->ref_event);
        lua_pushstring(ge_L, geEventName(type));
        lua_pushinteger(ge_L, (lua_Integer) a);
        lua_pushinteger(ge_L, (lua_Integer) b);
        lua_pushinteger(ge_L, (lua_Integer) c);
        ge_call(m, m->ref_event, 4);
    }
}

void gePortLuaPlayerSpawn(int player)
{
    int i;

    if (!ge_lua_ready) return;
    for (i = 0; i < ge_mod_count; i++) {
        struct ge_mod *m = &ge_mods[i];
        if (m->disabled || m->ref_spawn == LUA_NOREF) continue;
        lua_rawgeti(ge_L, LUA_REGISTRYINDEX, m->ref_spawn);
        lua_pushinteger(ge_L, (lua_Integer) player);
        ge_call(m, m->ref_spawn, 1);
    }
}

void gePortLuaWeaponFire(int weapon)
{
    int i;

    if (!ge_lua_ready) return;
    for (i = 0; i < ge_mod_count; i++) {
        struct ge_mod *m = &ge_mods[i];
        if (m->disabled || m->ref_fire == LUA_NOREF) continue;
        lua_rawgeti(ge_L, LUA_REGISTRYINDEX, m->ref_fire);
        lua_pushinteger(ge_L, (lua_Integer) weapon);
        ge_call(m, m->ref_fire, 1);
    }
}

#else  /* !GE_WITH_LUA */

/* Built without Lua. The hook points stay in the game and the port layer unconditionally,
 * so that enabling scripting is a build flag rather than a source change, and so the call
 * sites do not sprout #ifdefs. These are empty and the optimiser removes the calls. */
void gePortLuaInit(void) { }
void gePortLuaFrame(int frame) { (void) frame; }
void gePortLuaPlayerSpawn(int player) { (void) player; }
void gePortLuaWeaponFire(int weapon) { (void) weapon; }

#endif /* GE_WITH_LUA */
