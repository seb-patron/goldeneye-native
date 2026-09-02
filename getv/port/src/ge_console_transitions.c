/* Controlled mission-transition commands.  See ge_console_transitions.h. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ge_console_transitions.h"
#include "ge_world_levels.h"

static GeConsoleTransitionProvider ge_transition_provider;

/* These loadable stages have multiplayer setup only.  A one-player direct boot can allocate a
 * player on them, but it produces geometry without a mission setup.  That is not a supported
 * target for a command whose contract is a solo mission transition. */
static const int ge_transition_multiplayer_only_stages[] = { 31, 38, 45, 46, 48, 50 };

static int ge_transition_stage_known(int stage)
{
    int i;
    for (i = 0; i < GE_WORLD_STAGE_COUNT; i++) {
        if (ge_world_stage_names[i].stage == stage) { return 1; }
    }
    return 0;
}

static int ge_transition_stage_multiplayer_only(int stage)
{
    size_t i;
    for (i = 0; i < sizeof ge_transition_multiplayer_only_stages /
                    sizeof ge_transition_multiplayer_only_stages[0]; i++) {
        if (ge_transition_multiplayer_only_stages[i] == stage) { return 1; }
    }
    return 0;
}

static void ge_transition_payload_integer(GeConsoleReply *reply, uint32_t field_id, int value)
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

static void ge_transition_refused(GeConsoleReply *reply, const char *message)
{
    reply->status = GE_CONSOLE_STATUS_REFUSED_CONTEXT;
    reply->severity = GE_CONSOLE_SEVERITY_WARNING;
    snprintf(reply->message, sizeof reply->message, "%s", message);
}

static void ge_transition_request(const GeConsoleExecutionContext *context, int target,
                                  uint32_t command_id, const char *command,
                                  GeConsoleReply *reply)
{
    int previous;

    reply->message_id = command_id;
    reply->target_fields = GE_CONSOLE_TARGET_STAGE;
    reply->stage_id = target;

    if (context == NULL || !(context->flags & GE_CONSOLE_CONTEXT_HAS_STAGE)) {
        ge_transition_refused(reply, "current stage is unavailable");
        return;
    }
    previous = context->stage_id;
    ge_transition_payload_integer(reply, GE_CONSOLE_FIELD_STAGE_PREVIOUS, previous);
    ge_transition_payload_integer(reply, GE_CONSOLE_FIELD_STAGE_REQUESTED, target);

    /* The core's mission flag is intentionally general.  Tighten it here to the catalog of
     * loadable stages before asking the game to mutate transition state. */
    if (!ge_transition_stage_known(previous)) {
        ge_transition_refused(reply, "current stage is not a supported mission");
        return;
    }
    if (!ge_transition_stage_known(target)) {
        reply->status = GE_CONSOLE_STATUS_ARGUMENT_CHOICE;
        reply->severity = GE_CONSOLE_SEVERITY_WARNING;
        snprintf(reply->message, sizeof reply->message,
                 "stage %d is unavailable", target);
        return;
    }
    if (ge_transition_stage_multiplayer_only(previous) ||
        ge_transition_stage_multiplayer_only(target)) {
        ge_transition_refused(reply,
                              "multiplayer-only stages are unsupported for solo transitions");
        return;
    }
    if (!ge_transition_provider.request_stage(target)) {
        reply->status = GE_CONSOLE_STATUS_HANDLER_ERROR;
        reply->severity = GE_CONSOLE_SEVERITY_ERROR;
        snprintf(reply->message, sizeof reply->message,
                 "%s could not schedule stage %d", command, target);
        return;
    }

    if (command_id == GE_CONSOLE_COMMAND_RESTART) {
        snprintf(reply->message, sizeof reply->message,
                 "restart stage=%d scheduled", target);
    } else {
        snprintf(reply->message, sizeof reply->message,
                 "level stage=%d scheduled (was %d)", target, previous);
    }
}

static void ge_transition_restart(const GeConsoleRequest *request,
                                  const GeConsoleExecutionContext *context,
                                  GeConsoleReply *reply, void *user)
{
    (void)request;
    (void)user;
    ge_transition_request(context, context != NULL ? context->stage_id : -1,
                          GE_CONSOLE_COMMAND_RESTART, "restart", reply);
}

static void ge_transition_level(const GeConsoleRequest *request,
                                const GeConsoleExecutionContext *context,
                                GeConsoleReply *reply, void *user)
{
    (void)user;
    ge_transition_request(context, (int)request->arguments[0].integer,
                          GE_CONSOLE_COMMAND_LEVEL, "level", reply);
}

static void ge_transition_spec(GeConsoleCommandSpec *spec, uint32_t command_id,
                               const char *name, const char *summary)
{
    memset(spec, 0, sizeof *spec);
    spec->command_id = command_id;
    spec->schema_version = GE_CONSOLE_TRANSITION_SCHEMA_VERSION;
    spec->handler_id = (uint16_t)command_id;
    spec->flags = GE_CONSOLE_CMD_MUTATES_GAME |
                  GE_CONSOLE_CMD_REQUIRES_MISSION |
                  GE_CONSOLE_CMD_SOLO_ONLY |
                  GE_CONSOLE_CMD_RECORDABLE |
                  GE_CONSOLE_CMD_DIAGNOSTIC_SAFE;
    snprintf(spec->name, sizeof spec->name, "%s", name);
    snprintf(spec->summary, sizeof spec->summary, "%s", summary);
}

GeConsoleStatus geConsoleTransitionInstall(const GeConsoleTransitionProvider *provider)
{
    GeConsoleCommandSpec spec;
    GeConsoleStatus status;

    if (provider == NULL || provider->request_stage == NULL) {
        return GE_CONSOLE_STATUS_INVALID_DEFINITION;
    }
    ge_transition_provider = *provider;

    ge_transition_spec(&spec, GE_CONSOLE_COMMAND_RESTART, "restart",
                       "Restart the current solo mission through the controlled transition path");
    if ((status = geConsoleRegister(&spec, ge_transition_restart, NULL)) !=
        GE_CONSOLE_STATUS_OK) {
        return status;
    }

    ge_transition_spec(&spec, GE_CONSOLE_COMMAND_LEVEL, "level",
                       "Transition to a loadable solo mission stage id");
    spec.argument_count = 1;
    snprintf(spec.arguments[0].name, sizeof spec.arguments[0].name, "stage");
    spec.arguments[0].type = GE_CONSOLE_ARG_INTEGER;
    spec.arguments[0].required = 1;
    spec.arguments[0].minimum = 0;
    spec.arguments[0].maximum = 99;
    return geConsoleRegister(&spec, ge_transition_level, NULL);
}
