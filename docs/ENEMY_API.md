# Enemy awareness

## The gap this fills

`ge_world_api` answers *what does this level contain* -- objectives, waypoints, routes, and guard
spawn points from the extraction. That is static knowledge: true before the level starts, unchanged
by anything that happens in it. A bot steering by it alone is navigating a map of a room it is not
looking at.

`ge_enemy_api` is the other half: who is actually here, how hurt they are, whether they have
noticed anyone, and **where they think their target is**. The game has tracked all of it per
character since 1997. None of it was reachable from a bot or a mod.

## Why belief is the interesting part

`ChrRecord` (bondtypes.h:2454-2591) carries more than position and health:

| field | meaning |
| --- | --- |
| `alertness` | awareness level, driven by AI commands 86-8A |
| `lastknowntargetpos` | where this character last knew its target to be |
| `lastseetarget60` | frames since it had eyes on |
| `lastheartarget60` | frames since it heard something |
| `hearingscale` | rises when shot at |
| `chrseeshot`, `shotbondsum` | saw / took fire |

`lastknowntargetpos` is the one worth building on. The gap between what an enemy *believes* and
what is *true* is the whole basis for deciding whether to break contact:

- belief close to your real position, recently seen → you are tracked, and retreating straight
  away from the enemy is the worst option because that is where it is already aiming
- belief far or stale → contact is broken, and any move that does not re-enter its view keeps it
  broken
- several enemies believing the same wrong place → that place is where the fight is; it is a
  location to avoid, not an enemy to fight

A bot that knows enemy positions can fight. A bot that knows what enemies *believe* can disengage,
flank, and bait -- and those are the behaviours that read as intelligent.

`geEnemyThreatAt(x, y, z, radius)` exists for exactly this. It scores a **destination**, not a
neighbourhood, and it is deliberately not the same question as `geEnemiesNear`. In the test fixture
the origin has one living enemy within 120 units but three converging on it -- one of them 9000
units away, which is precisely the guard about to arrive and the one a proximity query misses.

It does **not** filter on alertness. An enemy walking to where it last saw someone threatens that
spot whether or not it is alert right now; alertness describes its state, the belief describes its
destination. Filtering here would hide the guard that is about to arrive.

## The source is installed, not linked

The live data is in `ChrRecord`, and the port layer is compiled without the decomp's include path
(`$portFlags` in `build_windows.ps1` covers `port/`, `port/include`, `port/fast3d`, `port/src` and
nothing else), so it cannot name that type. The established bridge is a flat accessor implemented
game-side -- `gePortPlayerPos` in `objective_status.c:717` is exactly that shape.

Declaring an extern the port cannot satisfy would turn a missing shim into a **link failure for
everyone**. So the source is registered at boot instead, the way `joySetPlaybackFunc` registers
input playback. With nothing registered, every query reports zero enemies and
`geEnemySourceInstalled()` returns 0 -- the absence is a readable runtime state rather than a
broken build.

## What is still needed, and it is not in my lane

`vendor/ge-decomp` is mac-getv's lane. The port half is written, compiled and tested; the
game-side shim is not, and it is about forty lines beside `gePortPlayerPos`:

```c
static int gePortEnemyCount(void)
{
    return g_NumChrSlots;          /* chr.h:215 */
}

static int gePortEnemyAt(int index, f32 *out, int count)
{
    ChrRecord *chr;

    if (index < 0 || index >= g_NumChrSlots) { return 0; }
    if (count < 14)                          { return 0; }   /* GE_ENEMY_FIELD_COUNT */

    chr = &g_ChrSlots[index];                                 /* chr.h:214 */
    if (chr->model == NULL) { return 0; }                     /* free slot: chr.c:1775 */
    if (chr->prop  == NULL) { return 0; }

    out[0]  = chr->prop->pos.x;    /* PropRecord.pos, bondtypes.h offset 0x08 */
    out[1]  = chr->prop->pos.y;
    out[2]  = chr->prop->pos.z;
    out[3]  = chr->damage;
    out[4]  = chr->maxdamage;
    out[5]  = (f32) chr->alertness;
    out[6]  = chr->hearingscale;
    out[7]  = chr->lastknowntargetpos.x;
    out[8]  = chr->lastknowntargetpos.y;
    out[9]  = chr->lastknowntargetpos.z;
    out[10] = (f32) chr->lastseetarget60;
    out[11] = (f32) chr->lastheartarget60;
    out[12] = (f32) chr->chrnum;
    out[13] = (chr->actiontype != ACT_DEAD) ? 1.0f : 0.0f;    /* per chr.c:202 */

    return 1 | 2 | 4 | 8;   /* POSITION | HEALTH | ALERT | BELIEF */
}
```

Then `geEnemySourceInstall(gePortEnemyCount, gePortEnemyAt)` once a level is running, and
`geEnemySourceInstall(NULL, NULL)` on teardown. **The uninstall matters**: a source outliving its
level hands out positions of characters that no longer exist, and they read as perfectly plausible
enemies.

Three details worth not improvising on:

1. **Return a field mask, not a bool.** A build that can reach position but not alertness should
   return `GE_EN_POSITION` alone, so a bot can see it is blind on awareness instead of reading
   every guard as oblivious.
2. **Check `count`.** Refuse rather than write past the array. Adding a field later must not
   silently shift every reader by one slot -- that failure is quiet and total.
3. **Health is inverted at the boundary, not here.** The game stores damage *taken*; the port
   converts to remaining. Getting it backwards reads a dying guard as healthy.

## Tests

`getv/port/tests/test_enemy.c` runs the whole API against a fake population with no game running --
which is the point of the install seam. It covers the health inversion, nearest-first ordering with
a `max` cap that keeps the closest rather than the first found, corpse exclusion, partial-data
rows, and the belief-versus-proximity contrast.

```bash
pwsh getv/port/tests/run_tests.ps1
```

The threat assertion failed on its first run claiming 4 where the code said 3. The code was right:
the fourth character reports position only, so it holds no belief and must not vote. The
expectation was wrong, which is the correct way round for a test to fail.
