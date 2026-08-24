/* Rulesets and horde mode.
 *
 * The premise, and why this is cheap. Enemy health, enemy damage, accuracy, ammo and
 * explosion strength are not properties of any level, model or asset -- they are floats the
 * game reads at load time, and the game already has one function that sets all of them:
 * lvlSetMultipliersForDifficulty() (lv.c:917). A ruleset is that function's output multiplied
 * by a table. No geometry, no setup file and no asset is touched, which is why CLASSIC,
 * HARDCORE and SURVIVAL are a config schema rather than a feature programme.
 *
 * Everything here is neutral by default. With no ruleset selected every accessor returns
 * 1.0, the hooks multiply by one, and the game behaves exactly as it did before -- which
 * matters, because "faithful" is the default this project is judged against.
 *
 * Percentages, not multipliers, in the user-facing surface: `enemy_health=200` reads the way
 * the design notes wrote it and the way a launcher would present it. They become multipliers
 * exactly once, here.
 *
 * ---------------------------------------------------------------- direction warning
 *
 * Two of these are inverted with respect to the name a player expects, and getting either
 * backwards silently makes HARDCORE easier:
 *
 *   g_AiHealthModifier scales the damage dealt TO a guard (chraction.c:2600,
 *   `damageToCause *= g_AiHealthModifier`). More enemy health therefore means a SMALLER
 *   value, so enemy_health DIVIDES it.
 *
 *   player health is `actual_health`, and bondhealth falls by damage/actual_health
 *   (bondview2.c:9917). More player health means a LARGER value, so it multiplies. The
 *   game's own 10x health cheat sets 10.0f, which is the shape being followed.
 *
 * enemy_reaction is deliberately described without a difficulty claim. It scales
 * g_AiReactionSpeed, which is the upper bound of a randomised AI timer
 * (`randomGetNext() % (333.33f * g_AiReactionSpeed)`, chraction.c:1680). Agent sets 0.2 and
 * 00 Agent sets 1.0, so a larger value lengthens that timer, and what that means for how
 * dangerous a guard feels has not been measured here. It is exposed because it is one of the
 * knobs the game itself varies by difficulty, and it is documented as what it does rather
 * than as what it might feel like.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    int enemy_health;      /* all values are percentages; 100 is unmodified */
    int enemy_damage;
    int enemy_accuracy;
    int enemy_reaction;
    int player_health;
    int player_armour;
    int ammo;
    int explosion_damage;
    int turret_damage;
    int horde;             /* 1 enables the wave system below */
    const char *blurb;
} ge_ruleset;

/* CHAOS deliberately does not randomise anything here. Randomisation belongs with the
 * randomizer work, which needs objectives to declare capabilities first, and a "chaos" that
 * silently reshuffles numbers per launch would make every bug report unreproducible. It is
 * a fixed, lopsided ruleset instead. */
static const ge_ruleset ge_presets[] = {
  /* name        eHP  eDMG eACC eRCT pHP  pARM ammo expl turr horde */
  { "classic",   100, 100, 100, 100, 100, 100, 100, 100, 100, 0,
    "the game as shipped" },
  { "hardcore",  200, 150, 130, 100,  50, 100,  50, 100, 100, 0,
    "tougher guards, less ammo, half the player health" },
  { "survival",  150, 125, 115, 100,  75, 100,  75, 100, 100, 1,
    "hardcore-lite, with endless waves" },
  { "chaos",     300, 200, 150, 100, 200, 200, 300, 200, 200, 0,
    "everything turned up" },
  { "horde",     100, 100, 100, 100, 100, 100, 200, 100, 100, 1,
    "stock difficulty, endless waves" },
};

static ge_ruleset ge_rs;        /* the active set, resolved once */
static int ge_rs_ready;

/* Horde tuning, read here so the game-side code in chr.c holds no configuration. */
static int ge_horde_per_kill  = 1;    /* replacements spawned per confirmed kill */
static int ge_horde_max_alive = 12;   /* hard ceiling on live spawned guards */
static int ge_horde_wave_kills = 10;  /* kills before the wave number advances */
static int ge_horde_growth    = 1;    /* extra per_kill added each wave, capped below */
static int ge_horde_per_kill_cap = 3;

static int ge_env_pct(const char *key, int fallback)
{
    const char *v = getenv(key);
    int n;

    if (v == NULL || *v == '\0') return fallback;
    n = atoi(v);
    if (n < 1)     n = 1;        /* 0% would mean invincible guards or zero ammo */
    if (n > 10000) n = 10000;
    return n;
}

static int ge_env_int(const char *key, int fallback, int lo, int hi)
{
    const char *v = getenv(key);
    int n;

    if (v == NULL || *v == '\0') return fallback;
    n = atoi(v);
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return n;
}

static void ge_rs_resolve(void)
{
    const char *want;
    unsigned i;

    if (ge_rs_ready) return;
    ge_rs_ready = 1;

    ge_rs = ge_presets[0];      /* classic: every value 100, horde off */

    want = getenv("GETV_RULESET");
    if (want != NULL && *want != '\0') {
        int found = 0;
        for (i = 0; i < sizeof(ge_presets) / sizeof(ge_presets[0]); i++) {
            if (strcmp(want, ge_presets[i].name) == 0) {
                ge_rs = ge_presets[i];
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("[getv][ruleset] unknown ruleset \"%s\"; known:", want);
            for (i = 0; i < sizeof(ge_presets) / sizeof(ge_presets[0]); i++) {
                printf(" %s", ge_presets[i].name);
            }
            printf("\n");
        }
    }

    /* Individual overrides apply on top of the preset, so `GETV_RULESET=hardcore
     * GETV_RS_AMMO=200` is a hardcore run with generous ammo. That composition is what makes
     * the preset list a starting point rather than a menu of five fixed options. */
    ge_rs.enemy_health     = ge_env_pct("GETV_RS_ENEMY_HEALTH",     ge_rs.enemy_health);
    ge_rs.enemy_damage     = ge_env_pct("GETV_RS_ENEMY_DAMAGE",     ge_rs.enemy_damage);
    ge_rs.enemy_accuracy   = ge_env_pct("GETV_RS_ENEMY_ACCURACY",   ge_rs.enemy_accuracy);
    ge_rs.enemy_reaction   = ge_env_pct("GETV_RS_ENEMY_REACTION",   ge_rs.enemy_reaction);
    ge_rs.player_health    = ge_env_pct("GETV_RS_PLAYER_HEALTH",    ge_rs.player_health);
    ge_rs.player_armour    = ge_env_pct("GETV_RS_PLAYER_ARMOUR",    ge_rs.player_armour);
    ge_rs.ammo             = ge_env_pct("GETV_RS_AMMO",             ge_rs.ammo);
    ge_rs.explosion_damage = ge_env_pct("GETV_RS_EXPLOSION_DAMAGE", ge_rs.explosion_damage);
    ge_rs.turret_damage    = ge_env_pct("GETV_RS_TURRET_DAMAGE",    ge_rs.turret_damage);

    {
        const char *h = getenv("GETV_HORDE");
        if (h != NULL && *h != '\0') ge_rs.horde = (*h != '0');
    }
    ge_horde_per_kill     = ge_env_int("GETV_HORDE_PER_KILL",   ge_horde_per_kill,   0, 8);
    ge_horde_max_alive    = ge_env_int("GETV_HORDE_MAX_ALIVE",  ge_horde_max_alive,  1, 64);
    ge_horde_wave_kills   = ge_env_int("GETV_HORDE_WAVE_KILLS", ge_horde_wave_kills, 1, 999);
    ge_horde_growth       = ge_env_int("GETV_HORDE_GROWTH",     ge_horde_growth,     0, 8);
    ge_horde_per_kill_cap = ge_env_int("GETV_HORDE_PER_KILL_CAP", ge_horde_per_kill_cap, 1, 8);

    /* Silent unless something is actually different, so a stock run's log is unchanged. */
    if (strcmp(ge_rs.name, "classic") != 0 || ge_rs.horde ||
        ge_rs.enemy_health != 100 || ge_rs.enemy_damage != 100 ||
        ge_rs.enemy_accuracy != 100 || ge_rs.enemy_reaction != 100 ||
        ge_rs.player_health != 100 || ge_rs.player_armour != 100 ||
        ge_rs.ammo != 100 || ge_rs.explosion_damage != 100 || ge_rs.turret_damage != 100) {
        printf("[getv][ruleset] \"%s\" -- %s\n", ge_rs.name, ge_rs.blurb);
        printf("[getv][ruleset]   enemy: health %d%% damage %d%% accuracy %d%% reaction %d%%\n",
               ge_rs.enemy_health, ge_rs.enemy_damage, ge_rs.enemy_accuracy, ge_rs.enemy_reaction);
        printf("[getv][ruleset]   player: health %d%% armour %d%% | ammo %d%% "
               "explosion %d%% turret %d%%\n",
               ge_rs.player_health, ge_rs.player_armour, ge_rs.ammo,
               ge_rs.explosion_damage, ge_rs.turret_damage);
        if (ge_rs.horde) {
            printf("[getv][ruleset]   horde: %d per kill (cap %d), max %d alive, "
                   "wave every %d kills, +%d per wave\n",
                   ge_horde_per_kill, ge_horde_per_kill_cap, ge_horde_max_alive,
                   ge_horde_wave_kills, ge_horde_growth);
        }
    }
}

/* ---------------------------------------------------------------- accessors
 *
 * Returned as float multipliers. The game-side hooks are one line each and hold no policy,
 * which keeps the decomp edits small enough to read in the patch. */

#define GE_RS_GET(fn, field)                    \
    float fn(void)                              \
    {                                           \
        ge_rs_resolve();                        \
        return (float) ge_rs.field / 100.0f;    \
    }

GE_RS_GET(gePortRulesetEnemyHealth,     enemy_health)
GE_RS_GET(gePortRulesetEnemyDamage,     enemy_damage)
GE_RS_GET(gePortRulesetEnemyAccuracy,   enemy_accuracy)
GE_RS_GET(gePortRulesetEnemyReaction,   enemy_reaction)
GE_RS_GET(gePortRulesetPlayerHealth,    player_health)
GE_RS_GET(gePortRulesetPlayerArmour,    player_armour)
GE_RS_GET(gePortRulesetAmmo,            ammo)
GE_RS_GET(gePortRulesetExplosionDamage, explosion_damage)
GE_RS_GET(gePortRulesetTurretDamage,    turret_damage)

#undef GE_RS_GET

/* True when anything differs from stock, so the game-side hooks can stay silent on a
 * faithful run without each of them repeating the comparison. */
int gePortRulesetChanged(void)
{
    ge_rs_resolve();
    return (ge_rs.horde ||
            ge_rs.enemy_health != 100 || ge_rs.enemy_damage != 100 ||
            ge_rs.enemy_accuracy != 100 || ge_rs.enemy_reaction != 100 ||
            ge_rs.player_health != 100 || ge_rs.player_armour != 100 ||
            ge_rs.ammo != 100 || ge_rs.explosion_damage != 100 ||
            ge_rs.turret_damage != 100);
}

int gePortHordeEnabled(void)      { ge_rs_resolve(); return ge_rs.horde; }
int gePortHordeMaxAlive(void)     { ge_rs_resolve(); return ge_horde_max_alive; }
int gePortHordeWaveKills(void)    { ge_rs_resolve(); return ge_horde_wave_kills; }
int gePortHordeGrowth(void)       { ge_rs_resolve(); return ge_horde_growth; }
int gePortHordePerKillBase(void)  { ge_rs_resolve(); return ge_horde_per_kill; }
int gePortHordePerKillCap(void)   { ge_rs_resolve(); return ge_horde_per_kill_cap; }
