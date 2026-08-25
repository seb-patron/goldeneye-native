# The player API

**Status: design, not yet implemented.** Two research threads are still outstanding and the
sections marked depend on them.

This is the seam that three separate features all need, which is why it is built once rather
than three times:

- **bots** drive a player slot
- **an external AI** drives a player slot and reads back what it did
- **LAN/WAN netplay** serialises exactly the same thing across a wire

A fourth consumer appeared during research and changes nothing structurally, which is the best
evidence the shape is right: an **LLM agent over MCP** is the same input injection and the same
state readout, at a different rate and granularity.

---

## 1. The one rule

**Tier 0 is a native C API, in-process, zero-copy. Everything else wraps it.**

This is not a preference. It is what the evidence says, and the spread is not subtle:

| Project | Engine ↔ learner | Throughput |
|---|---|---|
| EnvPool | C++ threadpool, zero-copy numpy | ~1M FPS (Atari) |
| NetHack Learning Environment | game compiled as a **C library**, in-process | **14,400 steps/sec** |
| ViZDoom | native C++, shared memory | ~7,000 FPS single-thread; >100k many-instance |
| Godot RL Agents | TCP sockets | ~3k/core |
| **MineRL / Malmo** | **TCP sockets, several writes per step** | **~10 FPS** |

Malmo is the cautionary case: the bottleneck was socket round-trips interacting with delayed
ACK, and the fix was `TCP_NODELAY` on both ends. A design that puts pixels on a socket has
already lost.

So: one C API. The Gymnasium wrapper, the PettingZoo multi-agent view, the netplay transport
and the MCP server are **siblings over the same handle**, never layered on one another. In
particular the MCP tier must not sit on top of the Python Gym wrapper, or it inherits that
layer's fixed-shape tensor assumptions.

---

## 2. Instancing: one instance per process, and that is the reference design

The decomp is full of file-scope state -- `port_os.c` alone owns a **32 MB static `ge_heap[]`**,
and the game has hundreds of `g_*` globals. Running N simulations inside one process would need
a context-struct refactor of the whole tree.

That is not a compromise forced on us. **ViZDoom does the same thing**: `DoomGame` spawns a
separate `vizdoom` process and communicates over shared memory, and Sample Factory's six-figure
throughput came from many such processes. One instance per process is the shape the fastest
reference implementation in this space already uses.

What that requires of us, and what it buys:

- The Tier-0 API must be written as though a handle existed (`GeSession *`), even while there is
  exactly one per process. That keeps in-process instancing a later optimisation rather than a
  rewrite.
- Observations go in **shared memory**, not on a pipe. The control channel (actions, flags) is
  tiny and can be anything.
- Process startup cost matters, because it is paid per environment. Boot-to-playable is already
  measurable via `GETV_EXIT_FRAME`; it becomes a tracked number.

---

## 3. The tick

**Input must be applied on a numbered simulation tick, not a wall-clock instant.** This is the
property netplay is built on and it is cheap now and expensive to retrofit.

The tick already exists: one per `osContGetReadData`, i.e. one pass over the four ports. It is
the same clock `GETV_EXIT_FRAME` counts and the same one `GETV_SCRIPT` schedules against.

**Unresolved and load-bearing.** Two mechanisms decouple simulation from rendering and the
project's own documents do not reconcile them:

- `GETV_TICKFIELDS=<n>` (`frametiming.c`, documented in `docs/FRAME_TIMING.md`) holds simulation
  to one update per *n* video fields by writing `frameDelay`.
- `GETV_SIMDIV=<n>` (`docs/WORK_SPLIT.md`) gates the tick calls inside `lvlRender` while draw
  calls run every frame.

`FRAME_TIMING.md` never mentions `GETV_SIMDIV`. Neither document says which of poll or tick
input lands on once they diverge. **The API cannot be built on "the tick" until that is settled**,
because under a divider the two are different numbers. This is the first thing to nail down and
it is shared with the Mac's frame-timing work.

### The ≥2-frame rule

`joyConsumeSamples()` (`joy.c:371`) derives `buttonspressed` from *consecutive* samples in a
20-deep ring: `buttonspressed |= cur & ~prev`. **A one-frame press is only registered by luck.**

This is not a quirk to document and move on from -- it decides the action API:

- An RL agent emitting single-tick actions would have inputs silently vanish, and the resulting
  policy would look like a learning failure rather than an API defect.
- Frame skip is therefore not merely an optimisation here, it is a *correctness* default.
  ViZDoom makes `tics` a first-class argument to the step call for performance reasons; we need
  it anyway.
- The API takes a hold duration, minimum enforced, and says so in the contract.

Also: a button left held forever produces exactly **one** press and then blocks the idle timers
other screens rely on. Release is part of an action, not an afterthought.

Injecting through `joySetPlaybackFunc` with exactly one sample per call makes this a non-issue
for the API -- `curlast == curstart + 1` gives a clean two-frame edge. It remains a real
constraint for anything driving the `GePadState` path.

### One step is not one world update

**The world is ticked `getPlayerCount()` times per rendered frame.**

`propsTick()` (`chrprop.c:2491`) walks the *entire* prop list and calls `chrTick`/`objTick`/
`explosionTick`/`playerTick` (`:2505-2524`) with no per-frame guard. It is called from inside
`lvlRender()`'s per-player loop (`lv.c:799`, loop at `:769-771`). **In a 4-player game every
guard is ticked four times per frame.**

The engine's answer is a *sim owner*: work that must happen once is guarded on
`get_player_position_in_shuffled(get_cur_playernum()) == 0` -- alarm and gas timers
(`chrprop.c:2553`), projectile integration (`propobj.c:4538-4559`), and about eight other sites.
The order is **shuffled every frame** by `shuffle_player_ids()` (`player.c:661-676`) so no player
gets a systematic advantage.

Three consequences the API must not paper over:

1. **A "step" is one input frame, not one world update.** Reward and observation are per frame;
   the simulation underneath ran N times. Any documentation that says "one tick advances the
   world once" would be wrong in multiplayer.
2. **`shuffle_player_ids()` consumes three `randomGetNext()` draws every single frame.** RNG
   state and player tick order are therefore welded together: any RNG divergence immediately
   reorders the whole simulation, which is about the worst possible failure mode for lockstep.
   It also means the RNG stream advances at a rate that depends on frame count, not on events.
3. **`mission_timer` is bound to slot 0, not to the shuffle** (`bondview2.c:8524`) -- the one
   simulation value hard-wired to a specific player. Correct only because slot 0 always exists.

---

## 4. Input injection

**The game ships the seam we want, and it is better than the one the port already uses.**

`joySetPlaybackFunc(fn, controllercount)` (`src/joy.c:360`), paired with
`joySetContDataIndex(1)` (`src/joy.c:851`), installs a callback that fills the pad ring
directly. It is GoldenEye's own demo-playback hook, and every property we need falls out of it:

- **It runs on the game thread, once per frame.** `joyConsumeSamplesWrapper()` calls it at
  `src/joy.c:412`, immediately before `joyConsumeSamples` on the same buffer, from the single
  main-loop tick at `src/boss.c:594`.
- **All four pads in one call.** The callback receives `struct contsample *` and fills
  `pads[0..3]`, so every slot is injected simultaneously and consistently -- which is precisely
  the "one tick authority" multiplayer needs.
- **It bypasses the connected-controller check entirely.** Every accessor guards on
  `(playbackcontcount < 0) && !(g_ConnectedControllers >> n & 1)` (`src/joy.c:540,551,562,573,584,595`);
  with `playbackcontcount >= 0` the hardware check is skipped. **Injected pads work with zero
  controllers attached.**
- **The rest of the game follows.** `joyGetControllerCount()` returns `playbackcontcount` during
  playback (`src/joy.c:277-280`), which is what `front.c:4842-4846` uses to default the player
  count. Rumble is a no-op while a playback func is installed (`src/joy.c:819-833`), so injection
  cannot trip hardware side effects.
- **Writing exactly one sample per call** gives `curlast == curstart + 1`, so `buttonspressed` is
  the clean edge between exactly two frames -- frame-exact press semantics, which sidesteps the
  ≥2-frame problem below rather than working around it.
- Menus, pause, character select, the debug menu and gameplay are all driven identically, because
  everything routes through `g_ContDataPtr`. There is no path that reads the pad another way.

**Crucially, this requires no change to the decomp.** `joySetPlaybackFunc` is a public function;
the port layer calls it with an `extern` declaration. `vendor/ge-decomp/` stays untouched, which
removes the ownership problem this design otherwise had.

### Why not the port-layer seam

The earlier plan was to inject into `struct GePadState` between `gePortInputPollPort()` and
`gePortDecodePad()` in `port_os.c`, which is where `GETV_SCRIPT` acts. That works and is
indistinguishable from a human -- but `osContGetReadData` runs on the **retrace thread at field
rate**, not on the game thread at frame rate. An agent wanting exactly one action per simulation
step should not be writing into a buffer that is filled on a different clock by a different
thread. Keep the `GePadState` path for the human-facing scripted-input harness; use
`joySetPlaybackFunc` for the API.

**Do not patch `lv.c:1732`.** It misses the second-pad reads at `bondview2.c:5293-5295`, all
menu input, and the debug menu.

### The reference implementation is already in the tree

`src/game/ramromreplay.c` is the record/replay system and it is the exact contract this API
should copy -- `ramrom_replay_handler` (`:273`) installed at `:469-470`, and the recording
counterpart `record_player_input_as_packet` (`:201`) at `:450`.

What it stores per block is the important part:

- the pad samples,
- **`speedframes` -- the frame delta** (`:256`),
- **`randseed` -- an RNG fingerprint** (`:257`), plus a checksum (`:259-260`).

And on playback it **aborts on mismatch**: `if (blk->randseed != (u8)g_randomSeed) ramromFadeToTitle();`
(`:312-315`). **This is a shipped desync detector.** It also snapshots and restores control
styles, characters, handicaps and both RNG seeds around a replay (`:377-409`).

The API should inject `(pads[4], frame_delta)` as a **pair**, not pads alone, and expose the
per-frame `g_randomSeed` fingerprint. Rare already worked out that this is the unit.

### Ranges

Sticks are N64 counts, **−80..80**. SDL full deflection delivers ~±127 against the N64's
practical ±84 (mgb64 FID-0015/0060). Deadzones are **subtracted, not clamped**: walk/turn ±5,
aim mode ±60, two-controller crouch ±30.

Actions are **continuous natively** -- aim delta, move/strafe as floats, plus a binary button
vector. Discretisation is a wrapper concern and the wrapper should offer both a `Box` and a
binned discrete space; VPT binned mouse movement into 1800 bins so one softmax head could drive
camera control, and which form a researcher wants depends on their algorithm.

### A semantic alternative, for later

`struct MoveData` (`src/bondtypes.h:4186-4248`) is the game's own abstract intent struct -- `analogWalk/Strafe/Pitch/Turn`, `triggerOn`, `aiming`, `digitalStep*`, `crouchUp/Down`, `btap`.
`bondviewProcessInput` (`bondview2.c:5143`) is where raw pad becomes that. Injecting `MoveData`
would skip control-style translation entirely -- attractive for a bot, wrong for netplay and for
anything that must be indistinguishable from a human. Pad level is the correct default.

Ranges and units are fixed by the existing harness and must not be re-invented:

- Sticks are **N64 counts, −80..80** (`SY` positive = up).
- SDL full deflection delivers ~±127 against the N64's practical ±84 (mgb64 FID-0015/0060).
  A bot emitting raw SDL magnitudes is outside the range the game was tuned against.
- Deadzones are **subtracted, not clamped**: walk/turn ±5 raw units, aim mode ±60, two-controller
  crouch ±30.

Actions are **continuous natively** -- aim delta, move/strafe as floats, plus a binary button
vector. Discretisation is a wrapper concern and the wrapper should offer both: a `Box` and a
binned discrete space. VPT binned mouse movement into 1800 bins precisely so one softmax head
could drive camera control; which a researcher wants depends on their algorithm and we do not
get to choose for them.

---

## 5. State readout

The facts exist; they are scattered across `GETV_STATE`, `GETV_GUN_DEBUG` and `GETV_CULLSTAT`
**as printf output**. Wanted: the same facts as data.

### What is reachable today

`gePortPlayerPos(idx, out)` → x/y/z, `getPlayerCount()`, `bossGetStageNum()`,
`gePortStateDump(frame)` (exists at `objective_status.c:731`, undocumented), plus the MP scoring
surface, which is already located and is exactly the reward signal an RL agent needs:
`g_playerPlayerData[killer].kill_counts[victim]`, `ks_ratio = kills*100/(shots+1)`,
`kd_ratio`, YOLT ordering, TLD flag counter (`mpmenu.c:411-412, 829`).

### What needs new game-side accessors

Angle, health, armour, current weapon, ammo, room, dead flag, and the visible-character list.
`struct player` is only visible inside the decomp, so these need an accessor block next to
`gePortPlayerPos`.

**That lands in `vendor/ge-decomp/`, which is on the do-not-touch list and is gitignored** -- it travels only through `getv/patches/0001-source.patch`, which the Mac is actively editing.
This is a coordination item, not a coding one, and it is the single dependency this work has on
the other machine.

### The thing we can do that ViZDoom's users cannot

ViZDoom's `labels_buffer` -- per-pixel segmentation plus per-object bounding boxes for *visible*
actors -- was singled out as the highest-value non-obvious feature, and auxiliary supervision on
"is an enemy visible" was the decisive ingredient in Arnold, which took the highest K/D in both
tracks of the Visual Doom AI Competition.

**We have a decompilation.** We know every actor's identity, type and render extent exactly,
without inference. Emitting labels and bounding boxes is cheap for us and is the most useful
observation we can offer.

Keep `objects` (all actors, including unseen) separate from `labels` (visible only), so partial
observability is the default and oracle policies are opt-in.

---

## 6. Termination

Return an explicit reason, never one boolean:

`GE_RUNNING`, `GE_DEAD`, `GE_OBJECTIVES_COMPLETE`, `GE_MISSION_FAILED`, `GE_TIME_LIMIT`,
`GE_STEP_LIMIT`, `GE_HOST_ABORT`.

Gymnasium splits `terminated` from `truncated` because the value target differs: on `terminated`
it is `r` with no bootstrap, on `truncated` it is `r + γV(s')`. Collapsing them silently biases
value functions on every time-limited task. The C layer must distinguish them at source and let
the wrapper map.

---

## 7. Determinism ledger

Netplay and reproducible RL both stand on this, so the known hazards are listed rather than
discovered later. Every entry is from the project's own measurements.

| # | Hazard | Consequence |
|---|---|---|
| 1 | **PAL and NTSC are compile-time constant sets**, not a scale factor (`CHRLV_FRAMERATE_F` 60/50, `CHRLV_DEFAULT_TIMER` 180/150) | **A US and a EU client can never share a lockstep session.** |
| 2 | **Render-only frames mutate simulation state** -- SFX voice teardown writes into `ChrRecord` (mgb64 FID-0089, P0) | "Render frames are side-effect free" is false here. Breaks the usual tick/draw split. |
| 3 | **Streets (level 29) is nondeterministic across processes** (FID-0046, verified) | At least one level cannot be lockstepped as-is. |
| 4 | **State hash is FP-optimisation and link-layout sensitive** (FID-0061/0131); we compile with neither `-ffp-contract=off` nor `-fno-fast-math` | Two differently-built clients disagree. Build flags become part of the protocol. |
| 5 | Seed is `randomSetSeed(osGetCount())`, and `osGetCount()` in the port is a **non-atomic static also incremented by the audio thread** | Measured deterministic today, but it is a race by construction. `GETV_SEED` forces it. |
| 6 | `g_GlobalTimerDelta` is **whole video frames, never fractional**; 122 of 135 files under `src/game` do per-frame work | You cannot feed this engine a fractional delta without retuning every constant. |
| 7 | Full-auto fire rate is gated on a **per-rendered-frame** counter, unscaled (FID-0056/0066) | Automatics fire 2-4× too fast at locked 60 Hz. |
| 8 | **`shuffle_player_ids()` draws 3 RNG values every frame** (`player.c:661-676`) to reorder the per-player tick | RNG state and tick order are welded together. Any divergence instantly reorders the whole simulation. |
| 9 | The frame delta is wall-clock derived (`frametiming.c:84`) and multiplied into **~310 simulation sites** via `g_ClockTimer` → `g_GlobalTimerDelta` (`lv.c:1112,1117`) | Deterministic in the port only *by accident*: `osGetCount()` returns `count += 1000`, so the quotient is exactly 1 every frame. `GETV_REALCLOCK=1` destroys it. |
| 10 | `chrObjRandom` is **stubbed to 0** in the port (`ge_link_stubs.c:60-61`) | Deterministic but not faithful -- `propobj.c` vertex jitter collapses to a constant. |
| 11 | Uninitialised locals read into logic: `stan.c:1425,1471`; `chr.c:3957-3968`; `chrprop.c:1377-1390` indexes `((u8*)g_Textures)[-8]` **on every shot** | Retail depended on deterministic RDRAM garbage. Do not "fix" blindly -- some are load-bearing. |

Precedent worth knowing: **the game already contains an input-replay format.** Attract-mode
demos are recorded controller inputs -- `ramromreplay.c`,
`struct ramromfilestructure { stagenum, difficulty, size_cmds, slotnum, totaltime_ms, savefile, ramrom_seed }`.
That is the netplay serialisation shape, seed included, already designed by Rare. The docs also
name its desync triple: any change to frame pacing, input sampling or `randomSetSeed` desyncs
attract mode.

There is also a ready-made desync harness design on file: FNV-1a 64 frame hashing of framebuffer
and display list, run 7× and diff. It gives the first diverging frame *and* a two-way classifier -- DL hash matches but framebuffer differs → renderer bug; DL hash differs → game logic.

---

## 8. Savestate

`ge_save_state(inst) -> blob` / `ge_restore_state(inst, blob)`, **in memory, never files.**
File-based savestates are orders of magnitude too slow for the uses that justify them.

The blob must include RNG state, tick counter and accumulated reward, or it is not a restore.
Restoring into a *different* instance must work -- that is how tree search parallelises.

Our state is largely the 32 MB `ge_heap[]`. That is fine for Go-Explore-style RL, where restore
replaces re-simulating from the start and cuts steps by "at least one order of magnitude". It is
**almost certainly too fat for per-frame rollback netplay**, which points at lockstep. Pending
the netplay research thread.

This also hands the MCP tier something genuinely novel: an LLM agent that can save, try, restore
and try again is doing explicit tree search over GoldenEye.

---

## 9. Multiplayer shape

The engine is already N-player generic, which is the foundation this rests on: one
`g_playerPointers[4]`, `MAX_PLAYER_COUNT` 4, and a single unguarded integer at `boss.c:478-489`
driving `init_player_data_ptrs_construct_viewports(playercount)`. There is no solo-versus-MP
branch. Split-screen is player-count-driven.

**One session owning the world, four agent views -- not four environments.** PettingZoo's
**Parallel** API is the correct native fit, because GoldenEye multiplayer is genuinely
simultaneous; expose `parallel_to_aec` for AEC-only algorithms.

**One tick authority.** ViZDoom's FAQ records that in synchronous multiplayer, frame skip
cannot be used per-agent because every agent must step before the server advances. Frame skip is
a property of the **session**, not of an agent's step call.

Known traps in the MP path, all already measured:

- **Co-op players do not move today.** Four explanations eliminated; what remains is MP-specific
  gating between the pad read and the movement apply. This is the Mac's queue item 4 and it
  blocks any bot demo in co-op.
- **`player_handicap[]` is BSS-zero and `MP_handicap_table[0]` is damage ×10.** Headless MP ran
  at 10× incoming damage. Any programmatic match setup must call
  `reset_mp_options_for_scenario()` then `init_mp_options_for_scenario(n)`, in that order.
- **NULL-stan spawn pads crash MP** (COMPLEX, 3 of 7 pads). Guarded by `GETV_MP_SPAWNGUARD`,
  default on; the retail `assert` is compiled out of release. Do not bypass it when respawning.
- **`CAMERAMODE_MP` is a swirl camera until ~frame 301.** Frame 61 is not gameplay.
- **Multiplayer never pauses** -- one player opening the menu does not stop the others. Good for
  netplay; means there is no natural stall point.

---

## 10. Bots

**Bots are a policy over the input API, not an AI-reuse project.** They are the *cheapest* of
the three consumers, not the most expensive, and the reason is worth stating plainly:

> **Retail GoldenEye multiplayer is already four players shooting each other.** Hit detection,
> damage, scoring, the kill matrix, respawn and the end-of-match awards all ship and all work.
> A bot needs **no AI whatsoever**. It needs pad input for slots 1-3.

Everything in the rest of this section -- the missing `CHR_BOND` target, the stubbed path tables,
the empty MP AI scaffolding -- applies **only** to reusing the *guard* AI, which we therefore have
no reason to do. It is recorded so nobody re-derives it.

### The hook supports mixing a human with bots

`joyPoll` writes real hardware to a **hardcoded** buffer:

```c
joy.c:476   osContGetReadData(g_ContData[0].samples[index].pads);
```

That happens regardless of whether playback is installed, and `joyConsumeSamplesWrapper` consumes
**both** buffers every frame (`joy.c:410-417`). `ramromreplay.c:323,331` already flips between
them mid-playback to poll the real pad for a human abort.

So one playback handler can fill all four slots from mixed sources:

- **human slots** -- copy the newest sample out of `g_ContData[0]`
- **bot slots** -- write the policy's output
- **network slots** -- write the peer's deserialised input for this tick

Which is the whole API in one sentence: *a bot, a network peer and an RL agent are the same
thing, differing only in where the four pad structs come from.*

### Players and guards are the same structure

A player is not structurally distinct from a guard. When a body model is needed -- always in
multiplayer -- the player is handed a slot by **the ordinary guard allocator**,
`init_GUARDdata_with_set_values` (`chr.c:1814`), which assigns `chrnum`, `ailist`, `aioffset`
and `aireturnlist` exactly as it does for any guard (`chr.c:1902-1904`). The caller then re-tags
**only the prop**: `prop->type = PROP_TYPE_VIEWER` (`bondview2.c:819`). `playerTick()` calls
`chrTick(prop)` (`:10877`) -- the identical entry point guards use.

So driving a player with an AI list is possible, and it **already ships as a feature**: the
opcode `SetBondsAiList` (`aicommands.def:501-504`) resolves `CHR_BOND_CINEMA = -8` to the
player's own `chrnum` (`chraction.c:9855-9861`). It is how cinematics move Bond.

### Why we should not use it for bots anyway

- In multiplayer the input path stomps the chr around every tick -- `actiontype = ACT_BONDMULTI`
  (`bondview2.c:11266`), `CHRHIDDEN_FREEZE` (`:11287`), `prop->pos`/`prop->stan` rewritten
  (`:11288-11291`). **`ACT_BONDMULTI` is not a case in `chrlvActionTick`** (`chraction.c:9485-9552`),
  so no locomotion tick runs. An AI list would execute and its movement opcodes would fight the
  input path every frame.
- **`init_path_table_links` is stubbed in the port.** Retail assigns waypoint `groupNum` and
  `dist` at load, which live pathfinding consumes. Its absence presents as AI misbehaviour, not
  as a crash.
- The AI has **no player-targeting vocabulary** -- the whole special-character set is
  `CHR_BOND_CINEMA, CHR_CLONE, CHR_SEE_SHOT, CHR_SEE_DIE, CHR_PRESET, CHR_SELF, CHR_OBJECTIVE,
  CHR_FREE`. There is not even a generic `CHR_BOND`; the AI addresses the player implicitly.
- MP arenas ship with `padnames` and `boundpadnames` both NULL -- no authored AI-list
  scaffolding to inherit.

**Conclusion: drive bots by pad injection, the same seam an RL agent and a network peer use.**
A bot becomes a policy function, and every one of the four obstacles above evaporates. AI-list
injection stays the right tool for scripted cinematic behaviour and nothing else.

### The trap to know in advance either way

**AI opcodes branch on render visibility.** `IFImOnScreen` tests `PROPFLAG_ONSCREEN`
(`chrai.c:1889`), a bit **set by the render pass** (`chr.c:2858`) from `posIsOnScreen`
(`propobj.c:13973`). `IFMyRoomIsOnScreen` tests `getROOMID_isRendered()` (`bg.c:2857`), a
per-room byte set by the portal visibility pass.

So guard behaviour is a function of camera position, portal state and viewport size. **A
headless build, a resized viewport, or an unrendered split-screen pane changes AI decisions.**
For an RL environment that runs headless by design, this is not a footnote -- it means the
training environment and the played game can differ behaviourally unless visibility is computed
even when nothing is drawn. Measure it early; it will present as a bot bug and it is a culling
question.

Also: AI lists are **cooperatively yielded** -- each chr runs part of its list per frame and must
issue `Yield`. A loop with no yield soft-locks the game.

---

## 11. Throughput

Training needs the game to run as fast as it can and never wait on a display.

- `GETV_FPS=0` removes the frame cap, but **vsync still gates** and there is **no headless mode**.
- ViZDoom deliberately uses a *software* renderer because at the resolutions RL actually uses
  (84×84, 160×120) GPU setup and readback dominate. **Our OpenGL path may be the wrong renderer
  for training**, and that is worth measuring before optimising it.
- Render features must be individually disableable -- HUD, crosshair, particles -- because each is
  pure cost during training.
- Requiring a display server is what makes an environment undeployable on a training box.

**Do not make the agent read its own HUD.** Export the numbers. The MP HUD is also
suspect: no health gauge, ammo counter or score was legible at any frame tested.

---

## 12. Phasing

Each phase is useful on its own and none of them requires the next.

1. **Settle the tick.** *Largely resolved by the seam.* The playback handler is called exactly
   once per `joyConsumeSamplesWrapper()`, i.e. once per main-loop iteration (`boss.c:594`), on the
   game thread -- so "the input tick" is unambiguous and is not affected by whatever `SIMDIV`
   does inside `lvlRender`. Injecting `(pads, frame_delta)` as a pair, as `ramromreplay` does,
   makes the step self-describing rather than dependent on the wall clock.
   What remains is the *documentation* conflict between `GETV_TICKFIELDS` and `GETV_SIMDIV`,
   which still needs reconciling with the Mac but no longer blocks this work.
2. **Input injection, Tier 0.** A `joySetPlaybackFunc` handler owned by the port layer, filling
   all four pads once per frame on the game thread, injecting `(pads, frame_delta)` as a pair
   and exposing the `g_randomSeed` fingerprint. **No decomp change required.** First consumer:
   re-point `GETV_SCRIPT` at this API instead of at `GePadState`, which proves the seam against
   something that already works rather than a demo invented to flatter it.
3. **State readout, what is reachable.** Position, counts, stage, objectives, MP scoring -- as
   data. No decomp changes needed.
4. **One bot that walks to a pad and fires**, per the queue's instruction to design against a
   real consumer. This is what tells us the API's shape is wrong before three consumers depend
   on it.
5. **Game-side accessors** for health/armour/weapon/ammo/angle/room. Needs Mac coordination.
6. **Savestate/restore**, in memory.
7. **Labels and bounding boxes.** The differentiator.
8. **Wrappers**: Gymnasium, then PettingZoo Parallel, then MCP. Siblings, not a stack.
9. **Netplay**, last, on a settled tick and a determinism ledger that has been closed out.

---

## 13. Licensing

The Perfect Dark port is MIT © 2022 Ryan Dwyer and may be adapted, but this project requires
every adaptation site to name the upstream repo, commit and file in a comment **and** be listed
in `docs/LICENSING.md`. Cite commit `514bf7a`, not the working tree -- several things in those
directories are this project's own tvOS additions and attributing them to Perfect Dark would be
wrong in both directions.

GoldenRecomp and `cblock85/GoldenEye64Recomp` are **GPL-3.0** and quarantined. `goldenpad` has
no top-level licence plus a GPL-3.0 obligation. Read for hazard intelligence, copy nothing.

ViZDoom, Gymnasium, PettingZoo and Stable-Retro are permissively licensed and are the references
this design follows; no code needs to be taken from any of them.

---

## 14. Netplay: the verdict

See [`docs/NETPLAY.md`](NETPLAY.md), which argues the opposite and has a working lockstep
session behind it. The disagreement is open; the determinism audit named there is what settles
it either way.

**Not lockstep.** Two independent reasons, either sufficient:

1. **Cross-platform bit-exactness is not realistically achievable.** x87 vs SSE vs ARM, and
   transcendentals differ between AMD and Intel, let alone between libc versions. And the
   Perfect Dark port -- the flagship N64 decomp port -- **ships at `-Og` because `-O2` breaks the
   game**, with `-fno-strict-aliasing` and `-fwrapv`. That is load-bearing undefined behaviour;
   the same source produces different behaviour under different flags. Ours has the same
   ancestry and §7 already records that our state hash is FP- and link-layout-sensitive.
2. **Lockstep adds RTT to your own aim.** GGPO's own guidance is that fighting games notice more
   than one frame of delay; FPS research puts degradation at ~100 ms. Age of Empires' 250 ms
   turn latency was unnoticed *in an RTS* -- aim is a continuous control loop and does not have
   that tolerance. Fine for LAN co-op, fatal for competitive deathmatch.

**Default: client-server with prediction and lag compensation**, which is also the cheap
retrofit -- it demands *nothing* of the simulation's determinism, which is the open-ended
expensive part of a decomp. Perfect Dark's `port-net` branch is the working reference: ENet,
Quake-style `SVC_`/`CLC_` split, client-authoritative movement with a forced-teleport escape
hatch, ~57-byte derived-state struct per 60 Hz tick, send-on-change via `memcmp`, remote players
**interpolated** (3 ticks ≈ 50 ms) rather than replayed.

### But rollback is more feasible here than for most 3D games

Worth recording, because the usual objection does not apply:

- **The simulation state is already one contiguous arena.** `memp` is a bump allocator with no
  per-allocation free (`memp.c`), spanning BSS→stack in fixed pools. That is exactly the property
  Slippi had to reverse-engineer for Melee, and Slippi does 4-player 60 Hz rollback with 7 frames
  against a **7.7 MiB** snapshot. Ours is smaller and its extent is known at link time.
- **Pointers are not a problem for rollback specifically.** GGPO's rebasing warning applies to
  games that re-`malloc`; rollback save/restore is same-process, same address space, so raw
  pointers inside a snapshot restore correctly.
- **The RNG is two `u64` integer seeds** with pure shift/xor and no time seeding -- 16 bytes,
  bit-exact on every platform.
- **4 players is exactly `GGPO_MAX_PLAYERS`**, and at 30 Hz the 8-frame prediction window is
  ~266 ms rather than ~133 ms.

If rollback is ever pursued: **GekkoNet over GGPO** (configurable prediction window, built-in
desync detection, replay record/playback, actually maintained), Slippi's declared-region-plus-
exclusion snapshot model, and the sync-test harness *before* any netcode.

### Two things to do first, whichever model wins

1. **Split "number of players" from "number of local viewports".** GoldenEye conflates them
   exactly as Perfect Dark did, and PD's netplay work had to do this refactor first; it is
   mechanical and it blocks everything else. Eight networked players must not imply eight
   split-screen panes.
2. **Give props a `syncid`**, seeded from array index at stage load and server-allocated
   thereafter. **Serialise identity, never pointers.** PD left the reverse lookup as a linear
   scan with `// TODO: make a map or something`; we can just build the map.

### Why this API is already the right shape

Perfect Dark added `u32 ucmd`, `bool isremote` and a client handle to `struct player`, plus
`CONTROLMODE_NA // dummy controls for remote players`, and routes local input, network input and
remote players through one branch. This design is the same idea reached independently:
`GeSlotSource` is `isremote`, `GePlayerInput` is `ucmd`, and `gePlayerPost()` is the single
boundary.

The difference is that **PD's bots do not use it** -- their simulants act directly on `chrdata`
as NPCs, so there is no `SVC_CHR_MOVE` in their protocol and *"simulants don't work in
netgames."* GoldenEye has no bot system to be constrained by, so bots here emit input into a
player slot like everything else, and bots, netplay and the RL agent share one path for free.

---

## 15. Synthetic input fires but does not move the player

Found while proving the input seam. **This is not a defect in the API** -- it affects the
existing `GETV_SCRIPT` harness identically -- but it blocks any bot, agent or replay that needs
to *walk*, so it is recorded here in full.

### What works

Stage 9, `GETV_BOT=0`, measured:

- the playback hook installs and the tick advances one per frame (59, 119, 179, …)
- **readback through joy.c's own accessors** -- `joyGetStickX/Y`, the same calls `lv.c:1732`
  makes to drive movement -- returns exactly what was posted: `stick=(25,60)`
- **injected FIRE reaches the gun: `shots=23`**
- state reads back; the seed fingerprint is live and changes every frame

So input delivery is proven, and proven twice over: buttons travel the whole way to the weapon
system through the same injection that carries the stick.

### What does not

The player does not walk. Position is frozen at `(-1356.0, 378.5, 2205.5)` from frame 240 to
1560 with `stick=(25,60)` landing every frame.

### Ruled out, with the measurement

| Hypothesis | Result |
|---|---|
| The API is not delivering input | readback returns the posted values, and fire produces 23 shots |
| Two-controller control style putting move on the second pad | reproduces under Honey (style 0) and Galore (style 5) alike |
| The intro camera freezing input (`bondviewFrozenMoveBond` zeroes buttons) | `cammode=0` -- gameplay -- throughout |
| Controls locked | `lv.c:596` calls `lvlSetControlsLockedFlag(0)` at boot and the mark prints |
| Something specific to the new seam | `GETV_SCRIPT` reproduces it exactly: `[getv][script] f=600 FIRE keys=0x0 stick=(0,60) hold=500` and the player does not move |

### What it means

This is almost certainly the same defect as the project's open **"co-op players do not move"**
item, whose remaining explanation was recorded as *"multiplayer-specific gating between the pad
read and the movement apply"*.

Two pieces of new information narrow it considerably:

1. **It is not multiplayer-specific.** It reproduces with a single player in a solo stage.
2. **It is movement-specific, not input-specific.** Buttons from the same injection reach the
   gun and fire 23 shots. So the pad read is fine and `bondviewProcessInput` is running; what
   fails is between the stick reaching `moveData.analogWalk` and the walk being applied
   (`bondview2.c:5961-6088`, `speedforwards = moveData.analogWalk / 70.0f`).

That is a much smaller search than "co-op movement", and it is in the Mac's lane
(`bondview2.c`). Worth handing over with these two facts rather than re-deriving them.

---

## Open

- The movement gate above -- §15. Blocks the bot demo; does not block the API.
- Whether the AI's render-visibility branching (§10) meaningfully changes behaviour headless.
  Needs measuring, not reasoning about.
- The `-O2` question: does our build have the same latent UB Perfect Dark works around at `-Og`?
  That is the determinism ceiling regardless of netcode model.
