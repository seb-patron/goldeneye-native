/* Initial state-changing developer-console commands. See ge_console_mutations.h. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ge_console_mutations.h"
#include "ge_gibs.h"

static GeConsoleMutationProvider ge_mutation_provider;

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

GeConsoleStatus geConsoleMutationInstall(const GeConsoleMutationProvider *provider)
{
    GeConsoleCommandSpec spec;

    if (provider == NULL || provider->gibs_mode == NULL || provider->set_gibs_mode == NULL) {
        return GE_CONSOLE_STATUS_INVALID_DEFINITION;
    }
    ge_mutation_provider = *provider;

    memset(&spec, 0, sizeof spec);
    spec.command_id = GE_CONSOLE_COMMAND_GIBS;
    spec.schema_version = GE_CONSOLE_MUTATION_SCHEMA_VERSION;
    spec.handler_id = (uint16_t)GE_CONSOLE_COMMAND_GIBS;
    /* The typed enum and allowlisted payload are safe diagnostic evidence, and a future
     * reproduction stream must record this gameplay-visible policy change. It is intentionally
     * valid outside missions and in local multiplayer; the core still refuses it in netplay. */
    spec.flags = GE_CONSOLE_CMD_MUTATES_GAME | GE_CONSOLE_CMD_RECORDABLE |
                 GE_CONSOLE_CMD_DIAGNOSTIC_SAFE;
    snprintf(spec.name, sizeof spec.name, "gibs");
    snprintf(spec.summary, sizeof spec.summary, "Set the runtime enemy gib policy");
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
    return geConsoleRegister(&spec, ge_mutation_gibs, NULL);
}
