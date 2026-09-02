/* Initial read-only developer-console commands.  See ge_console_commands.h. */
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "ge_console_commands.h"

#define GE_CONSOLE_TITLE_STAGE 90

static GeConsoleReadProvider ge_read_provider;

static int ge_read_equal_ci(const char *a, const char *b)
{
    size_t i;
    if (a == NULL || b == NULL) { return 0; }
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = (unsigned char)(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = (unsigned char)(cb - 'A' + 'a'); }
        if (ca != cb) { return 0; }
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int ge_read_message_append(GeConsoleReply *reply, const char *format, ...)
{
    char addition[GE_CONSOLE_MAX_MESSAGE];
    size_t used, add;
    int n;
    va_list ap;

    if (reply == NULL || format == NULL) { return 0; }
    used = strlen(reply->message);
    va_start(ap, format);
    n = vsnprintf(addition, sizeof addition, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof addition) { return 0; }
    add = (size_t)n;
    if (used + add >= sizeof reply->message) { return 0; }
    memcpy(reply->message + used, addition, add + 1u);
    return 1;
}

static void ge_read_message_truncated(GeConsoleReply *reply)
{
    size_t used;
    if (reply == NULL) { return; }
    used = strlen(reply->message);
    if (used + 4u < sizeof reply->message) {
        memcpy(reply->message + used, " ...", 5u);
    } else if (sizeof reply->message >= 4u) {
        memcpy(reply->message + sizeof reply->message - 4u, "...", 4u);
    }
}

static void ge_read_payload_integer(GeConsoleReply *reply, uint32_t field_id, int64_t value)
{
    GeConsolePayloadValue *payload;
    if (reply == NULL || reply->payload_count >= GE_CONSOLE_MAX_PAYLOAD_VALUES) { return; }
    payload = &reply->payload[reply->payload_count++];
    memset(payload, 0, sizeof *payload);
    payload->field_id = field_id;
    payload->value.type = GE_CONSOLE_ARG_INTEGER;
    payload->value.present = 1;
    payload->value.integer = value;
    payload->value.choice_index = -1;
}

static void ge_read_payload_boolean(GeConsoleReply *reply, uint32_t field_id, int value)
{
    GeConsolePayloadValue *payload;
    if (reply == NULL || reply->payload_count >= GE_CONSOLE_MAX_PAYLOAD_VALUES) { return; }
    payload = &reply->payload[reply->payload_count++];
    memset(payload, 0, sizeof *payload);
    payload->field_id = field_id;
    payload->value.type = GE_CONSOLE_ARG_BOOLEAN;
    payload->value.present = 1;
    payload->value.boolean = value ? 1 : 0;
    payload->value.choice_index = -1;
}

static int ge_read_scaled(float value, double scale, int64_t *out)
{
    double scaled;
    if (out == NULL || !isfinite((double)value)) { return 0; }
    scaled = (double)value * scale;
    if (scaled > (double)INT64_MAX || scaled < (double)INT64_MIN) { return 0; }
    *out = (int64_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    return 1;
}

static unsigned int ge_read_player_count(uint32_t mask)
{
    unsigned int count = 0;
    while (mask != 0) {
        count += mask & 1u;
        mask >>= 1u;
    }
    return count;
}

static void ge_read_context(GeConsoleExecutionContext *context, void *user)
{
    int slot;
    unsigned int count;
    (void)user;
    memset(context, 0, sizeof *context);

    context->stage_id = ge_read_provider.stage_id();
    if (context->stage_id >= 0) { context->flags |= GE_CONSOLE_CONTEXT_HAS_STAGE; }
    if (ge_read_provider.netplay_active()) { context->flags |= GE_CONSOLE_CONTEXT_NETPLAY; }

    for (slot = 0; slot < GE_MAX_SLOTS; slot++) {
        GePlayerState state;
        memset(&state, 0, sizeof state);
        if (ge_read_provider.player_state(slot, &state) && state.present) {
            context->player_mask |= 1u << (unsigned int)slot;
        }
    }
    count = ge_read_player_count(context->player_mask);
    if ((context->flags & GE_CONSOLE_CONTEXT_HAS_STAGE) &&
        context->stage_id != GE_CONSOLE_TITLE_STAGE && count > 0) {
        context->flags |= GE_CONSOLE_CONTEXT_MISSION_ACTIVE;
    }
    if (count == 1) { context->flags |= GE_CONSOLE_CONTEXT_SOLO; }
}

static int ge_read_find_command(const char *path, GeConsoleCommandSpec *out)
{
    unsigned int i, j;
    for (i = 0; i < geConsoleCommandCount(); i++) {
        GeConsoleCommandSpec spec;
        if (!geConsoleCommandAt(i, &spec)) { continue; }
        if (ge_read_equal_ci(path, spec.name)) {
            if (out != NULL) { *out = spec; }
            return 1;
        }
        for (j = 0; j < spec.alias_count; j++) {
            if (ge_read_equal_ci(path, spec.aliases[j])) {
                if (out != NULL) { *out = spec; }
                return 1;
            }
        }
    }
    return 0;
}

static void ge_read_help(const GeConsoleRequest *request,
                         const GeConsoleExecutionContext *context,
                         GeConsoleReply *reply, void *user)
{
    char path[GE_CONSOLE_MAX_COMMAND_PATH];
    GeConsoleCommandSpec spec;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_HELP;

    if (!request->arguments[0].present) {
        ge_read_message_append(reply, "help <command> -- inspect registered metadata; use commands to list names");
        return;
    }
    if (request->arguments[1].present) {
        int n = snprintf(path, sizeof path, "%s %s", request->arguments[0].text,
                         request->arguments[1].text);
        if (n < 0 || (size_t)n >= sizeof path) { path[0] = '\0'; }
    } else {
        int n = snprintf(path, sizeof path, "%s", request->arguments[0].text);
        if (n < 0 || (size_t)n >= sizeof path) { path[0] = '\0'; }
    }
    if (path[0] == '\0' || !ge_read_find_command(path, &spec)) {
        reply->status = GE_CONSOLE_STATUS_ARGUMENT_CHOICE;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "no registered command named '%s'", path[0] ? path : "?");
        return;
    }

    ge_read_message_append(reply, "%s -- %s", spec.name, spec.summary);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_COMMAND_ID, spec.command_id);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_COMMAND_VERSION, spec.schema_version);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_COMMAND_FLAGS, spec.flags);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_ARGUMENT_COUNT, spec.argument_count);
}

static void ge_read_commands(const GeConsoleRequest *request,
                             const GeConsoleExecutionContext *context,
                             GeConsoleReply *reply, void *user)
{
    unsigned int i, captured = 0, total = geConsoleCommandCount();
    int truncated = 0;
    (void)request;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_COMMANDS;
    ge_read_message_append(reply, "commands %u: ", total);
    for (i = 0; i < total; i++) {
        GeConsoleCommandSpec spec;
        if (!geConsoleCommandAt(i, &spec)) { continue; }
        if (!ge_read_message_append(reply, "%s%s", captured ? ", " : "", spec.name)) {
            truncated = 1;
            break;
        }
        captured++;
    }
    if (truncated) {
        ge_read_message_truncated(reply);
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
    }
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_TOTAL, total);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_CAPTURED, captured);
    ge_read_payload_boolean(reply, GE_CONSOLE_FIELD_TRUNCATED, truncated);
}

static int ge_read_platform(void)
{
#if defined(_WIN32)
    return GE_CONSOLE_PLATFORM_WINDOWS;
#elif defined(__linux__)
    return GE_CONSOLE_PLATFORM_LINUX;
#elif defined(__APPLE__) && defined(TARGET_OS_TV) && TARGET_OS_TV
    return GE_CONSOLE_PLATFORM_TVOS;
#elif defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS
    return GE_CONSOLE_PLATFORM_IOS;
#elif defined(__APPLE__)
    return GE_CONSOLE_PLATFORM_MACOS;
#else
    return GE_CONSOLE_PLATFORM_UNKNOWN;
#endif
}

static const char *ge_read_platform_name(int value)
{
    switch (value) {
    case GE_CONSOLE_PLATFORM_MACOS:   return "macos";
    case GE_CONSOLE_PLATFORM_IOS:     return "ios";
    case GE_CONSOLE_PLATFORM_TVOS:    return "tvos";
    case GE_CONSOLE_PLATFORM_LINUX:   return "linux";
    case GE_CONSOLE_PLATFORM_WINDOWS: return "windows";
    default:                          return "unknown";
    }
}

static int ge_read_architecture(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return GE_CONSOLE_ARCH_ARM64;
#elif defined(__x86_64__) || defined(_M_X64)
    return GE_CONSOLE_ARCH_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return GE_CONSOLE_ARCH_X86;
#elif defined(__arm__) || defined(_M_ARM)
    return GE_CONSOLE_ARCH_ARM32;
#else
    return GE_CONSOLE_ARCH_UNKNOWN;
#endif
}

static const char *ge_read_architecture_name(int value)
{
    switch (value) {
    case GE_CONSOLE_ARCH_ARM64:  return "arm64";
    case GE_CONSOLE_ARCH_X86_64: return "x86_64";
    case GE_CONSOLE_ARCH_X86:    return "x86";
    case GE_CONSOLE_ARCH_ARM32:  return "arm32";
    default:                     return "unknown";
    }
}

static int ge_read_renderer(void)
{
#if defined(RAPI_METAL)
    return GE_CONSOLE_RENDERER_METAL;
#elif defined(RAPI_GL)
    return GE_CONSOLE_RENDERER_OPENGL;
#else
    return GE_CONSOLE_RENDERER_UNKNOWN;
#endif
}

static const char *ge_read_renderer_name(int value)
{
    switch (value) {
    case GE_CONSOLE_RENDERER_OPENGL: return "OpenGL";
    case GE_CONSOLE_RENDERER_METAL:  return "Metal";
    default:                         return "unknown";
    }
}

static void ge_read_build(const GeConsoleRequest *request,
                          const GeConsoleExecutionContext *context,
                          GeConsoleReply *reply, void *user)
{
    int platform = ge_read_platform();
    int architecture = ge_read_architecture();
    int renderer = ge_read_renderer();
    (void)request;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_BUILD;
    ge_read_message_append(reply,
        "build platform=%s arch=%s renderer=%s console_schema=%u command_schema=%u",
        ge_read_platform_name(platform), ge_read_architecture_name(architecture),
        ge_read_renderer_name(renderer), (unsigned int)GE_CONSOLE_SCHEMA_VERSION,
        (unsigned int)GE_CONSOLE_READ_SCHEMA_VERSION);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_PLATFORM, platform);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_ARCHITECTURE, architecture);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_RENDERER, renderer);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_CONSOLE_SCHEMA, GE_CONSOLE_SCHEMA_VERSION);
}

static void ge_read_status(const GeConsoleRequest *request,
                           const GeConsoleExecutionContext *context,
                           GeConsoleReply *reply, void *user)
{
    unsigned int players = ge_read_player_count(context->player_mask);
    int difficulty = ge_read_provider.difficulty();
    int netplay = (context->flags & GE_CONSOLE_CONTEXT_NETPLAY) != 0;
    const char *mode = players == 1 ? "solo" : (players > 1 ? "multiplayer" : "none");
    (void)request;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_STATUS;
    ge_read_message_append(reply,
        "status stage=%d difficulty=%d players=%u mode=%s netplay=%s tick=%llu frame=%llu",
        context->stage_id, difficulty, players, mode, netplay ? "on" : "off",
        (unsigned long long)context->game_tick, (unsigned long long)context->render_frame);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_STAGE, context->stage_id);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_DIFFICULTY, difficulty);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_PLAYER_MASK, context->player_mask);
    ge_read_payload_boolean(reply, GE_CONSOLE_FIELD_NETPLAY, netplay);
}

static void ge_read_player_list(const GeConsoleRequest *request,
                                const GeConsoleExecutionContext *context,
                                GeConsoleReply *reply, void *user)
{
    int slot;
    unsigned int count = ge_read_player_count(context->player_mask);
    int first = 1;
    (void)request;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_PLAYER_LIST;
    ge_read_message_append(reply, "players %u mask=0x%x slots=", count, context->player_mask);
    for (slot = 0; slot < GE_MAX_SLOTS; slot++) {
        if (!(context->player_mask & (1u << (unsigned int)slot))) { continue; }
        ge_read_message_append(reply, "%s%d", first ? "" : ",", slot);
        first = 0;
    }
    if (first) { ge_read_message_append(reply, "none"); }
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_TOTAL, count);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_PLAYER_MASK, context->player_mask);
}

static void ge_read_player_show(const GeConsoleRequest *request,
                                const GeConsoleExecutionContext *context,
                                GeConsoleReply *reply, void *user)
{
    int slot = (int)request->arguments[0].integer;
    GePlayerState state;
    int64_t scaled;
    int truncated = 0;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_PLAYER_SHOW;
    reply->target_fields |= GE_CONSOLE_TARGET_PLAYER;
    reply->player_slot = slot;
    memset(&state, 0, sizeof state);
    if (!ge_read_provider.player_state(slot, &state) || !state.present) {
        reply->status = GE_CONSOLE_STATUS_REFUSED_PLAYER;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "player slot %d is unavailable", slot);
        return;
    }

    ge_read_message_append(reply, "player %d fields=0x%x", slot, state.fields);
    if (state.fields & GE_ST_POSITION) {
        truncated |= !ge_read_message_append(reply, " pos=(%.2f,%.2f,%.2f)",
            (double)state.x, (double)state.y, (double)state.z);
    } else {
        truncated |= !ge_read_message_append(reply, " pos=n/a");
    }
    if (state.fields & GE_ST_ROOM) {
        truncated |= !ge_read_message_append(reply, " room=%d", state.room);
    } else {
        truncated |= !ge_read_message_append(reply, " room=n/a");
    }
    if (state.fields & GE_ST_HEALTH) {
        truncated |= !ge_read_message_append(reply, " hp=%.3f armour=%.3f dead=%d",
            (double)state.health, (double)state.armour, state.dead ? 1 : 0);
    } else {
        truncated |= !ge_read_message_append(reply, " health=n/a");
    }
    if (state.fields & GE_ST_WEAPON) {
        truncated |= !ge_read_message_append(reply, " weapon=%d ammo=%d/%d", state.weapon,
            state.ammo_clip, state.ammo_reserve);
    } else {
        truncated |= !ge_read_message_append(reply, " weapon=n/a");
    }
    if (state.fields & GE_ST_SCORE) {
        truncated |= !ge_read_message_append(reply, " score=%d/%d/%d", state.kills,
            state.deaths, state.shots);
    }
    if (truncated) {
        ge_read_message_truncated(reply);
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
    }

    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_PLAYER_FIELDS, state.fields);
    if ((state.fields & GE_ST_HEALTH) && ge_read_scaled(state.health, 1000.0, &scaled)) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_HEALTH_MILLI, scaled);
    }
    if ((state.fields & GE_ST_HEALTH) && ge_read_scaled(state.armour, 1000.0, &scaled)) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_ARMOUR_MILLI, scaled);
    }
    if (state.fields & GE_ST_WEAPON) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_WEAPON, state.weapon);
    }
}

static void ge_read_where(const GeConsoleRequest *request,
                          const GeConsoleExecutionContext *context,
                          GeConsoleReply *reply, void *user)
{
    int slot = (int)request->arguments[0].integer;
    GePlayerState state;
    int64_t scaled;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_WHERE;
    reply->target_fields |= GE_CONSOLE_TARGET_PLAYER;
    reply->player_slot = slot;
    memset(&state, 0, sizeof state);
    if (!ge_read_provider.player_state(slot, &state) || !state.present) {
        reply->status = GE_CONSOLE_STATUS_REFUSED_PLAYER;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "player slot %d is unavailable", slot);
        return;
    }
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_PLAYER_FIELDS, state.fields);
    if (!(state.fields & GE_ST_POSITION) || !isfinite((double)state.x) ||
        !isfinite((double)state.y) || !isfinite((double)state.z)) {
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "player %d position unavailable", slot);
        return;
    }

    ge_read_message_append(reply, "player %d pos=(%.2f,%.2f,%.2f)", slot,
        (double)state.x, (double)state.y, (double)state.z);
    if (state.fields & GE_ST_ROOM) { ge_read_message_append(reply, " room=%d", state.room); }
    else { ge_read_message_append(reply, " room=n/a"); }
    if (state.fields & GE_ST_ANGLE) {
        ge_read_message_append(reply, " heading=%.2f", (double)state.angle);
    } else {
        ge_read_message_append(reply, " heading=n/a");
    }
    if (ge_read_scaled(state.x, 100.0, &scaled)) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_POSITION_X_CENTI, scaled);
    }
    if (ge_read_scaled(state.y, 100.0, &scaled)) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_POSITION_Y_CENTI, scaled);
    }
    if (ge_read_scaled(state.z, 100.0, &scaled)) {
        ge_read_payload_integer(reply, GE_CONSOLE_FIELD_POSITION_Z_CENTI, scaled);
    }
}

static const char *ge_read_objective_status_name(int status)
{
    switch (status) {
    case 0:  return "incomplete";
    case 1:  return "complete";
    case 2:  return "failed";
    default: return "unknown";
    }
}

static void ge_read_objective_list(const GeConsoleRequest *request,
                                   const GeConsoleExecutionContext *context,
                                   GeConsoleReply *reply, void *user)
{
    int total = ge_read_provider.objective_count();
    int captured, i;
    uint32_t present_mask = 0;
    uint32_t status_bits = 0;
    (void)request;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_OBJECTIVE_LIST;
    if (total < 0) {
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "objectives unavailable for stage %d", context->stage_id);
        total = 0;
    } else if (total == 0) {
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        ge_read_message_append(reply, "objectives: none reported for stage %d", context->stage_id);
    }
    captured = total < GE_CONSOLE_OBJECTIVE_CAPACITY ? total : GE_CONSOLE_OBJECTIVE_CAPACITY;
    if (total > 0) {
        if (captured < total) {
            ge_read_message_append(reply, "objectives showing %d/%d:", captured, total);
            reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        } else {
            ge_read_message_append(reply, "objectives %d:", total);
        }
    }
    for (i = 0; i < captured; i++) {
        int status = -1;
        int present = ge_read_provider.objective_status(i, &status);
        unsigned int encoded = 3u;
        if (present) {
            present_mask |= 1u << (unsigned int)i;
            if (status >= 0 && status <= 2) { encoded = (unsigned int)status; }
        }
        status_bits |= encoded << ((unsigned int)i * 2u);
        if (!ge_read_message_append(reply, " %d=%s", i,
                present ? ge_read_objective_status_name(status) : "unavailable")) {
            ge_read_message_truncated(reply);
            reply->severity = GE_CONSOLE_SEVERITY_WARNING;
            break;
        }
    }
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_TOTAL, total);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_CAPTURED, captured);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_OBJECTIVE_PRESENT, present_mask);
    ge_read_payload_integer(reply, GE_CONSOLE_FIELD_OBJECTIVE_STATUS, status_bits);
}

static void ge_read_spec(GeConsoleCommandSpec *spec, uint32_t command_id,
                         const char *name, const char *summary, uint32_t flags)
{
    memset(spec, 0, sizeof *spec);
    spec->command_id = command_id;
    spec->schema_version = GE_CONSOLE_READ_SCHEMA_VERSION;
    spec->handler_id = (uint16_t)command_id;
    spec->flags = flags;
    snprintf(spec->name, sizeof spec->name, "%s", name);
    snprintf(spec->summary, sizeof spec->summary, "%s", summary);
}

static GeConsoleStatus ge_read_register(GeConsoleCommandSpec *spec, GeConsoleHandler handler)
{
    return geConsoleRegister(spec, handler, NULL);
}

GeConsoleStatus geConsoleReadInstall(const GeConsoleReadProvider *provider)
{
    GeConsoleCommandSpec spec;
    GeConsoleStatus status;
    const uint32_t safe = GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE;
    const uint32_t player = safe | GE_CONSOLE_CMD_REQUIRES_MISSION |
                            GE_CONSOLE_CMD_REQUIRES_PLAYER;

    if (provider == NULL || provider->stage_id == NULL || provider->difficulty == NULL ||
        provider->netplay_active == NULL || provider->player_state == NULL ||
        provider->objective_count == NULL || provider->objective_status == NULL) {
        return GE_CONSOLE_STATUS_INVALID_DEFINITION;
    }
    ge_read_provider = *provider;

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_HELP, "help", "Show metadata for one command",
                 GE_CONSOLE_CMD_READ_ONLY);
    spec.argument_count = 2;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "command");
    spec.arguments[0].type = GE_CONSOLE_ARG_TEXT;
    snprintf(spec.arguments[1].name, sizeof spec.arguments[1].name, "subcommand");
    spec.arguments[1].type = GE_CONSOLE_ARG_TEXT;
    if ((status = ge_read_register(&spec, ge_read_help)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_COMMANDS, "commands", "List registered commands", safe);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "cmds");
    if ((status = ge_read_register(&spec, ge_read_commands)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_BUILD, "build", "Show platform, architecture and renderer", safe);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "version");
    if ((status = ge_read_register(&spec, ge_read_build)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_STATUS, "status", "Show current session facts", safe);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "st");
    if ((status = ge_read_register(&spec, ge_read_status)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_PLAYER_LIST, "player list", "List occupied player slots", safe);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "players");
    if ((status = ge_read_register(&spec, ge_read_player_list)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_PLAYER_SHOW, "player show", "Show available state for an explicit player slot", player);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "player");
    spec.argument_count = 1;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "slot");
    spec.arguments[0].type = GE_CONSOLE_ARG_PLAYER_SLOT;
    spec.arguments[0].required = 1;
    if ((status = ge_read_register(&spec, ge_read_player_show)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_WHERE, "where", "Show position, room and heading for an explicit player slot", player);
    spec.argument_count = 1;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "slot");
    spec.arguments[0].type = GE_CONSOLE_ARG_PLAYER_SLOT;
    spec.arguments[0].required = 1;
    if ((status = ge_read_register(&spec, ge_read_where)) != GE_CONSOLE_STATUS_OK) { return status; }

    ge_read_spec(&spec, GE_CONSOLE_COMMAND_OBJECTIVE_LIST, "objective list", "List bounded live objective status", safe | GE_CONSOLE_CMD_REQUIRES_MISSION);
    spec.alias_count = 1;
    snprintf(spec.aliases[0], sizeof spec.aliases[0], "objectives");
    if ((status = ge_read_register(&spec, ge_read_objective_list)) != GE_CONSOLE_STATUS_OK) { return status; }

    geConsoleSetContextProvider(ge_read_context, NULL);
    return GE_CONSOLE_STATUS_OK;
}
