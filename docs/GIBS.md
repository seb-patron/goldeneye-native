# Enemy gibs

Enemy gibs are an opt-in visual system for non-player character deaths. Every enabled policy
uses the same Quake-like effect: solid chunks launch away from the killing hit, bounce off the
level, settle on the floor, remain for about ten seconds, then fade. Retail behavior remains the
default.

## Policies

Set one policy in `goldeneye.cfg`:

```ini
gibs = off
```

| Value | Deaths that gib |
|---|---|
| `off` | None. This is the default. |
| `explosions` | Deaths whose final damage came from an area explosion or direct rocket impact. |
| `high_damage` | Deaths where the final hit dealt at least `4.0` internal damage units, after hit-location and weapon multipliers. |
| `always` | Every observed NPC death, including scripted deaths that have no recorded weapon cause. |

`on`, `true`, `yes`, `1` and `explosion` are compatibility aliases for `explosions`. The raw
developer gate accepts the canonical names too:

```bash
GETV_GIBS=high_damage ./getv/build-mac/goldeneye
```

The high-damage threshold deliberately describes the actual final hit rather than a hard-coded
weapon list. Powerful weapons qualify naturally, while a weak weapon does not become a gib weapon
merely because an earlier hit removed most of the target's health. GoldenEye splits a direct
rocket impact from its immediately following blast, so the direct impact is classified at the
`4.0` threshold to represent the complete rocket hit.

## The Quake-like effect

One gib event creates twelve opaque tetrahedral chunks: four larger pieces and eight smaller
ones. The recorded hit supplies the main launch direction, while a cosmetic event-local random
generator supplies spread and spin without consuming GoldenEye's gameplay random sequence.

Each moving chunk:

- follows gravity and tests movement against the level's stan geometry;
- reflects from walls with energy loss;
- makes up to three damped floor bounces;
- comes to rest flat on the floor;
- remains settled for roughly 600 simulation ticks (about ten seconds at 60 Hz); and
- fades during the following 60 ticks.

The effect is intentionally cosmetic. Chunks do not block actors, take damage, cross the network
as gameplay state or occupy normal prop slots. A fixed native sidecar stores their floor tile and
bounce state. At most 72 gibs can be active, and at least 24 entries in the original flying-debris
pool remain available to retail effects. When the gib budget is full, the oldest settled piece is
reused first.

The intact character is hidden with `fadealpha`, not destroyed. Scoring, objective state, AI death
observation, dropped items, Horde bookkeeping and corpse cleanup continue to use the original
`ChrRecord` and prop. A port-side registry keeps a gibbed body hidden through the stock corpse
timer, then forgets the pointer when normal cleanup runs or before its character slot is reused.

Players are unaffected because their damage follows a separate player path. The hook applies to
all `PROP_TYPE_CHR` actors, which includes ordinary guards but can also include friendly or
civilian mission characters. In `always` mode those actors gib too; allegiance filtering is an
explicit expansion item below rather than an unreliable assumption that every character is an
enemy.

## Implementation map

| Part | Responsibility |
|---|---|
| `getv/port/src/ge_gibs.c` | Parses the policy, evaluates death context, and tracks the last damaging hit and recycled character slots. |
| Damage hooks | Record actual weapon-hit damage, explosion damage and direct-rocket context without creating an effect at the weapon site. |
| `chr.c` death observer | Detects the first `ACT_DIE` or `ACT_DEAD` tick and asks the policy whether the recorded final hit qualifies. |
| `chraction.c` | Hides the original model and emits the one shared twelve-chunk effect. |
| `explosion.c` | Owns bounded allocation, solid rendering, stan collision, bounce, settling and fade. |
| `getv/patches/0023-enemy-gibs.patch` | Replays all game-source changes on a fresh decomp checkout. |

Keeping policy separate from presentation is important: `explosions`, `high_damage` and `always`
do not select different visuals. They only decide whether the same death event reaches the same
Quake-like emitter.

## Expanding the system

The current death context records victim, cause, final-hit damage and impulse. Extend that one
event instead of adding a separate visual effect at every weapon site:

1. **Add victim eligibility.** Define named scopes such as hostile-only, all NPCs and multiplayer.
   Mission allies and civilians share `PROP_TYPE_CHR` with guards, so eligibility must use mission
   role/AI data and have escort/protect-objective tests.
2. **Tune overkill with weapon context.** Add weapon ID, hit part and pre-hit health if designers
   need exceptions beyond the current actual-damage threshold. Keep the threshold policy as the
   fallback so new weapons do not require another effect hook.
3. **Add model-specific pieces.** Map `CHR_RENDER_PART` and model nodes across the body catalogue,
   hide detached nodes and spawn matching replacement meshes. Stump geometry should be original,
   redistributable project art; do not commit ROM-extracted meshes or generated asset C.
4. **Promote only interactive remains to props.** The current stan response is visual collision.
   Pieces that can block actors, take damage or affect objectives need a separate bounded prop
   type, room registration, save/net serialization and deterministic update order.
5. **Expose intensity controls.** Chunk count, launch force and settled lifetime can become
   validated settings. Preserve the 72-piece global budget or define explicit caps for each tier
   before raising counts, especially for Horde mode.
6. **Protect deterministic play.** Cosmetic changes must keep using the event-local visual PRNG.
   If pieces ever affect play, include their configuration in session negotiation and serialize
   them as authoritative state.
7. **Retain renderer parity.** Capture every new material on OpenGL and Metal. Prefer existing
   Fast3D commands until a renderer-specific path has coverage on both backends.

## Developer checks

The policy and config tests need no ROM:

```bash
bash getv/port/tests/run_tests.sh gibs
bash getv/port/tests/run_tests.sh config
```

With a local legal build, the integration gate makes the first eligible guard take real fatal
explosion damage at the selected simulation tick:

```bash
GETV_GIBS=explosions \
GETV_GIBS_SELFTEST=120 \
GETV_STAGE=9 \
GETV_INTROCAM=0 \
GETV_EXIT_FRAME=500 \
GETV_NO_AUDIO=1 \
./getv/build-mac/goldeneye
```

The first success line contains `killed=1 alpha=0 chunks=12`. A second line 360 ticks later reports
the persistent chunk count; this is beyond the old short-lived particle lifetime. Repeat with
`GETV_GIBS=off`: the death still occurs, while the stock body remains and `chunks=0`. The self-test
gate is developer-only, off by default, and does not require Horde mode.
