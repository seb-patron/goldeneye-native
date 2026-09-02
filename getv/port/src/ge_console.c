/* See ge_console.h for the ownership, safety and execution contracts. */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ge_console.h"
#include "ge_console_pause.h"

typedef struct GeConsoleRegisteredCommand {
    GeConsoleCommandSpec spec;
    GeConsoleHandler handler;
    void *user;
} GeConsoleRegisteredCommand;

typedef struct GeConsoleTokens {
    unsigned int count;
    char value[GE_CONSOLE_MAX_TOKENS][GE_CONSOLE_MAX_VALUE_TEXT];
} GeConsoleTokens;

static struct {
    GeConsoleRegisteredCommand commands[GE_CONSOLE_MAX_COMMANDS];
    unsigned int command_count;

    GeConsoleRequest queue[GE_CONSOLE_QUEUE_CAPACITY];
    unsigned int queue_head;
    unsigned int queue_count;

    GeConsoleResult results[GE_CONSOLE_RESULT_CAPACITY];
    unsigned int result_head;
    unsigned int result_count;
    uint64_t results_dropped;

    uint64_t last_sequence;
    uint64_t last_result_sequence;
    int pumping;
    GeConsoleContextProvider context_provider;
    void *context_user;
} ge_console;

static size_t ge_console_bounded_length(const char *s, size_t max)
{
    size_t n;
    if (s == NULL) { return max; }
    for (n = 0; n < max && s[n] != '\0'; n++) {}
    return n;
}

static int ge_console_copy(char *dst, size_t dst_size, const char *src)
{
    size_t n = ge_console_bounded_length(src, dst_size);
    if (dst == NULL || dst_size == 0 || n >= dst_size) { return 0; }
    memcpy(dst, src, n + 1);
    return 1;
}

static int ge_console_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static unsigned char ge_console_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static int ge_console_equal_ci(const char *a, const char *b)
{
    size_t i;
    if (a == NULL || b == NULL) { return 0; }
    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (ge_console_lower((unsigned char)a[i]) != ge_console_lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int ge_console_prefix_ci(const char *text, const char *prefix)
{
    size_t i;
    if (text == NULL || prefix == NULL) { return 0; }
    for (i = 0; prefix[i] != '\0'; i++) {
        if (text[i] == '\0') { return 0; }
        if (ge_console_lower((unsigned char)text[i]) !=
            ge_console_lower((unsigned char)prefix[i])) { return 0; }
    }
    return 1;
}

static int ge_console_symbol(const char *s, size_t cap)
{
    size_t i, n = ge_console_bounded_length(s, cap);
    if (n == 0 || n >= cap) { return 0; }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) { return 0; }
    }
    return 1;
}

static int ge_console_command_path(const char *s, size_t cap)
{
    size_t i, n = ge_console_bounded_length(s, cap);
    int at_word_start = 1;
    if (n == 0 || n >= cap || s[0] == ' ' || s[n - 1] == ' ') { return 0; }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') {
            if (at_word_start) { return 0; }
            at_word_start = 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
        if (at_word_start && !(c >= 'a' && c <= 'z')) { return 0; }
        at_word_start = 0;
    }
    return !at_word_start;
}

static int ge_console_text_value(const char *s, size_t cap)
{
    size_t i, n = ge_console_bounded_length(s, cap);
    if (n == 0 || n >= cap) { return 0; }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) { return 0; }
    }
    return 1;
}

static int ge_console_paths_collide(const char *path)
{
    unsigned int i, j;
    for (i = 0; i < ge_console.command_count; i++) {
        const GeConsoleCommandSpec *spec = &ge_console.commands[i].spec;
        if (ge_console_equal_ci(path, spec->name)) { return 1; }
        for (j = 0; j < spec->alias_count; j++) {
            if (ge_console_equal_ci(path, spec->aliases[j])) { return 1; }
        }
    }
    return 0;
}

static int ge_console_valid_definition(const GeConsoleCommandSpec *spec,
                                       GeConsoleHandler handler)
{
    unsigned int i, j;
    int optional_seen = 0;
    int has_player_arg = 0;
    unsigned int mode_flags;
    const uint32_t known_flags = GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_MUTATES_GAME |
        GE_CONSOLE_CMD_REQUIRES_MISSION | GE_CONSOLE_CMD_REQUIRES_PLAYER |
        GE_CONSOLE_CMD_SOLO_ONLY | GE_CONSOLE_CMD_DETERMINISTIC_ONLY |
        GE_CONSOLE_CMD_RECORDABLE | GE_CONSOLE_CMD_DIAGNOSTIC_SAFE;

    if (spec == NULL || handler == NULL || spec->command_id == 0 ||
        spec->schema_version == 0 || spec->handler_id == 0 ||
        !ge_console_command_path(spec->name, sizeof spec->name) ||
        !ge_console_text_value(spec->summary, sizeof spec->summary) ||
        spec->alias_count > GE_CONSOLE_MAX_ALIASES ||
        spec->argument_count > GE_CONSOLE_MAX_ARGS ||
        (spec->flags & ~known_flags) != 0) { return 0; }

    mode_flags = spec->flags & (GE_CONSOLE_CMD_READ_ONLY | GE_CONSOLE_CMD_MUTATES_GAME);
    if (mode_flags != GE_CONSOLE_CMD_READ_ONLY && mode_flags != GE_CONSOLE_CMD_MUTATES_GAME) {
        return 0;
    }

    for (i = 0; i < spec->alias_count; i++) {
        if (!ge_console_command_path(spec->aliases[i], sizeof spec->aliases[i])) { return 0; }
        if (ge_console_equal_ci(spec->aliases[i], spec->name)) { return 0; }
        for (j = 0; j < i; j++) {
            if (ge_console_equal_ci(spec->aliases[i], spec->aliases[j])) { return 0; }
        }
    }

    for (i = 0; i < spec->argument_count; i++) {
        const GeConsoleArgSchema *arg = &spec->arguments[i];
        if (!ge_console_symbol(arg->name, sizeof arg->name)) { return 0; }
        for (j = 0; j < i; j++) {
            if (ge_console_equal_ci(arg->name, spec->arguments[j].name)) { return 0; }
        }
        if (!arg->required) { optional_seen = 1; }
        else if (optional_seen) { return 0; }

        switch (arg->type) {
        case GE_CONSOLE_ARG_INTEGER:
            if (arg->minimum > arg->maximum || arg->enum_count != 0) { return 0; }
            break;
        case GE_CONSOLE_ARG_PLAYER_SLOT:
            if (arg->enum_count != 0) { return 0; }
            has_player_arg = 1;
            break;
        case GE_CONSOLE_ARG_BOOLEAN:
        case GE_CONSOLE_ARG_SYMBOL:
        case GE_CONSOLE_ARG_TEXT:
            if (arg->enum_count != 0) { return 0; }
            if (arg->type == GE_CONSOLE_ARG_TEXT &&
                (spec->flags & GE_CONSOLE_CMD_DIAGNOSTIC_SAFE)) { return 0; }
            break;
        case GE_CONSOLE_ARG_ENUM:
            if (arg->enum_count == 0 || arg->enum_count > GE_CONSOLE_MAX_ENUM_VALUES) { return 0; }
            for (j = 0; j < arg->enum_count; j++) {
                unsigned int k;
                if (!ge_console_text_value(arg->enum_values[j], sizeof arg->enum_values[j])) {
                    return 0;
                }
                for (k = 0; k < j; k++) {
                    if (ge_console_equal_ci(arg->enum_values[j], arg->enum_values[k])) { return 0; }
                }
            }
            break;
        default:
            return 0;
        }
    }

    if ((spec->flags & GE_CONSOLE_CMD_REQUIRES_PLAYER) && !has_player_arg) { return 0; }
    return 1;
}

void geConsoleReset(void)
{
    memset(&ge_console, 0, sizeof ge_console);
}

GeConsoleStatus geConsoleRegister(const GeConsoleCommandSpec *spec,
                                  GeConsoleHandler handler,
                                  void *user)
{
    unsigned int i;
    GeConsoleRegisteredCommand *dst;
    if (!ge_console_valid_definition(spec, handler)) {
        return GE_CONSOLE_STATUS_INVALID_DEFINITION;
    }
    if (ge_console.command_count >= GE_CONSOLE_MAX_COMMANDS) {
        return GE_CONSOLE_STATUS_REGISTRY_FULL;
    }
    for (i = 0; i < ge_console.command_count; i++) {
        if (ge_console.commands[i].spec.command_id == spec->command_id) {
            return GE_CONSOLE_STATUS_DUPLICATE_COMMAND;
        }
    }
    if (ge_console_paths_collide(spec->name)) { return GE_CONSOLE_STATUS_DUPLICATE_COMMAND; }
    for (i = 0; i < spec->alias_count; i++) {
        if (ge_console_paths_collide(spec->aliases[i])) {
            return GE_CONSOLE_STATUS_DUPLICATE_COMMAND;
        }
    }

    dst = &ge_console.commands[ge_console.command_count++];
    memset(dst, 0, sizeof *dst);
    dst->spec = *spec;
    dst->handler = handler;
    dst->user = user;
    return GE_CONSOLE_STATUS_OK;
}

unsigned int geConsoleCommandCount(void)
{
    return ge_console.command_count;
}

int geConsoleCommandAt(unsigned int index, GeConsoleCommandSpec *out)
{
    if (index >= ge_console.command_count || out == NULL) { return 0; }
    *out = ge_console.commands[index].spec;
    return 1;
}

int geConsoleCommandById(uint32_t command_id, GeConsoleCommandSpec *out)
{
    unsigned int i;
    if (out == NULL) { return 0; }
    for (i = 0; i < ge_console.command_count; i++) {
        if (ge_console.commands[i].spec.command_id == command_id) {
            *out = ge_console.commands[i].spec;
            return 1;
        }
    }
    return 0;
}

unsigned int geConsoleComplete(const char *prefix, GeConsoleCompletion *out,
                               unsigned int max, int *truncated)
{
    unsigned int i, j, total = 0;
    size_t n;
    if (truncated != NULL) { *truncated = 0; }
    if (prefix == NULL) { prefix = ""; }
    n = ge_console_bounded_length(prefix, GE_CONSOLE_MAX_COMMAND_PATH);
    if (n >= GE_CONSOLE_MAX_COMMAND_PATH) { return 0; }

    for (i = 0; i < ge_console.command_count; i++) {
        const GeConsoleCommandSpec *spec = &ge_console.commands[i].spec;
        int match = ge_console_prefix_ci(spec->name, prefix);
        for (j = 0; !match && j < spec->alias_count; j++) {
            match = ge_console_prefix_ci(spec->aliases[j], prefix);
        }
        if (!match) { continue; }
        if (out != NULL && total < max) {
            GeConsoleCompletion *row = &out[total];
            memset(row, 0, sizeof *row);
            row->command_id = spec->command_id;
            row->schema_version = spec->schema_version;
            row->flags = spec->flags;
            ge_console_copy(row->name, sizeof row->name, spec->name);
            ge_console_copy(row->summary, sizeof row->summary, spec->summary);
        }
        total++;
    }
    if (truncated != NULL && total > max) { *truncated = 1; }
    return total;
}

static GeConsoleStatus ge_console_tokenize(const char *line, GeConsoleTokens *tokens)
{
    size_t line_len, pos = 0;
    if (tokens == NULL) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
    memset(tokens, 0, sizeof *tokens);
    if (line == NULL) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
    line_len = ge_console_bounded_length(line, GE_CONSOLE_MAX_LINE + 1u);
    if (line_len > GE_CONSOLE_MAX_LINE) { return GE_CONSOLE_STATUS_LINE_TOO_LONG; }

    while (pos < line_len) {
        size_t out_len = 0;
        int quoted = 0;
        int started = 0;
        while (pos < line_len && ge_console_space((unsigned char)line[pos])) { pos++; }
        if (pos >= line_len) { break; }
        if (tokens->count >= GE_CONSOLE_MAX_TOKENS) {
            return GE_CONSOLE_STATUS_TOO_MANY_TOKENS;
        }

        while (pos < line_len) {
            unsigned char c = (unsigned char)line[pos];
            if (!quoted && ge_console_space(c)) { break; }
            started = 1;
            if (c == '"') {
                quoted = !quoted;
                pos++;
                continue;
            }
            if (c == '\\') {
                pos++;
                if (pos >= line_len) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
                c = (unsigned char)line[pos];
                if (!(c == '\\' || c == '"' || c == ' ' || c == '\t')) {
                    return GE_CONSOLE_STATUS_INVALID_SYNTAX;
                }
            } else if (c < 0x20 || c == 0x7f) {
                return GE_CONSOLE_STATUS_INVALID_SYNTAX;
            }
            if (out_len + 1 >= GE_CONSOLE_MAX_VALUE_TEXT) {
                return GE_CONSOLE_STATUS_TOKEN_TOO_LONG;
            }
            tokens->value[tokens->count][out_len++] = (char)c;
            pos++;
        }
        if (quoted) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
        if (!started) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
        tokens->value[tokens->count][out_len] = '\0';
        tokens->count++;
    }
    return tokens->count == 0 ? GE_CONSOLE_STATUS_EMPTY_INPUT : GE_CONSOLE_STATUS_OK;
}

static int ge_console_path_match(const char *path, const GeConsoleTokens *tokens)
{
    unsigned int word = 0;
    size_t p = 0;
    while (path[p] != '\0') {
        size_t start = p, n, i;
        while (path[p] != '\0' && path[p] != ' ') { p++; }
        n = p - start;
        if (word >= tokens->count) { return 0; }
        if (ge_console_bounded_length(tokens->value[word], GE_CONSOLE_MAX_VALUE_TEXT) != n) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            if (ge_console_lower((unsigned char)path[start + i]) !=
                ge_console_lower((unsigned char)tokens->value[word][i])) { return 0; }
        }
        word++;
        if (path[p] == ' ') { p++; }
    }
    return (int)word;
}

static GeConsoleStatus ge_console_parse_integer(const char *token,
                                                const GeConsoleArgSchema *schema,
                                                GeConsoleValue *value)
{
    char *end = NULL;
    long long parsed;
    errno = 0;
    parsed = strtoll(token, &end, 10);
    if (token[0] == '\0' || end == NULL || *end != '\0') {
        return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
    }
    if (errno == ERANGE || parsed < schema->minimum || parsed > schema->maximum) {
        return GE_CONSOLE_STATUS_ARGUMENT_RANGE;
    }
    value->integer = (int64_t)parsed;
    return GE_CONSOLE_STATUS_OK;
}

static GeConsoleStatus ge_console_parse_value(const char *token,
                                              const GeConsoleArgSchema *schema,
                                              GeConsoleValue *value)
{
    unsigned int i;
    memset(value, 0, sizeof *value);
    value->type = schema->type;
    value->present = 1;
    value->choice_index = -1;

    switch (schema->type) {
    case GE_CONSOLE_ARG_INTEGER:
        return ge_console_parse_integer(token, schema, value);
    case GE_CONSOLE_ARG_PLAYER_SLOT: {
        GeConsoleArgSchema slot_schema = *schema;
        slot_schema.minimum = 0;
        slot_schema.maximum = 3;
        return ge_console_parse_integer(token, &slot_schema, value);
    }
    case GE_CONSOLE_ARG_BOOLEAN:
        if (ge_console_equal_ci(token, "on") || ge_console_equal_ci(token, "true") ||
            ge_console_equal_ci(token, "yes") || ge_console_equal_ci(token, "1")) {
            value->boolean = 1;
            ge_console_copy(value->text, sizeof value->text, "on");
            return GE_CONSOLE_STATUS_OK;
        }
        if (ge_console_equal_ci(token, "off") || ge_console_equal_ci(token, "false") ||
            ge_console_equal_ci(token, "no") || ge_console_equal_ci(token, "0")) {
            value->boolean = 0;
            ge_console_copy(value->text, sizeof value->text, "off");
            return GE_CONSOLE_STATUS_OK;
        }
        return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
    case GE_CONSOLE_ARG_ENUM:
        for (i = 0; i < schema->enum_count; i++) {
            if (ge_console_equal_ci(token, schema->enum_values[i])) {
                value->choice_index = (int)i;
                ge_console_copy(value->text, sizeof value->text, schema->enum_values[i]);
                return GE_CONSOLE_STATUS_OK;
            }
        }
        return GE_CONSOLE_STATUS_ARGUMENT_CHOICE;
    case GE_CONSOLE_ARG_SYMBOL:
        if (!ge_console_symbol(token, GE_CONSOLE_MAX_VALUE_TEXT)) {
            return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
        }
        ge_console_copy(value->text, sizeof value->text, token);
        return GE_CONSOLE_STATUS_OK;
    case GE_CONSOLE_ARG_TEXT:
        if (ge_console_bounded_length(token, GE_CONSOLE_MAX_VALUE_TEXT) >=
            GE_CONSOLE_MAX_VALUE_TEXT) { return GE_CONSOLE_STATUS_TOKEN_TOO_LONG; }
        ge_console_copy(value->text, sizeof value->text, token);
        return GE_CONSOLE_STATUS_OK;
    default:
        return GE_CONSOLE_STATUS_ARGUMENT_TYPE;
    }
}

GeConsoleStatus geConsoleParse(const char *line, GeConsoleRequest *out)
{
    GeConsoleTokens tokens;
    GeConsoleStatus status;
    int best_words = 0;
    unsigned int best = UINT_MAX;
    unsigned int i, j, required = 0, supplied;
    const GeConsoleCommandSpec *spec;

    if (out == NULL) { return GE_CONSOLE_STATUS_INVALID_SYNTAX; }
    memset(out, 0, sizeof *out);
    status = ge_console_tokenize(line, &tokens);
    if (status != GE_CONSOLE_STATUS_OK) { return status; }

    for (i = 0; i < ge_console.command_count; i++) {
        int words = ge_console_path_match(ge_console.commands[i].spec.name, &tokens);
        for (j = 0; j < ge_console.commands[i].spec.alias_count; j++) {
            int alias_words = ge_console_path_match(ge_console.commands[i].spec.aliases[j], &tokens);
            if (alias_words > words) { words = alias_words; }
        }
        if (words > best_words) {
            best_words = words;
            best = i;
        }
    }
    if (best == UINT_MAX) { return GE_CONSOLE_STATUS_UNKNOWN_COMMAND; }

    spec = &ge_console.commands[best].spec;
    out->command_id = spec->command_id;
    out->command_version = spec->schema_version;
    out->handler_id = spec->handler_id;
    out->command_flags = spec->flags;
    out->registry_index = best;
    out->argument_count = spec->argument_count;

    for (i = 0; i < spec->argument_count; i++) {
        if (spec->arguments[i].required) { required++; }
    }
    supplied = tokens.count - (unsigned int)best_words;
    if (supplied < required || supplied > spec->argument_count) {
        return GE_CONSOLE_STATUS_ARGUMENT_COUNT;
    }

    for (i = 0; i < spec->argument_count; i++) {
        if (i >= supplied) {
            out->arguments[i].type = spec->arguments[i].type;
            out->arguments[i].choice_index = -1;
            continue;
        }
        status = ge_console_parse_value(tokens.value[best_words + (int)i],
                                        &spec->arguments[i], &out->arguments[i]);
        if (status != GE_CONSOLE_STATUS_OK) { return status; }
    }
    return GE_CONSOLE_STATUS_OK;
}

const char *geConsoleStatusName(GeConsoleStatus status)
{
    switch (status) {
    case GE_CONSOLE_STATUS_OK:                  return "ok";
    case GE_CONSOLE_STATUS_EMPTY_INPUT:         return "empty_input";
    case GE_CONSOLE_STATUS_UNKNOWN_COMMAND:     return "unknown_command";
    case GE_CONSOLE_STATUS_INVALID_SYNTAX:      return "invalid_syntax";
    case GE_CONSOLE_STATUS_LINE_TOO_LONG:       return "line_too_long";
    case GE_CONSOLE_STATUS_TOO_MANY_TOKENS:     return "too_many_tokens";
    case GE_CONSOLE_STATUS_TOKEN_TOO_LONG:      return "token_too_long";
    case GE_CONSOLE_STATUS_ARGUMENT_COUNT:      return "argument_count";
    case GE_CONSOLE_STATUS_ARGUMENT_TYPE:       return "argument_type";
    case GE_CONSOLE_STATUS_ARGUMENT_RANGE:      return "argument_range";
    case GE_CONSOLE_STATUS_ARGUMENT_CHOICE:     return "argument_choice";
    case GE_CONSOLE_STATUS_QUEUE_FULL:          return "queue_full";
    case GE_CONSOLE_STATUS_REGISTRY_FULL:       return "registry_full";
    case GE_CONSOLE_STATUS_DUPLICATE_COMMAND:   return "duplicate_command";
    case GE_CONSOLE_STATUS_INVALID_DEFINITION:  return "invalid_definition";
    case GE_CONSOLE_STATUS_REFUSED_MISSION:     return "refused_mission";
    case GE_CONSOLE_STATUS_REFUSED_PLAYER:      return "refused_player";
    case GE_CONSOLE_STATUS_REFUSED_SOLO:        return "refused_solo";
    case GE_CONSOLE_STATUS_REFUSED_NETPLAY:     return "refused_netplay";
    case GE_CONSOLE_STATUS_REFUSED_DETERMINISM: return "refused_determinism";
    case GE_CONSOLE_STATUS_HANDLER_ERROR:       return "handler_error";
    case GE_CONSOLE_STATUS_RESULT_OVERFLOW:     return "result_overflow";
    default:                                    return "unknown_status";
    }
}

static GeConsoleSeverity ge_console_severity(GeConsoleStatus status)
{
    if (status == GE_CONSOLE_STATUS_OK) { return GE_CONSOLE_SEVERITY_INFO; }
    if (status == GE_CONSOLE_STATUS_QUEUE_FULL || status == GE_CONSOLE_STATUS_RESULT_OVERFLOW ||
        (status >= GE_CONSOLE_STATUS_REFUSED_MISSION &&
         status <= GE_CONSOLE_STATUS_REFUSED_DETERMINISM)) {
        return GE_CONSOLE_SEVERITY_WARNING;
    }
    return GE_CONSOLE_SEVERITY_ERROR;
}

static void ge_console_append_result(GeConsoleResult *result)
{
    unsigned int index;
    if (ge_console.result_count == GE_CONSOLE_RESULT_CAPACITY) {
        ge_console.result_head = (ge_console.result_head + 1u) % GE_CONSOLE_RESULT_CAPACITY;
        ge_console.result_count--;
        ge_console.results_dropped++;
    }
    result->sequence = ++ge_console.last_result_sequence;
    result->history_dropped_before = ge_console.results_dropped;
    index = (ge_console.result_head + ge_console.result_count) % GE_CONSOLE_RESULT_CAPACITY;
    ge_console.results[index] = *result;
    ge_console.result_count++;
}

static void ge_console_immediate_result(const GeConsoleRequest *request, uint64_t sequence,
                                        uint64_t tick, uint64_t frame, GeConsoleStatus status)
{
    GeConsoleResult result;
    memset(&result, 0, sizeof result);
    result.request_sequence = sequence;
    result.status = status;
    result.severity = ge_console_severity(status);
    result.submission_tick = tick;
    result.submission_frame = frame;
    result.message_id = (uint32_t)status;
    ge_console_copy(result.message, sizeof result.message, geConsoleStatusName(status));
    result.player_slot = -1;
    result.stage_id = -1;
    if (request != NULL) {
        result.command_id = request->command_id;
        result.command_version = request->command_version;
        result.handler_id = request->handler_id;
    }
    ge_console_append_result(&result);
}

GeConsoleStatus geConsoleSubmit(const char *line, uint64_t submission_tick,
                                uint64_t submission_frame, uint64_t *sequence_out)
{
    GeConsoleRequest request;
    GeConsoleStatus status;
    unsigned int tail;
    uint64_t sequence = ++ge_console.last_sequence;
    if (sequence_out != NULL) { *sequence_out = sequence; }

    status = geConsoleParse(line, &request);
    if (status != GE_CONSOLE_STATUS_OK) {
        ge_console_immediate_result(&request, sequence, submission_tick, submission_frame, status);
        return status;
    }
    request.sequence = sequence;
    request.submission_tick = submission_tick;
    request.submission_frame = submission_frame;
    if (ge_console.queue_count >= GE_CONSOLE_QUEUE_CAPACITY) {
        ge_console_immediate_result(&request, sequence, submission_tick, submission_frame,
                                    GE_CONSOLE_STATUS_QUEUE_FULL);
        return GE_CONSOLE_STATUS_QUEUE_FULL;
    }
    tail = (ge_console.queue_head + ge_console.queue_count) % GE_CONSOLE_QUEUE_CAPACITY;
    ge_console.queue[tail] = request;
    ge_console.queue_count++;
    return GE_CONSOLE_STATUS_OK;
}

unsigned int geConsoleQueueCount(void)
{
    return ge_console.queue_count;
}

static int ge_console_result_status_valid(GeConsoleStatus status)
{
    switch (status) {
    case GE_CONSOLE_STATUS_OK:
    case GE_CONSOLE_STATUS_ARGUMENT_COUNT:
    case GE_CONSOLE_STATUS_ARGUMENT_TYPE:
    case GE_CONSOLE_STATUS_ARGUMENT_RANGE:
    case GE_CONSOLE_STATUS_ARGUMENT_CHOICE:
    case GE_CONSOLE_STATUS_REFUSED_MISSION:
    case GE_CONSOLE_STATUS_REFUSED_PLAYER:
    case GE_CONSOLE_STATUS_REFUSED_SOLO:
    case GE_CONSOLE_STATUS_REFUSED_NETPLAY:
    case GE_CONSOLE_STATUS_REFUSED_DETERMINISM:
    case GE_CONSOLE_STATUS_HANDLER_ERROR:
        return 1;
    default:
        return 0;
    }
}

static int ge_console_reply_valid(const GeConsoleReply *reply)
{
    unsigned int i, j;
    if (!ge_console_result_status_valid(reply->status) ||
        reply->severity < GE_CONSOLE_SEVERITY_INFO ||
        reply->severity > GE_CONSOLE_SEVERITY_ERROR ||
        reply->payload_count > GE_CONSOLE_MAX_PAYLOAD_VALUES ||
        (reply->target_fields & ~(GE_CONSOLE_TARGET_PLAYER | GE_CONSOLE_TARGET_STAGE)) != 0 ||
        ((reply->target_fields & GE_CONSOLE_TARGET_PLAYER) &&
         (reply->player_slot < 0 || reply->player_slot > 3)) ||
        ge_console_bounded_length(reply->message, sizeof reply->message) >= sizeof reply->message) {
        return 0;
    }
    for (i = 0; i < reply->payload_count; i++) {
        const GeConsolePayloadValue *payload = &reply->payload[i];
        const GeConsoleValue *value = &payload->value;
        if (payload->field_id == 0 || !value->present) { return 0; }
        for (j = 0; j < i; j++) {
            if (reply->payload[j].field_id == payload->field_id) { return 0; }
        }
        switch (value->type) {
        case GE_CONSOLE_ARG_INTEGER:
        case GE_CONSOLE_ARG_PLAYER_SLOT:
            break;
        case GE_CONSOLE_ARG_BOOLEAN:
            if (value->boolean != 0 && value->boolean != 1) { return 0; }
            break;
        case GE_CONSOLE_ARG_ENUM:
        case GE_CONSOLE_ARG_SYMBOL:
        case GE_CONSOLE_ARG_TEXT:
            if (ge_console_bounded_length(value->text, sizeof value->text) >=
                sizeof value->text) { return 0; }
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int ge_console_player_target(const GeConsoleCommandSpec *spec,
                                    const GeConsoleRequest *request)
{
    unsigned int i;
    for (i = 0; i < spec->argument_count; i++) {
        if (spec->arguments[i].type == GE_CONSOLE_ARG_PLAYER_SLOT &&
            request->arguments[i].present) {
            return (int)request->arguments[i].integer;
        }
    }
    return -1;
}

static GeConsoleStatus ge_console_context_status(const GeConsoleCommandSpec *spec,
                                                 const GeConsoleRequest *request,
                                                 const GeConsoleExecutionContext *context,
                                                 int *player_slot)
{
    if ((spec->flags & GE_CONSOLE_CMD_MUTATES_GAME) &&
        (context->flags & GE_CONSOLE_CONTEXT_NETPLAY)) {
        return GE_CONSOLE_STATUS_REFUSED_NETPLAY;
    }
    if ((spec->flags & GE_CONSOLE_CMD_REQUIRES_MISSION) &&
        !(context->flags & GE_CONSOLE_CONTEXT_MISSION_ACTIVE)) {
        return GE_CONSOLE_STATUS_REFUSED_MISSION;
    }
    if ((spec->flags & GE_CONSOLE_CMD_SOLO_ONLY) &&
        !(context->flags & GE_CONSOLE_CONTEXT_SOLO)) {
        return GE_CONSOLE_STATUS_REFUSED_SOLO;
    }
    if ((spec->flags & GE_CONSOLE_CMD_DETERMINISTIC_ONLY) &&
        !(context->flags & GE_CONSOLE_CONTEXT_DETERMINISTIC)) {
        return GE_CONSOLE_STATUS_REFUSED_DETERMINISM;
    }
    if (spec->flags & GE_CONSOLE_CMD_REQUIRES_PLAYER) {
        *player_slot = ge_console_player_target(spec, request);
        if (*player_slot < 0 || !(context->player_mask & (1u << (unsigned int)*player_slot))) {
            return GE_CONSOLE_STATUS_REFUSED_PLAYER;
        }
    }
    return GE_CONSOLE_STATUS_OK;
}

unsigned int geConsolePump(const GeConsoleExecutionContext *context)
{
    GeConsoleExecutionContext empty_context;
    unsigned int pending, executed = 0;
    if (ge_console.pumping) { return 0; }
    if (context == NULL) {
        memset(&empty_context, 0, sizeof empty_context);
        context = &empty_context;
    }

    /* Snapshot the count.  Follow-up submissions stay queued for the next tick, so one handler
     * cannot create an unbounded same-tick loop or make execution count depend on handler order. */
    pending = ge_console.queue_count;
    ge_console.pumping = 1;
    while (executed < pending && ge_console.queue_count > 0) {
        GeConsoleRequest request = ge_console.queue[ge_console.queue_head];
        GeConsoleResult result;
        GeConsoleReply reply;
        GeConsoleRegisteredCommand *registered = NULL;
        GeConsoleStatus status = GE_CONSOLE_STATUS_HANDLER_ERROR;
        int player_slot = -1;

        /* Pop before any validation or handler call.  Even a re-entrant pump therefore cannot
         * observe this request a second time. */
        ge_console.queue_head = (ge_console.queue_head + 1u) % GE_CONSOLE_QUEUE_CAPACITY;
        ge_console.queue_count--;
        executed++;

        memset(&result, 0, sizeof result);
        result.request_sequence = request.sequence;
        result.command_id = request.command_id;
        result.command_version = request.command_version;
        result.handler_id = request.handler_id;
        result.submission_tick = request.submission_tick;
        result.submission_frame = request.submission_frame;
        result.execution_tick = context->game_tick;
        result.execution_frame = context->render_frame;
        result.player_slot = -1;
        result.stage_id = -1;
        if (context->flags & GE_CONSOLE_CONTEXT_HAS_STAGE) {
            result.target_fields |= GE_CONSOLE_TARGET_STAGE;
            result.stage_id = context->stage_id;
        }

        if (request.registry_index < ge_console.command_count) {
            registered = &ge_console.commands[request.registry_index];
            if (registered->spec.command_id != request.command_id ||
                registered->spec.schema_version != request.command_version ||
                registered->spec.handler_id != request.handler_id) {
                registered = NULL;
            }
        }
        if (registered != NULL) {
            status = ge_console_context_status(&registered->spec, &request, context, &player_slot);
        }
        if (player_slot >= 0) {
            result.target_fields |= GE_CONSOLE_TARGET_PLAYER;
            result.player_slot = player_slot;
        }

        if (registered != NULL && status == GE_CONSOLE_STATUS_OK) {
            memset(&reply, 0, sizeof reply);
            reply.status = GE_CONSOLE_STATUS_OK;
            reply.severity = GE_CONSOLE_SEVERITY_INFO;
            reply.player_slot = -1;
            reply.stage_id = -1;
            registered->handler(&request, context, &reply, registered->user);
            if (!ge_console_reply_valid(&reply)) {
                memset(&reply, 0, sizeof reply);
                reply.status = GE_CONSOLE_STATUS_HANDLER_ERROR;
                reply.severity = GE_CONSOLE_SEVERITY_ERROR;
            }
            status = reply.status;
            result.severity = reply.severity;
            result.message_id = reply.message_id;
            ge_console_copy(result.message, sizeof result.message, reply.message);
            result.payload_count = reply.payload_count;
            memcpy(result.payload, reply.payload,
                   sizeof(result.payload[0]) * (size_t)reply.payload_count);
            if (reply.target_fields & GE_CONSOLE_TARGET_PLAYER) {
                result.target_fields |= GE_CONSOLE_TARGET_PLAYER;
                result.player_slot = reply.player_slot;
            }
            if (reply.target_fields & GE_CONSOLE_TARGET_STAGE) {
                result.target_fields |= GE_CONSOLE_TARGET_STAGE;
                result.stage_id = reply.stage_id;
            }
        } else {
            result.severity = ge_console_severity(status);
        }

        result.status = status;
        if (result.message[0] == '\0') {
            result.message_id = (uint32_t)status;
            ge_console_copy(result.message, sizeof result.message, geConsoleStatusName(status));
        }
        ge_console_append_result(&result);
    }
    ge_console.pumping = 0;
    return executed;
}

void geConsoleSetContextProvider(GeConsoleContextProvider provider, void *user)
{
    ge_console.context_provider = provider;
    ge_console.context_user = user;
}

void gePortConsoleGameTick(void)
{
    extern unsigned long gePlayerTick(void);
    extern unsigned long gePortRenderedFrame(void);
    GeConsoleExecutionContext context;
    /* Reconcile renderer intent at the admitted game-thread boundary before either command
     * handlers or the simulation can observe this tick. */
    gePortConsolePauseGameTick();
    memset(&context, 0, sizeof context);
    if (ge_console.context_provider != NULL) {
        ge_console.context_provider(&context, ge_console.context_user);
    }
    /* These two identities come from the established port clocks, not from a provider that could
     * accidentally report a stale or renderer-local value.  gePortRenderedFrame() is the index
     * about to be submitted because the completed-frame counter increments after presentation. */
    context.game_tick = (uint64_t)gePlayerTick();
    context.render_frame = (uint64_t)gePortRenderedFrame();
    geConsolePump(&context);
}

int gePortConsoleAdmitGameTick(int admitted)
{
    extern int gePortSimShouldTick(void);
    if (admitted && gePortSimShouldTick()) {
        gePortConsoleGameTick();
    }
    return admitted;
}

unsigned int geConsoleResultCount(void)
{
    return ge_console.result_count;
}

int geConsoleResultAt(unsigned int index, GeConsoleResult *out)
{
    unsigned int slot;
    if (index >= ge_console.result_count || out == NULL) { return 0; }
    slot = (ge_console.result_head + index) % GE_CONSOLE_RESULT_CAPACITY;
    *out = ge_console.results[slot];
    return 1;
}

void geConsoleHistoryInfo(GeConsoleHistoryInfo *out)
{
    if (out == NULL) { return; }
    out->count = ge_console.result_count;
    out->capacity = GE_CONSOLE_RESULT_CAPACITY;
    out->dropped = ge_console.results_dropped;
    out->status = ge_console.results_dropped ? GE_CONSOLE_STATUS_RESULT_OVERFLOW
                                             : GE_CONSOLE_STATUS_OK;
}

void geConsoleClearHistory(void)
{
    ge_console.result_head = 0;
    ge_console.result_count = 0;
    ge_console.results_dropped = 0;
    memset(ge_console.results, 0, sizeof ge_console.results);
}
