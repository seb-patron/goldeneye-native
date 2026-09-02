/* Narrow current-player adapter used by explicit-slot developer-console mutations.
 *
 * GoldenEye's original mutation functions operate on an implicit current-player cursor.  This
 * wrapper is the only console path allowed to move that cursor: it selects the requested slot,
 * performs one bounded operation, and restores the previous slot on every exit path.
 */
#ifndef GE_CONSOLE_PLAYER_ADAPTER_H
#define GE_CONSOLE_PLAYER_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeConsolePlayerGameApi {
    int (*current_player)(void);
    void (*select_player)(int slot);
    int (*god_enabled)(void);
    void (*set_god_enabled)(int enabled);
    /* Operates on the selected player and returns zero without a usable final weapon. */
    int (*give_weapon)(int weapon_id);
    int (*current_weapon)(void);
    int (*ammo_type_for_weapon)(int weapon_id);
    int (*max_ammo_for_type)(int ammo_type);
    void (*set_ammo)(int ammo_type, int amount);
} GeConsolePlayerGameApi;

/* Copies the API. Installation fails atomically when any operation is absent. */
int geConsolePlayerAdapterInstall(const GeConsolePlayerGameApi *api);

int geConsolePlayerAdapterGod(int slot, int enabled, int *previous, int *current);
int geConsolePlayerAdapterGive(int slot, int weapon_id);
/* `full` resolves to the selected weapon's maximum. The game setter retains its native
 * clip/reserve and clamping behavior for both forms. */
int geConsolePlayerAdapterAmmo(int slot, int amount, int full,
                               int *weapon_id, int *resolved_amount);

#ifdef __cplusplus
}
#endif
#endif /* GE_CONSOLE_PLAYER_ADAPTER_H */
