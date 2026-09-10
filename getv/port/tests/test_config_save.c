/* Unit tests for geConfigSave() -- writing settings back to goldeneye.cfg.
 *
 * Worth its own file rather than more cases in test_config.c: that one is about
 * READING, and every case here is about not destroying something. A config file is a
 * document the player edits by hand as well, so the failure this guards against is not
 * "the setting did not save" but "the launcher ate my comments", which is the kind of
 * thing nobody notices until the file they wanted is already gone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#define GE_PLATFORM_MAC 1
#include <sys/stat.h>
#endif

/* The externs ge_config.c expects from the rest of the port and the game. None of them
 * are reached by a save; they only have to exist for the unit to link. Same set as
 * test_config.c. */
unsigned int  configFiltering = 2;
unsigned int  configWidescreen = 1;
unsigned char ge_crosshair_r, ge_crosshair_g, ge_crosshair_b;
float ge_crosshair_scale;
void set_debug_testingmanpos_flag(int flag) { (void) flag; }
unsigned char g_CheatPlayerTextRelated[256];
int           num_chars_selectable_mp = 8;

/* port_paths.c owns these. Every test here points g_cfgpath at a real temporary file
 * first, so geConfigPath() returns before it needs either -- reporting "no user data
 * directory" keeps the fallback path from writing into a real one during a test run. */
int gePortUserDataDir(const char *org, const char *app, char *out, size_t outsz)
{ (void) org; (void) app; if (out && outsz) out[0] = '\0'; return -1; }
int gePortMakeDirTree(const char *path, unsigned mode)
{ (void) path; (void) mode; return -1; }

#include "ge_config.c"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) { printf("ok   %s\n", what); }
    else       { failures++; printf("FAIL %s\n", what); }
}

static char g_tmpdir[512];
static char g_cfg[640];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) { printf("FAIL cannot create %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static char *slurp(const char *path)
{
    static char buf[16384];
    FILE *f = fopen(path, "r");
    size_t n;
    if (f == NULL) { buf[0] = '\0'; return buf; }
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* Count non-overlapping occurrences -- a key must be updated, not duplicated. */
static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    size_t len = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += len; }
    return n;
}

/* geConfigSave() writes to geConfigPath(), which reads the file-scope g_cfgpath the
 * locate() step fills in. Setting it directly is why the unit is #included. */
static void point_at(const char *path)
{
    snprintf(g_cfgpath, sizeof g_cfgpath, "%s", path);
}

static void test_updates_existing_key(void)
{
    const char *keys[] = { "key.reload" };
    const char *vals[] = { "F" };
    const char *got;

    printf("# an existing live key is updated in place\n");
    write_file(g_cfg,
        "# my config\n"
        "fullscreen = 1\n"
        "key.reload = R\n"
        "deadzone = 20\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 1) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(contains(got, "key.reload = F"), "value replaced");
    ok(!contains(got, "key.reload = R"), "old value gone");
    ok(count_of(got, "key.reload") == 1, "updated rather than duplicated");

    /* Position matters: a key that moved to the end is separated from the comment
     * block that explains it. */
    ok(strstr(got, "fullscreen") < strstr(got, "key.reload"), "order preserved (before)");
    ok(strstr(got, "key.reload") < strstr(got, "deadzone"),   "order preserved (after)");
}

static void test_preserves_everything_else(void)
{
    const char *keys[] = { "deadzone" };
    const char *vals[] = { "12" };
    const char *got;

    printf("# comments, blank lines and unknown keys survive\n");
    write_file(g_cfg,
        "# --- a heading -----------------\n"
        "# an explanatory comment\n"
        "\n"
        "deadzone = 20   # trailing note\n"
        "\n"
        "# a key from a NEWER build that this one has never heard of. Dropping it would\n"
        "# make the launcher unsafe to run on a config written by a later version.\n"
        "some_future_key = 7\n"
        "moddir = mods\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 1) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(contains(got, "deadzone = 12"),        "target updated");
    ok(contains(got, "# --- a heading"),      "heading comment kept");
    ok(contains(got, "# an explanatory comment"), "body comment kept");
    ok(contains(got, "some_future_key = 7"),  "unrecognised key kept");
    ok(contains(got, "moddir = mods"),        "untouched key kept");
    ok(contains(got, "\n\n"),                 "blank lines kept");
}

static void test_uncomments_in_place(void)
{
    const char *keys[] = { "key.crouch" };
    const char *vals[] = { "Left Shift" };
    const char *got;

    printf("# a commented-out key is uncommented where it sits\n");
    /* The shipped template shows every binding as a commented example. Appending a
     * duplicate at the end of the file instead of enabling the documented one would
     * leave two lines for one setting, and a reader could not tell which won. */
    write_file(g_cfg,
        "# key.crouch = C,Left Ctrl\n"
        "# key.stand = V\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 1) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(contains(got, "key.crouch = Left Shift"), "enabled with the new value");
    ok(count_of(got, "key.crouch") == 1,         "not duplicated");
    ok(contains(got, "# key.stand = V"),         "the neighbouring example is untouched");
    ok(strstr(got, "key.crouch") < strstr(got, "key.stand"), "still first");
}

static void test_appends_unknown(void)
{
    const char *keys[] = { "aim_mode", "crouch_mode" };
    const char *vals[] = { "toggle",   "toggle" };
    const char *got;

    printf("# a key the file never mentioned is appended, labelled\n");
    write_file(g_cfg, "fullscreen = 1\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 2) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(contains(got, "fullscreen = 1"),   "original kept");
    ok(contains(got, "aim_mode = toggle"),    "first appended");
    ok(contains(got, "crouch_mode = toggle"), "second appended");
    ok(contains(got, "written by the launcher"), "appended block is labelled");
}

static void test_empty_value_comments_out(void)
{
    const char *keys[] = { "key.weapon_prev" };
    const char *vals[] = { "" };
    const char *got;

    printf("# an empty value comments the key out rather than deleting the line\n");
    write_file(g_cfg,
        "# cycle backwards\n"
        "key.weapon_prev = wheeldown\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 1) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(!contains(got, "key.weapon_prev = wheeldown"), "value removed");
    ok(contains(got, "# key.weapon_prev ="),          "commented, not deleted");
    ok(contains(got, "# cycle backwards"),            "its documentation survives");
}

static void test_no_file_yet(void)
{
    const char *keys[] = { "input_preset" };
    const char *vals[] = { "n64" };
    const char *got;
    char fresh[700];

    printf("# saving works on a first run, with no config file present\n");
    snprintf(fresh, sizeof fresh, "%s/brand-new.cfg", g_tmpdir);
    remove(fresh);
    point_at(fresh);

    ok(geConfigSave(keys, vals, 1) == 0, "save succeeded");
    got = slurp(fresh);
    ok(contains(got, "input_preset = n64"), "key written");
    remove(fresh);
}

static void test_round_trip(void)
{
    const char *keys[] = { "key.reload", "crouch_mode" };
    const char *vals[] = { "Left Alt",   "toggle" };

    printf("# what is saved is what read_file() reads back\n");
    write_file(g_cfg, "# start\n");
    point_at(g_cfg);
    ok(geConfigSave(keys, vals, 2) == 0, "save succeeded");

    /* The point of the round trip: a value containing a space ("Left Alt") must come
     * back whole. The reader splits on the FIRST '=' and takes the rest of the line,
     * so a scancode name with a space in it survives -- but only as long as nothing
     * quotes or trims it on the way out. Two thirds of SDL's modifier key names have
     * a space in them, so getting this wrong would break most of them. */
    {
        char *got = slurp(g_cfg);
        ok(contains(got, "key.reload = Left Alt"), "spaces in a value survive");
        ok(contains(got, "crouch_mode = toggle"),  "second key survives");
    }

    /* And it must be re-readable as config: parse it back and check the gates. */
    unsetenv("GETV_KEY_RELOAD");
    unsetenv("GETV_CROUCH_MODE");
    read_file();   /* the unit's own reader; it reads g_cfgpath, which points at g_cfg */
    ok(getenv("GETV_KEY_RELOAD") != NULL &&
       strcmp(getenv("GETV_KEY_RELOAD"), "Left Alt") == 0,
       "reader parses the saved binding back");
    ok(getenv("GETV_CROUCH_MODE") != NULL &&
       strcmp(getenv("GETV_CROUCH_MODE"), "toggle") == 0,
       "reader parses the saved mode back");
}

static void test_line_matcher(void)
{
    int commented = -1;

    printf("# the line matcher\n");
    ok(cfg_line_matches("deadzone = 20", "deadzone", &commented) > 0, "plain");
    ok(commented == 0, "not commented");

    ok(cfg_line_matches("  deadzone=20", "deadzone", &commented) > 0, "indented, no spaces");
    ok(cfg_line_matches("# deadzone = 20", "deadzone", &commented) > 0, "commented");
    ok(commented == 1, "reported as commented");
    ok(cfg_line_matches("#deadzone = 20", "deadzone", &commented) > 0, "commented, no space");
    ok(cfg_line_matches("DEADZONE = 20", "deadzone", &commented) > 0, "case-insensitive");

    /* A prefix must not match a longer key, or `key.look_up` would be updated by a
     * save of `key.look`. */
    ok(cfg_line_matches("deadzone_extra = 1", "deadzone", &commented) < 0, "prefix does not match");
    ok(cfg_line_matches("# a comment about deadzone", "deadzone", &commented) < 0,
       "prose mentioning the key does not match");
    ok(cfg_line_matches("", "deadzone", &commented) < 0, "empty line");
    ok(cfg_line_matches("\n", "deadzone", &commented) < 0, "blank line");
}

/* The template --write-config emits must be readable by the reader that ships with it.
 *
 * Not hypothetical: the template is a hand-maintained string literal several hundred
 * lines long, and every key added to it is one somebody has to remember to add to
 * apply() as well. If it ever drifts, every fresh install starts by printing "unknown
 * key" warnings at a player who has not configured anything yet -- and the value in the
 * template is silently not applied. */
static void test_template_round_trips(void)
{
    char path[700];
    FILE *f;
    int before;

    printf("# the embedded config template parses cleanly\n");
    snprintf(path, sizeof path, "%s/getv-template-%d.cfg", g_tmpdir, (int) getpid());

    f = fopen(path, "w");
    if (f == NULL) { ok(0, "could not write the template"); return; }
    fputs(DEFAULT_CFG, f);
    fclose(f);

    before = g_errors;
    point_at(path);
    read_file();
    ok(g_errors == before, "template produced no config errors");

    /* And it must contain the keys this change added, live or commented -- a template
     * that documents nothing about rebinding is a template nobody discovers it from. */
    {
        const char *t = DEFAULT_CFG;
        ok(contains(t, "input_preset"), "template documents input_preset");
        ok(contains(t, "crouch_mode"),  "template documents crouch_mode");
        ok(contains(t, "aim_mode"),     "template documents aim_mode");
        ok(contains(t, "key.reload"),   "template documents key.reload");
        ok(contains(t, "key.forward"),  "template documents a movement axis");
    }

    remove(path);
}

static void test_unchanged_lines_verbatim(void)
{
    const char *keys[] = { "fire", "deadzone" };
    const char *vals[] = { "rt",   "12" };
    const char *got;

    printf("# a key whose value did not change is left byte for byte\n");
    /* Found on the first real launch: saving rewrote `fire        = rt` as `fire = rt`,
     * dropping the file's column alignment on a setting that had not changed. */
    write_file(g_cfg,
        "fire        = rt          # right trigger\n"
        "deadzone    = 20          # percent\n");
    point_at(g_cfg);

    ok(geConfigSave(keys, vals, 2) == 0, "save succeeded");
    got = slurp(g_cfg);

    ok(contains(got, "fire        = rt          # right trigger\n"),
       "unchanged line kept exactly, alignment and trailing comment included");
    ok(contains(got, "deadzone = 12"), "a changed line is still rewritten");
    ok(!contains(got, "deadzone    = 20"), "and its old value is gone");
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0') { tmp = "/tmp"; }
    snprintf(g_tmpdir, sizeof g_tmpdir, "%s", tmp);
    snprintf(g_cfg, sizeof g_cfg, "%s/getv-test-%d.cfg", g_tmpdir, (int) getpid());

    test_line_matcher();
    test_updates_existing_key();
    test_preserves_everything_else();
    test_uncomments_in_place();
    test_appends_unknown();
    test_empty_value_comments_out();
    test_no_file_yet();
    test_round_trip();
    test_template_round_trips();
    test_unchanged_lines_verbatim();

    remove(g_cfg);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
