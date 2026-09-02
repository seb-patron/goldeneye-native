/* Explicit-slot wrapper for original game functions that use g_CurrentPlayer. */
#include <stddef.h>

#include "ge_console_player_adapter.h"

#define GE_CONSOLE_PLAYER_SLOTS 4
#define GE_CONSOLE_AMMO_MAX_INPUT 1000

static GeConsolePlayerGameApi ge_player_game;
static int ge_player_game_installed;

static int ge_player_begin(int slot, int *saved_slot)
{
    int previous;

    if (!ge_player_game_installed || saved_slot == NULL || slot < 0 ||
        slot >= GE_CONSOLE_PLAYER_SLOTS) {
        return 0;
    }
    previous = ge_player_game.current_player();
    if (previous < 0 || previous >= GE_CONSOLE_PLAYER_SLOTS) { return 0; }
    *saved_slot = previous;
    if (slot != previous) { ge_player_game.select_player(slot); }
    if (ge_player_game.current_player() != slot) {
        ge_player_game.select_player(previous);
        return 0;
    }
    return 1;
}

static void ge_player_restore(int saved_slot)
{
    if (ge_player_game.current_player() != saved_slot) {
        ge_player_game.select_player(saved_slot);
    }
}

int geConsolePlayerAdapterInstall(const GeConsolePlayerGameApi *api)
{
    if (api == NULL || api->current_player == NULL || api->select_player == NULL ||
        api->god_enabled == NULL || api->set_god_enabled == NULL ||
        api->give_weapon == NULL || api->current_weapon == NULL ||
        api->ammo_type_for_weapon == NULL || api->max_ammo_for_type == NULL ||
        api->set_ammo == NULL) {
        return 0;
    }
    ge_player_game = *api;
    ge_player_game_installed = 1;
    return 1;
}

int geConsolePlayerAdapterGod(int slot, int enabled, int *previous, int *current)
{
    int saved_slot;
    int before;
    int after;

    if (previous == NULL || current == NULL || (enabled != 0 && enabled != 1) ||
        !ge_player_begin(slot, &saved_slot)) {
        return 0;
    }
    before = ge_player_game.god_enabled() != 0;
    ge_player_game.set_god_enabled(enabled);
    after = ge_player_game.god_enabled() != 0;
    ge_player_restore(saved_slot);
    *previous = before;
    *current = after;
    return after == enabled;
}

int geConsolePlayerAdapterGive(int slot, int weapon_id)
{
    int saved_slot;
    int applied;

    if (!ge_player_begin(slot, &saved_slot)) { return 0; }
    applied = ge_player_game.give_weapon(weapon_id) != 0;
    ge_player_restore(saved_slot);
    return applied;
}

int geConsolePlayerAdapterAmmo(int slot, int amount, int full,
                               int *weapon_id, int *resolved_amount)
{
    int saved_slot;
    int weapon;
    int ammo_type;
    int resolved = amount;
    int ok = 0;

    if (weapon_id == NULL || resolved_amount == NULL ||
        (full != 0 && full != 1) || amount < 0 || amount > GE_CONSOLE_AMMO_MAX_INPUT ||
        !ge_player_begin(slot, &saved_slot)) {
        return 0;
    }

    weapon = ge_player_game.current_weapon();
    ammo_type = ge_player_game.ammo_type_for_weapon(weapon);
    if (ammo_type > 0) {
        if (full) { resolved = ge_player_game.max_ammo_for_type(ammo_type); }
        if (resolved >= 0 && resolved <= GE_CONSOLE_AMMO_MAX_INPUT) {
            ge_player_game.set_ammo(ammo_type, resolved);
            ok = 1;
        }
    }

    ge_player_restore(saved_slot);
    *weapon_id = weapon;
    *resolved_amount = resolved;
    return ok;
}
