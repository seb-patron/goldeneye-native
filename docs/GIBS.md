# Enemy gibs

Enemy gibs are an opt-in visual effect for non-player characters killed by explosions. The
initial implementation replaces the intact character model with eight tumbling red chunks. It is
off by default and is not part of either graphics preset.

## Enabling it

Use the friendly configuration key:

```ini
gibs = explosions
```

`gibs = off` restores retail behavior. `on`, `true`, `yes` and `explosion` are accepted aliases.
The equivalent developer gate is `GETV_GIBS=1`; values other than `0` and `1` are not modes and
fail closed to off.

The initial scope is deliberately narrow:

- only a new death returned to `explosion.c`'s real blast dispatcher emits gibs;
- players are unaffected because the explosion system sends player props down a separate damage
  path;
- bullet, melee, fall, vehicle and scripted deaths keep their original models;
- scoring, objective state, AI death observation, dropped items, Horde bookkeeping and corpse
  cleanup still use the original `ChrRecord` and prop; and
- the short-lived chunks do not collide with the level or persist as props.

The hook applies to the game's `PROP_TYPE_CHR` actors. That includes ordinary guards and can also
include friendly or civilian mission characters; reliable allegiance filtering is one of the
recommended next steps below.

## How the first pass is built

| Part | Responsibility |
|---|---|
| `getv/port/src/ge_gibs.c` | Parses the selected policy and tracks which recycled character slots must stay visually hidden. |
| `getv/patches/0023-enemy-gibs.patch` | Hooks the real explosion-death transition, preserves normal cleanup, and creates/renders the chunks. |
| `explosion.c`'s flying-particle pool | Supplies bounded allocation, gravity, rotation, lifetime and an existing OpenGL/Metal render path. |
| `GETV_GIBS_SELFTEST` | Drives one real explosion death and reports the model alpha and active gib count for integration tests. |

No new game asset is required. Each chunk is an irregular quad using the existing flying-particle
material with opaque red vertex colors. The chunks are deliberately larger and inherit a strong
outward blast impulse so they clear the standard rocket explosion's long-lived flare cloud; their
four-second lifetime leaves a visible tail after the blast. The pool is already capped at 200
entries, so a burst can replace old cosmetic debris but cannot grow memory use. Gib creation and
retirement use no calls to the gameplay random generator; turning the effect on therefore does
not shift later AI or gameplay random choices.

The intact character is hidden with `fadealpha`, not destroyed. A small port-side registry keeps
that alpha at zero through the stock corpse timer, then forgets the pointer when normal cleanup
runs or before the slot is reused. This avoids borrowing any unknown `ChrRecord` flag and preserves
the original death lifecycle.

## Expanding the system

Build outward from one death event rather than adding a separate effect at every weapon. A useful
next abstraction is a single port-side death context:

```c
struct GeDeathContext {
    void *character;
    int cause;
    int weapon_id;
    int hit_part;
    float overkill;
    coord3d origin;
    coord3d impulse;
};
```

Populate it only when an action first changes to `ACT_DIE` or `ACT_DEAD`, then pass it to one
dispatcher. The current `newly_dead` policy argument protects that one-shot boundary. Extending it
in this order keeps the work reviewable:

1. **Classify every death path.** Add bullet deaths at `triggered_on_shot_hit()`, then melee,
   vehicle, fall and scripted causes where they actually commit death. Do not infer a cause later
   by observing `ACT_DEAD`; by then the weapon, hit part and impulse have often been lost.
2. **Define victim eligibility.** Add explicit modes such as enemies-only, all NPCs and multiplayer.
   GoldenEye's `PROP_TYPE_CHR` is not synonymous with hostile: mission allies and civilians share
   it. An eligibility helper should consider the victim's mission role and civilian flag, with
   tests for escort and protect-objective characters.
3. **Add policy without changing effects.** Extend `GeGibMode` and `key_gibs()` with named values
   such as `overkill` or `all`. Keep unknown raw integers disabled so an old binary never
   misinterprets a future config.
4. **Add real body-part dismemberment.** Map `CHR_RENDER_PART` and model nodes per body/head model,
   hide only the detached nodes, and spawn a matching replacement mesh. This needs coverage across
   the guard-body catalogue: the head is separately attached, but arm and torso hierarchies are
   not guaranteed to be identical. Sealed stump geometry should be original, redistributable
   project art; do not commit ROM-extracted meshes or generated asset C.
5. **Promote persistent pieces to props.** Pieces that bounce, collide, cross rooms or receive
   damage should have their own bounded prop pool, room registration, collision response and
   lifetime. Do not overload flying particles with gameplay collision. Specify caps and eviction
   first so a Horde wave cannot exhaust prop or room lists.
6. **Expose intensity controls.** Chunk count, lifetime and violence level can become validated
   named settings after their behavior exists. Keep `off` as the default and provide a low-detail
   option that avoids persistent remains.
7. **Protect determinism and network play.** Cosmetic variation must use a visual PRNG seeded from
   stable event data and must never consume `randomGetNext()`. If a future piece gains collision or
   can affect play, it is no longer cosmetic: serialize it in netplay, include its settings in
   session negotiation and lock its update order.
8. **Retain renderer parity.** Every new material needs OpenGL and Metal captures. Prefer existing
   Fast3D commands until a renderer-specific effect has a test on each backend.

## Developer checks

The policy and config parsers need no ROM:

```bash
bash getv/port/tests/run_tests.sh gibs
bash getv/port/tests/run_tests.sh config
```

With a local legal build, the integration gate makes the first eligible guard take real fatal
explosion damage at the selected simulation tick:

```bash
GETV_GIBS=1 \
GETV_GIBS_SELFTEST=120 \
GETV_STAGE=9 \
GETV_INTROCAM=0 \
GETV_EXIT_FRAME=180 \
GETV_NO_AUDIO=1 \
./getv/build-mac/goldeneye
```

The success line contains `killed=1 alpha=0 chunks=8`. Repeat with `GETV_GIBS=0`; the death should
still occur, while the line reports the stock body alpha and `chunks=0`. The self-test gate is
developer-only, off by default, and does not need Horde mode enabled.
