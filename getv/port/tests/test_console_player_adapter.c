/* Current-player cursor safety for console mutations, without a ROM or game binary. */
#include <stdio.h>
#include <string.h>

#include "ge_console_player_adapter.c"

static int failures;
static int fake_current;
static int fake_refuse_selection = -1;
static int fake_god[4];
static int fake_god_apply = 1;
static int fake_give_result = 1;
static int fake_give_changes_cursor = -1;
static int fake_weapon[4];
static int fake_ammo_type = 3;
static int fake_max_ammo = 400;
static int select_calls;
static int select_log[8];
static int give_calls;
static int ammo_calls;
static int last_weapon;
static int last_ammo_type;
static int last_ammo_amount;

static void check_i(const char *what, long long got, long long want)
{
    if (got == want) { printf("  ok    %s\n", what); }
    else { printf("  FAIL  %s: got %lld want %lld\n", what, got, want); failures++; }
}

static int game_current(void) { return fake_current; }

static void game_select(int slot)
{
    if (select_calls < (int)(sizeof select_log / sizeof select_log[0])) {
        select_log[select_calls] = slot;
    }
    select_calls++;
    if (slot != fake_refuse_selection) { fake_current = slot; }
}

static int game_god(void) { return fake_god[fake_current]; }

static void game_set_god(int enabled)
{
    if (fake_god_apply) { fake_god[fake_current] = enabled; }
}

static int game_give(int weapon_id)
{
    give_calls++;
    last_weapon = weapon_id;
    if (fake_give_changes_cursor >= 0) { fake_current = fake_give_changes_cursor; }
    return fake_give_result;
}

static int game_weapon(void) { return fake_weapon[fake_current]; }
static int game_ammo_type(int weapon_id) { (void)weapon_id; return fake_ammo_type; }
static int game_max_ammo(int ammo_type) { (void)ammo_type; return fake_max_ammo; }

static void game_set_ammo(int ammo_type, int amount)
{
    ammo_calls++;
    last_ammo_type = ammo_type;
    last_ammo_amount = amount;
}

static GeConsolePlayerGameApi game_api(void)
{
    GeConsolePlayerGameApi api;
    memset(&api, 0, sizeof api);
    api.current_player = game_current;
    api.select_player = game_select;
    api.god_enabled = game_god;
    api.set_god_enabled = game_set_god;
    api.give_weapon = game_give;
    api.current_weapon = game_weapon;
    api.ammo_type_for_weapon = game_ammo_type;
    api.max_ammo_for_type = game_max_ammo;
    api.set_ammo = game_set_ammo;
    return api;
}

static void reset_fakes(void)
{
    fake_current = 1;
    fake_refuse_selection = -1;
    memset(fake_god, 0, sizeof fake_god);
    fake_god_apply = 1;
    fake_give_result = 1;
    fake_give_changes_cursor = -1;
    fake_weapon[0] = 4;
    fake_weapon[1] = 8;
    fake_weapon[2] = 14;
    fake_weapon[3] = 19;
    fake_ammo_type = 3;
    fake_max_ammo = 400;
    select_calls = 0;
    memset(select_log, 0, sizeof select_log);
    give_calls = 0;
    ammo_calls = 0;
    last_weapon = -1;
    last_ammo_type = -1;
    last_ammo_amount = -1;
}

static void test_install(void)
{
    GeConsolePlayerGameApi api = game_api();
    int previous;
    int current;

    check_i("operation before install is refused",
            geConsolePlayerAdapterGod(0, 1, &previous, &current), 0);
    check_i("null API is refused", geConsolePlayerAdapterInstall(NULL), 0);
    api.set_ammo = NULL;
    check_i("partial API is refused", geConsolePlayerAdapterInstall(&api), 0);
    api = game_api();
    check_i("complete API installs", geConsolePlayerAdapterInstall(&api), 1);
}

static void test_god_restores(void)
{
    GeConsolePlayerGameApi partial = game_api();
    int previous = -1;
    int current = -1;

    check_i("god applies to explicit slot",
            geConsolePlayerAdapterGod(2, 1, &previous, &current), 1);
    check_i("god previous state returned", previous, 0);
    check_i("god current state returned", current, 1);
    check_i("god changed only target", fake_god[2], 1);
    check_i("original cursor restored", fake_current, 1);
    check_i("target and original each selected once", select_calls, 2);
    check_i("target selected first", select_log[0], 2);
    check_i("original restored last", select_log[1], 1);

    select_calls = 0;
    fake_god_apply = 0;
    check_i("unapplied god is refused",
            geConsolePlayerAdapterGod(3, 1, &previous, &current), 0);
    check_i("failure still restores cursor", fake_current, 1);
    check_i("failure restores after target", select_calls, 2);

    /* A failed re-install leaves the already validated API intact. */
    partial.current_weapon = NULL;
    check_i("invalid reinstall is atomic", geConsolePlayerAdapterInstall(&partial), 0);
    fake_god_apply = 1;
    check_i("prior API remains installed",
            geConsolePlayerAdapterGod(1, 1, &previous, &current), 1);
}

static void test_selection_and_give_failures_restore(void)
{
    int before;

    select_calls = 0;
    fake_refuse_selection = 3;
    before = give_calls;
    check_i("failed target selection refuses give", geConsolePlayerAdapterGive(3, 25), 0);
    check_i("failed selection never calls operation", give_calls, before);
    check_i("failed selection leaves original cursor", fake_current, 1);

    fake_refuse_selection = -1;
    select_calls = 0;
    fake_give_result = 0;
    check_i("game-side give refusal propagates", geConsolePlayerAdapterGive(0, 25), 0);
    check_i("give refusal restores cursor", fake_current, 1);
    check_i("give refusal selects target and original", select_calls, 2);

    fake_give_result = 1;
    fake_give_changes_cursor = 3;
    select_calls = 0;
    check_i("successful give reports success", geConsolePlayerAdapterGive(0, 14), 1);
    check_i("give receives weapon id", last_weapon, 14);
    check_i("cursor restored even if operation moved it", fake_current, 1);
    check_i("restore observes operation cursor change", select_log[1], 1);
}

static void test_ammo_restores(void)
{
    int weapon = -1;
    int resolved = -1;

    fake_give_changes_cursor = -1;
    select_calls = 0;
    check_i("numeric ammo applies", geConsolePlayerAdapterAmmo(2, 175, 0,
                                                               &weapon, &resolved), 1);
    check_i("ammo resolves selected player's weapon", weapon, 14);
    check_i("numeric amount returned", resolved, 175);
    check_i("ammo type resolved before set", last_ammo_type, 3);
    check_i("numeric amount reaches game", last_ammo_amount, 175);
    check_i("numeric ammo restores cursor", fake_current, 1);

    check_i("full ammo applies", geConsolePlayerAdapterAmmo(0, 0, 1,
                                                            &weapon, &resolved), 1);
    check_i("full resolves game maximum", resolved, 400);
    check_i("full maximum reaches game", last_ammo_amount, 400);

    fake_ammo_type = 0;
    select_calls = 0;
    ammo_calls = 0;
    check_i("weapon without ammo is refused", geConsolePlayerAdapterAmmo(3, 20, 0,
                                                                          &weapon, &resolved), 0);
    check_i("unsupported ammo does not call setter", ammo_calls, 0);
    check_i("unsupported ammo restores cursor", fake_current, 1);
    check_i("unsupported ammo selects target and original", select_calls, 2);

    fake_ammo_type = 3;
    fake_max_ammo = 1001;
    ammo_calls = 0;
    check_i("invalid game maximum is refused", geConsolePlayerAdapterAmmo(0, 0, 1,
                                                                           &weapon, &resolved), 0);
    check_i("invalid maximum does not call setter", ammo_calls, 0);
    check_i("invalid maximum restores cursor", fake_current, 1);
}

int main(void)
{
    printf("test_console_player_adapter\n");
    reset_fakes();
    test_install();
    test_god_restores();
    test_selection_and_give_failures_restore();
    test_ammo_restores();
    return failures ? 1 : 0;
}
