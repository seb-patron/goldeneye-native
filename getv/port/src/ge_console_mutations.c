/* Initial state-changing developer-console commands. See ge_console_mutations.h. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_console_mutations.h"
#include "ge_gibs.h"

static GeConsoleMutationProvider ge_mutation_provider;

#define GE_MUTATION_MAX_AMMO 1000

typedef struct GeConsoleWeaponName {
    int id;
    const char *name;
} GeConsoleWeaponName;

/* ITEM_FIST through ITEM_TANKSHELLS are the game's usable inventory-weapon range.  Friendly
 * canonical names are stable console schema; aliases below retain the decomp's internal names.
 * Numeric input is the same ITEM_IDS value and is accepted only when it resolves in this table. */
static const GeConsoleWeaponName ge_mutation_weapons[] = {
    { 1, "fist" },              { 2, "knife" },
    { 3, "throwing_knife" },    { 4, "pp7" },
    { 5, "silenced_pp7" },      { 6, "dd44" },
    { 7, "klobb" },             { 8, "kf7" },
    { 9, "zmg" },               { 10, "d5k" },
    { 11, "silenced_d5k" },     { 12, "phantom" },
    { 13, "ar33" },             { 14, "rcp90" },
    { 15, "shotgun" },          { 16, "automatic_shotgun" },
    { 17, "sniper_rifle" },     { 18, "cougar_magnum" },
    { 19, "golden_gun" },       { 20, "silver_pp7" },
    { 21, "gold_pp7" },         { 22, "laser" },
    { 23, "watch_laser" },      { 24, "grenade_launcher" },
    { 25, "rocket_launcher" },  { 26, "grenade" },
    { 27, "timed_mine" },       { 28, "proximity_mine" },
    { 29, "remote_mine" },      { 30, "remote_detonator" },
    { 31, "taser" },            { 32, "tank_shells" }
};

static const GeConsoleWeaponName ge_mutation_weapon_aliases[] = {
    { 3, "throwknife" }, { 4, "wppk" }, { 5, "wppksil" }, { 6, "tt33" },
    { 7, "skorpion" }, { 8, "ak47" }, { 9, "uzi" }, { 10, "mp5k" },
    { 11, "mp5ksil" }, { 12, "spectre" }, { 13, "m16" }, { 14, "fnp90" },
    { 16, "autoshot" }, { 18, "ruger" }, { 19, "goldengun" },
    { 20, "silverwppk" }, { 21, "goldwppk" }, { 23, "watchlaser" },
    { 24, "grenadelaunch" }, { 25, "rocketlaunch" }, { 27, "timedmine" },
    { 28, "proximitymine" }, { 29, "remotemine" }, { 30, "trigger" },
    { 32, "tankshells" }
};

static int ge_mutation_token_equal(const char *left, const char *right)
{
    size_t i = 0;
    if (left == NULL || right == NULL) { return 0; }
    while (left[i] != '\0' && right[i] != '\0') {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a == '-') { a = '_'; }
        if (b == '-') { b = '_'; }
        if (a >= 'A' && a <= 'Z') { a = (unsigned char)(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = (unsigned char)(b - 'A' + 'a'); }
        if (a != b) { return 0; }
        i++;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static const GeConsoleWeaponName *ge_mutation_weapon_by_id(int id)
{
    size_t i;
    for (i = 0; i < sizeof ge_mutation_weapons / sizeof ge_mutation_weapons[0]; i++) {
        if (ge_mutation_weapons[i].id == id) { return &ge_mutation_weapons[i]; }
    }
    return NULL;
}

static const GeConsoleWeaponName *ge_mutation_weapon(const char *token)
{
    char *end = NULL;
    long id;
    size_t i;

    if (token == NULL) { return NULL; }
    errno = 0;
    id = strtol(token, &end, 10);
    if (token[0] != '\0' && end != NULL && *end == '\0' && errno != ERANGE &&
        id >= 1 && id <= 32) {
        return ge_mutation_weapon_by_id((int)id);
    }
    for (i = 0; i < sizeof ge_mutation_weapons / sizeof ge_mutation_weapons[0]; i++) {
        if (ge_mutation_token_equal(token, ge_mutation_weapons[i].name)) {
            return &ge_mutation_weapons[i];
        }
    }
    for (i = 0; i < sizeof ge_mutation_weapon_aliases / sizeof ge_mutation_weapon_aliases[0]; i++) {
        if (ge_mutation_token_equal(token, ge_mutation_weapon_aliases[i].name)) {
            return ge_mutation_weapon_by_id(ge_mutation_weapon_aliases[i].id);
        }
    }
    return NULL;
}

static const char *ge_mutation_gibs_name(int mode)
{
    switch (mode) {
    case GE_GIBS_OFF:         return "off";
    case GE_GIBS_EXPLOSIONS:  return "explosions";
    case GE_GIBS_HIGH_DAMAGE: return "high_damage";
    case GE_GIBS_ALWAYS:      return "always";
    default:                  return NULL;
    }
}

static void ge_mutation_payload_integer(GeConsoleReply *reply, uint32_t field_id, int64_t value)
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

static void ge_mutation_payload_boolean(GeConsoleReply *reply, uint32_t field_id, int value)
{
    GeConsolePayloadValue *payload;
    if (reply == NULL || reply->payload_count >= GE_CONSOLE_MAX_PAYLOAD_VALUES) { return; }
    payload = &reply->payload[reply->payload_count++];
    memset(payload, 0, sizeof *payload);
    payload->field_id = field_id;
    payload->value.type = GE_CONSOLE_ARG_BOOLEAN;
    payload->value.present = 1;
    payload->value.boolean = value != 0;
    payload->value.choice_index = -1;
    snprintf(payload->value.text, sizeof payload->value.text, "%s", value ? "on" : "off");
}

static void ge_mutation_payload_symbol(GeConsoleReply *reply, uint32_t field_id,
                                       const char *value)
{
    GeConsolePayloadValue *payload;
    if (reply == NULL || value == NULL ||
        reply->payload_count >= GE_CONSOLE_MAX_PAYLOAD_VALUES) { return; }
    payload = &reply->payload[reply->payload_count++];
    memset(payload, 0, sizeof *payload);
    payload->field_id = field_id;
    payload->value.type = GE_CONSOLE_ARG_SYMBOL;
    payload->value.present = 1;
    payload->value.choice_index = -1;
    snprintf(payload->value.text, sizeof payload->value.text, "%s", value);
}

static void ge_mutation_refused(GeConsoleReply *reply, const char *message)
{
    reply->status = GE_CONSOLE_STATUS_REFUSED_CONTEXT;
    reply->severity = GE_CONSOLE_SEVERITY_WARNING;
    snprintf(reply->message, sizeof reply->message, "%s", message);
}

static void ge_mutation_gibs(const GeConsoleRequest *request,
                             const GeConsoleExecutionContext *context,
                             GeConsoleReply *reply, void *user)
{
    int previous = ge_mutation_provider.gibs_mode();
    int requested = request->arguments[0].choice_index;
    int current;
    const char *previous_name = ge_mutation_gibs_name(previous);
    const char *requested_name = ge_mutation_gibs_name(requested);
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_GIBS;

    if (previous_name == NULL || requested_name == NULL) {
        reply->status = GE_CONSOLE_STATUS_HANDLER_ERROR;
        reply->severity = GE_CONSOLE_SEVERITY_ERROR;
        snprintf(reply->message, sizeof reply->message,
                 "gibs policy provider returned invalid state");
        return;
    }
    if (!ge_mutation_provider.set_gibs_mode(requested)) {
        reply->status = GE_CONSOLE_STATUS_HANDLER_ERROR;
        reply->severity = GE_CONSOLE_SEVERITY_ERROR;
        snprintf(reply->message, sizeof reply->message, "gibs policy refused %s", requested_name);
        return;
    }

    current = ge_mutation_provider.gibs_mode();
    if (current != requested) {
        reply->status = GE_CONSOLE_STATUS_HANDLER_ERROR;
        reply->severity = GE_CONSOLE_SEVERITY_ERROR;
        snprintf(reply->message, sizeof reply->message,
                 "gibs policy did not apply %s", requested_name);
        return;
    }
    snprintf(reply->message, sizeof reply->message,
             "gibs mode=%s (was %s)", requested_name, previous_name);
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_GIBS_PREVIOUS_MODE, previous);
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_GIBS_CURRENT_MODE, current);
}

static void ge_mutation_god(const GeConsoleRequest *request,
                            const GeConsoleExecutionContext *context,
                            GeConsoleReply *reply, void *user)
{
    int slot = (int)request->arguments[0].integer;
    int enabled = request->arguments[1].boolean;
    int previous = 0;
    int current = 0;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_GOD;

    if (!ge_mutation_provider.set_player_god(slot, enabled, &previous, &current)) {
        ge_mutation_refused(reply, "god mode unavailable for the selected player");
        return;
    }
    snprintf(reply->message, sizeof reply->message, "god player=%d mode=%s (was %s)",
             slot, current ? "on" : "off", previous ? "on" : "off");
    ge_mutation_payload_boolean(reply, GE_CONSOLE_FIELD_GOD_PREVIOUS, previous);
    ge_mutation_payload_boolean(reply, GE_CONSOLE_FIELD_GOD_CURRENT, current);
}

static void ge_mutation_give(const GeConsoleRequest *request,
                             const GeConsoleExecutionContext *context,
                             GeConsoleReply *reply, void *user)
{
    int slot = (int)request->arguments[0].integer;
    const GeConsoleWeaponName *weapon = ge_mutation_weapon(request->arguments[1].text);
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_GIVE;

    if (weapon == NULL) {
        reply->status = GE_CONSOLE_STATUS_ARGUMENT_CHOICE;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        snprintf(reply->message, sizeof reply->message,
                 "unknown weapon; use a canonical name or item id 1-32");
        return;
    }
    if (!ge_mutation_provider.give_player_weapon(slot, weapon->id)) {
        ge_mutation_refused(reply, "weapon unavailable for the selected player");
        return;
    }
    snprintf(reply->message, sizeof reply->message, "give player=%d weapon=%s id=%d",
             slot, weapon->name, weapon->id);
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_WEAPON_ID, weapon->id);
    ge_mutation_payload_symbol(reply, GE_CONSOLE_FIELD_WEAPON_NAME, weapon->name);
}

static GeConsoleStatus ge_mutation_ammo_value(const char *token, int *amount, int *full)
{
    char *end = NULL;
    long value;

    if (token == NULL || amount == NULL || full == NULL) {
        return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
    }
    if (ge_mutation_token_equal(token, "full")) {
        *amount = 0;
        *full = 1;
        return GE_CONSOLE_STATUS_OK;
    }
    errno = 0;
    value = strtol(token, &end, 10);
    if (token[0] == '\0' || end == NULL || *end != '\0') {
        return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
    }
    if (errno == ERANGE || value < 0 || value > GE_MUTATION_MAX_AMMO) {
        return GE_CONSOLE_STATUS_ARGUMENT_RANGE;
    }
    *amount = (int)value;
    *full = 0;
    return GE_CONSOLE_STATUS_OK;
}

static void ge_mutation_ammo(const GeConsoleRequest *request,
                             const GeConsoleExecutionContext *context,
                             GeConsoleReply *reply, void *user)
{
    int slot = (int)request->arguments[0].integer;
    int amount = 0;
    int full = 0;
    int weapon_id = -1;
    int resolved = 0;
    GeConsoleStatus status;
    const GeConsoleWeaponName *weapon;
    (void)context;
    (void)user;
    reply->message_id = GE_CONSOLE_COMMAND_AMMO;

    status = ge_mutation_ammo_value(request->arguments[1].text, &amount, &full);
    if (status != GE_CONSOLE_STATUS_OK) {
        reply->status = status;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        snprintf(reply->message, sizeof reply->message,
                 "ammo amount must be 0-%d or full", GE_MUTATION_MAX_AMMO);
        return;
    }
    if (!ge_mutation_provider.set_player_ammo(slot, amount, full, &weapon_id, &resolved)) {
        ge_mutation_refused(reply, "equipped weapon has no supported ammunition");
        return;
    }
    weapon = ge_mutation_weapon_by_id(weapon_id);
    snprintf(reply->message, sizeof reply->message,
             "ammo player=%d weapon=%s amount=%d%s", slot,
             weapon != NULL ? weapon->name : "unknown", resolved, full ? " (full)" : "");
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_WEAPON_ID, weapon_id);
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_AMMO_REQUESTED, amount);
    ge_mutation_payload_boolean(reply, GE_CONSOLE_FIELD_AMMO_FULL, full);
    ge_mutation_payload_integer(reply, GE_CONSOLE_FIELD_AMMO_RESOLVED, resolved);
}

static void ge_mutation_spec(GeConsoleCommandSpec *spec, uint32_t command_id,
                             const char *name, const char *summary, uint32_t flags)
{
    memset(spec, 0, sizeof *spec);
    spec->command_id = command_id;
    spec->schema_version = GE_CONSOLE_MUTATION_SCHEMA_VERSION;
    spec->handler_id = (uint16_t)command_id;
    spec->flags = flags;
    snprintf(spec->name, sizeof spec->name, "%s", name);
    snprintf(spec->summary, sizeof spec->summary, "%s", summary);
}

GeConsoleStatus geConsoleMutationInstall(const GeConsoleMutationProvider *provider)
{
    GeConsoleCommandSpec spec;
    GeConsoleStatus status;
    const uint32_t player_mutation = GE_CONSOLE_CMD_MUTATES_GAME |
                                     GE_CONSOLE_CMD_REQUIRES_MISSION |
                                     GE_CONSOLE_CMD_REQUIRES_PLAYER |
                                     GE_CONSOLE_CMD_RECORDABLE |
                                     GE_CONSOLE_CMD_DIAGNOSTIC_SAFE;

    if (provider == NULL || provider->gibs_mode == NULL || provider->set_gibs_mode == NULL ||
        provider->set_player_god == NULL || provider->give_player_weapon == NULL ||
        provider->set_player_ammo == NULL) {
        return GE_CONSOLE_STATUS_INVALID_DEFINITION;
    }
    ge_mutation_provider = *provider;

    /* The typed enum and allowlisted payload are safe diagnostic evidence, and a future
     * reproduction stream must record this gameplay-visible policy change. It is intentionally
     * valid outside missions and in local multiplayer; the core still refuses it in netplay. */
    ge_mutation_spec(&spec, GE_CONSOLE_COMMAND_GIBS, "gibs",
                     "Set the runtime enemy gib policy",
                     GE_CONSOLE_CMD_MUTATES_GAME | GE_CONSOLE_CMD_RECORDABLE |
                     GE_CONSOLE_CMD_DIAGNOSTIC_SAFE);
    spec.argument_count = 1;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "mode");
    spec.arguments[0].type = GE_CONSOLE_ARG_ENUM;
    spec.arguments[0].required = 1;
    spec.arguments[0].enum_count = 4;
    snprintf(spec.arguments[0].enum_values[0], sizeof spec.arguments[0].enum_values[0], "off");
    snprintf(spec.arguments[0].enum_values[1], sizeof spec.arguments[0].enum_values[1],
             "explosions");
    snprintf(spec.arguments[0].enum_values[2], sizeof spec.arguments[0].enum_values[2],
             "high_damage");
    snprintf(spec.arguments[0].enum_values[3], sizeof spec.arguments[0].enum_values[3], "always");
    if ((status = geConsoleRegister(&spec, ge_mutation_gibs, NULL)) != GE_CONSOLE_STATUS_OK) {
        return status;
    }

    ge_mutation_spec(&spec, GE_CONSOLE_COMMAND_GOD, "god",
                     "Set invincibility for an explicit player slot", player_mutation);
    spec.argument_count = 2;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "slot");
    spec.arguments[0].type = GE_CONSOLE_ARG_PLAYER_SLOT;
    spec.arguments[0].required = 1;
    snprintf(spec.arguments[1].name, sizeof spec.arguments[1].name, "mode");
    spec.arguments[1].type = GE_CONSOLE_ARG_BOOLEAN;
    spec.arguments[1].required = 1;
    if ((status = geConsoleRegister(&spec, ge_mutation_god, NULL)) != GE_CONSOLE_STATUS_OK) {
        return status;
    }

    ge_mutation_spec(&spec, GE_CONSOLE_COMMAND_GIVE, "give",
                     "Give and equip a validated weapon name or item id", player_mutation);
    spec.argument_count = 2;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "slot");
    spec.arguments[0].type = GE_CONSOLE_ARG_PLAYER_SLOT;
    spec.arguments[0].required = 1;
    snprintf(spec.arguments[1].name, sizeof spec.arguments[1].name, "weapon");
    spec.arguments[1].type = GE_CONSOLE_ARG_SYMBOL;
    spec.arguments[1].required = 1;
    if ((status = geConsoleRegister(&spec, ge_mutation_give, NULL)) != GE_CONSOLE_STATUS_OK) {
        return status;
    }

    ge_mutation_spec(&spec, GE_CONSOLE_COMMAND_AMMO, "ammo",
                     "Set equipped-weapon ammunition to an amount or full", player_mutation);
    spec.argument_count = 2;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "slot");
    spec.arguments[0].type = GE_CONSOLE_ARG_PLAYER_SLOT;
    spec.arguments[0].required = 1;
    snprintf(spec.arguments[1].name, sizeof spec.arguments[1].name, "amount");
    spec.arguments[1].type = GE_CONSOLE_ARG_SYMBOL;
    spec.arguments[1].required = 1;
    return geConsoleRegister(&spec, ge_mutation_ammo, NULL);
}
