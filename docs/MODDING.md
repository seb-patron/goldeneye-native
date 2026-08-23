# Modding

An orientation for changing the game rather than playing it. Nothing here is a supported API;
this is a description of how the tree is arranged and where the seams are.

## Layout

```
vendor/ge-decomp/          the decompiled game, from n64decomp/007. Untracked.
  src/                     game code. Compiled straight to arm64.
  src/game/                the bulk of it: front-end, AI, weapons, camera, levels
  src/libultra/            SGI's N64 OS. Mostly excluded from the build; gu/ and audio/ are used.
  assets/                  asset sources, generated from your ROM
  include/                 the game's own headers, including its libultra shims

getv/port/                 the platform layer
  fast3d/                  display-list renderer: gfx_pc.c, gfx_opengl.c, gfx_cc.c, gfx_sdl2.c,
                           plus ge_sky_rdp.c for GoldenEye's hand-built RDP sky triangles
  src/                     OS, input, audio bridge, asset bridge, save, render loop, config
  audio/                   the software mixer
  mac/                     macOS entry point
  include/                 port-side headers

getv/build_mac.sh          the macOS build
getv/patches/              this port's diff against the decompilation
tools/                     Python generators (prototypes, link stubs, asset blobs, layout audit)
```

`vendor/` is gitignored, so every change the port makes to the decompilation lives in
`getv/patches/` and nowhere else. Re-cloning the decomp discards your work unless the patches are
re-applied.

They are split by *when* they can be applied, not by subject. `0001-source.patch` covers `src/`,
`include/` and `tools/`, and goes on immediately after cloning. `0002-assets.patch` covers eight
generated asset files, which do not exist until the ROM has been extracted, so it goes on at the
end of the asset pipeline. Keeping the split means regenerating each one over its own paths:

```bash
cd vendor/ge-decomp
git diff -- src include tools > ../../getv/patches/0001-source.patch
git diff -- assets/animationtable_data.h assets/font_dl.c assets/rarewarelogo.c \
            assets/font/fontBankGothic.c assets/font/fontZurichBold.c \
            assets/obseg/setup/e/UsetuplenZ.c assets/obseg/setup/j/UsetuplenZ.c \
            assets/obseg/setup/u/UsetuplenZ.c > ../../getv/patches/0002-assets.patch
```

Regenerating `0001` with a bare `git diff` instead will sweep the entire extracted asset tree into
it — several hundred megabytes of ROM-derived data, which must never be committed.

Generated, ROM-derived data is deliberately excluded from the patch — the audio segment, the
object-segment blobs, animation blobs, the images segment, per-model `Model.c` files. Regenerate
those with the commands in the README.

The renderer and the platform layer rebuild in seconds:

```bash
./build_mac.sh port && ./build_mac.sh app
```

Game code needs `./build_mac.sh lib` instead, which is slower but still parallel.

## The `GETV_*` environment gates

These are the de-facto mod surface. There are around 250 of them, read straight from the
environment at the point of use. They were built as A/B switches during the port — one gate per
behavioural change, so that any two builds could be compared without recompiling — and they are
the cheapest way to alter the game without touching a line of code.

### The convention

- A gate is an environment variable read with `getenv`, usually cached in a function-local
  `static` on first use.
- **Unset preserves stock behaviour.** A gate that changes what the game does when unset is a bug.
- `=0` reverts to the pre-fix behaviour. For fixes that are on by default, this is how you get the
  old code path back inside the same binary.
- `=1` enables, for gates that are off by default.
- Announce what you resolved to. A gate that applies silently is indistinguishable from one that
  failed to reach the consumer.

A minimal example, following the shape used throughout the tree:

```c
static int geMyGate(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GETV_MYTHING");
        on = (e != NULL && *e != '\0') ? (atoi(e) != 0) : 1;  /* default ON */
        printf("[getv] mything %s\n", on ? "on" : "off");
        fflush(stdout);
    }
    return on;
}
```

Any gate can also be set from the configuration file or the command line by its real name — the
config layer matches raw `GETV_` names before friendly ones, so a gate can never be shadowed:

```
GETV_STAGE = 34
```

```bash
./build-mac/goldeneye --GETV_STAGE=34
```

### The useful ones

**Where the game starts.**

| Gate | Effect |
|---|---|
| `GETV_STAGE=<n>` | Boot straight into a stage id, skipping the front-end. `90` is the normal title boot. Dam is `33`, Facility `34`, Cradle `41`, Cuba (the credits) `54`. |
| `GETV_MENU=<n>` | The front-end counterpart. Enters at a menu screen: `0` legal, `1` Nintendo, `2` Rareware, `3` eye intro, `4` logo, `5` file select, `6` mode select. Everything from file select onward is input-only with no timer path forward, so this is the only way to reach those screens headlessly. |
| `GETV_MP=<n>` | Boot into multiplayer with `n` players (2 and up). Six stages are multiplayer-only; loading them solo gives geometry and no setup at all. |
| `GETV_MP_SCENARIO=<n>` | Pick the multiplayer scenario. |
| `GETV_DIFFICULTY=<0..3>` | Force the difficulty. A direct `GETV_STAGE` boot never runs the difficulty menu, so without this you get whatever BSS left behind, which is Agent. Several AI lists branch on difficulty. |
| `GETV_UNLOCKALL=1` | Show every mission on the file-select screen. |
| `GETV_TICKFIELDS=<1..4>` | Video fields per simulation update. `1` is one update per field, which at 60 fps runs the game's 122 frame-counted files at 60 Hz. `2` gives a 30 Hz simulation with game time still real, because the delta the other 13 files scale by rises to match. Pair it with a render rate that matches or game time runs fast; `framerate=30` sets it to `2` for you. |

Stage ids are not contiguous and not all of them are levels. Eleven have no data in the ROM;
`docs/ROADMAP.md` and `docs/research/GE_GAME_FACTS.md` carry the full table, including which
stages are multiplayer-only and which were cut. Check it before concluding a stage is broken.

**Ending a run, and capturing it.**

| Gate | Effect |
|---|---|
| `GETV_EXIT_FRAME=<n>` | End the run after `n` rendered frames and exit cleanly. This is a fixed *frame* count, not a wall-clock timeout, so two runs with the same value do the same amount of work. |
| `GETV_SHOTFRAME=<n>` | Write a BMP of frame `n`. Costs one `glReadPixels` on exactly one frame; off unless set. |
| `GETV_SHOTPATH=<path>` | Where that BMP goes. Defaults to `getv_shot.bmp` in the working directory. |

Setting `GETV_EXIT_FRAME` also makes the keyboard pad idle by default — present, so the front-end
does not decide there are no controllers, but reporting nothing held. The Mac window takes keyboard
focus when it opens, so without this anything you type lands in the game, and a single stray edge
aborts a level's opening cinema. `GETV_KEYBOARD_IDLE=0` overrides.

**Driving the game without hands.**

`GETV_SCRIPT` injects controller input at the device level, upstream of the N64 bit mapping. Every
stage below it is the production path — the stick rescale, the C-button Schmitt triggers,
`osContGetReadData`, the 20-deep sample ring and its edge detector — so a scripted entry is
indistinguishable from a human holding the pad.

```
GETV_SCRIPT="<frame>:<keys>[:<hold>][,<frame>:<keys>[:<hold>]...]"
```

- `frame` is the poll tick, one per game frame — the same clock `GETV_EXIT_FRAME` counts.
- `keys` are `+`-joined and case-insensitive: `A B X Y START BACK Z L R DU DD DL DR CU CD CL CR`,
  plus `SX=<n>` and `SY=<n>` for stick counts in the range -80 to 80 (`SY` positive is up).
- `hold` is how many frames to hold, defaulting to 4.

A button must be held for at least two frames to register, because the press edge is derived from
consecutive samples. Overlapping entries OR together, so a stick can be held across several taps.
A live script forces port 0 present, so it works with no hardware attached.

`GETV_SCRIPT_PORT=<n>` selects which N64 port the script drives (default 0). `GETV_SCRIPT_TRACE=0`
silences the per-entry log, which is otherwise on and is the only proof an entry fired.

Example — boot to file select and press A on frame 120:

```bash
GETV_MENU=5 GETV_SCRIPT="120:A:6" GETV_EXIT_FRAME=181 ./build-mac/goldeneye
```

**Determinism and instrumentation.**

| Gate | Effect |
|---|---|
| `GETV_SEED=<n>` | Force the random seed. |
| `GETV_PADS=<n>` | Force `n` controller ports present. |
| `GETV_KEYBOARD=0` | Unbind the keyboard from port 0. |
| `GETV_INPUT_DEBUG=<n>` | Per-frame input trace. |
| `GETV_PROF=1` | Frame profiling. |
| `GETV_CULLSTAT=1` | Room-culling statistics. |
| `GETV_TEXDUMP=<n>` | Dump textures as they are uploaded. Takes a count, not a boolean. |
| `GETV_NOBG` / `GETV_NOPROPS` / `GETV_NOOBJ` / `GETV_NOFOG` | Suppress a class of draw work. Useful for isolating which subsystem is responsible for something on screen. |

**Reverting a fix.** Most of the correctness fixes in this port sit behind a gate that is on by
default. Setting it to `0` restores the pre-fix behaviour in the same binary, which is how to check
whether a fix is responsible for something you are seeing. A few worth knowing:
`GETV_GFXPOOL` (display-list pool sizing), `GETV_VTXSWAP` (room vertex endianness),
`GETV_MIPCROP` and `GETV_PALOFF` (texture decode), `GETV_NANLEDGE` (camera ledge correction),
`GETV_SFXGUARD` (out-of-range sound ids), `GETV_MODELTEX` (model texture expansion).

One known harness defect: `GETV_GUN_SKIPINTRO` suppresses the sky. If you use it, the sky being
absent is your own doing.

**Build-time, not runtime.** `GETV_DEBUGMENU=1` is read by `build_mac.sh`, not by the game. It
enables the leftover debug menu, which changes code generation and repurposes the Start button:

```bash
GETV_DEBUGMENU=1 ./build_mac.sh lib && ./build_mac.sh app
```

Its level select does not work — those entries are gutted no-ops. Use `GETV_STAGE`.


**Looking at why a surface is the wrong colour.**

These print and change nothing. Run the same stage with and without one and compare; run a
stage that is known good, usually Dam (`GETV_STAGE=33`), as a control in the same session.

| Gate | Effect |
|---|---|
| `GETV_CCTRACE=<n>` | Every `G_SETCOMBINE`, both one- and two-cycle halves, plus the live combiner at each `G_SETTIMG`. Cycle-1 RGB is four 3-bit slots, `(a - b) * c + d`, in the `CC_*` order in `gfx_cc.h`; cycle 2 is four 4-bit slots at bit 32. |
| `GETV_CIPROBE=<n>` | Per CI decode: pixel address, palette pointer, the palette's byte offset from the pixel block, distinct source indices, and distinct resolved colours. `distinct_col` far below `distinct_idx` is a palette fault; `distinct_idx == 1` is an image fault. |
| `GETV_PALTRACE=1` | The `gDPLoadTLUT07` fields and the byte offset derived from them. |
| `GETV_ENVTRACE=<n>` | Every environment colour the game sets. Worth reaching for whenever a combiner resolves to the shape `(x - ENV) * 0 + ENV`, which outputs the environment colour flat and makes the texture and palette irrelevant to the result. |
| `GETV_LIGHTTRACE=1` | Per-frame census of which colour-combiner mux codes were seen, and which fell through to the constant zero. A code marked `(DROPPED)` that the level actually relies on is a decode gap. `COMBINED` in cycle 1 is dropped legitimately -- it is undefined on hardware. |

One invariant is worth knowing before reading any of it: this engine stores a CI texture's
palette immediately after its pixel block, so `GETV_CIPROBE`'s `d` normally equals `blksz`. It
holds for every decode on Dam and all but two on Depot, and those two are the known Depot
ground-colour defect. A decode where `d != blksz` is using some other texture's palette.
## Where the game data lives

Everything here is generated from your ROM and is untracked.

- **Levels.** `vendor/ge-decomp/assets/obseg/`, split by kind: `bg/` for room geometry and
  portals, `setup/` for object placement, spawn pads and AI lists, `stan/` for the walkable
  collision mesh, `brief/` for mission briefings, `text/` for strings. Each has its own
  `Makefile.*`. A stage needs both a background and a setup file; several stage ids have one and
  not the other, which is why they can never load.
- **Models.** `assets/obseg/chr/` (characters), `gun/` (weapons), `prop/` (props). The port
  consumes these as blobs converted by `tools/gen_obseg_blobs.py`; the decompiled per-model
  `Model.c` representation exists but does not compile and is not needed.
- **Textures.** `assets/images/`, driven by `assets/images.def` and the generated
  `imagelist.csv`. `assets/oddtextures.c` holds the ones that do not fit that scheme.
- **Fonts.** `assets/font/`, plus `font_chardata*.c` for the per-region character tables.
- **Audio.** `assets/music/` — instrument banks (`sfx.ctl` and friends) and sequences.
- **Animation.** `assets/animationtable_*.c` and the generated animation segment.

Model files and room vertex data in the ROM are big-endian and must be byte-swapped on load; the
conversion happens in the port's asset bridge, not in the game code.

## Things that will bite you

These are properties of this tree, not general C advice.

**The game ships its own `stddef.h`.** `vendor/ge-decomp/include/stddef.h` is on the include path
ahead of the system headers and its entire body is commented out. Including `<stddef.h>` from game
code therefore defines nothing — no `size_t`, no `NULL`, no `offsetof`. The same applies to several
other headers under `include/`, which are SGI-era shims rather than the system ones.

**`atof` returns 1.0 regardless of input**, despite a correct declaration in the tree's
`stdlib.h`. Use `strtod`. This is particularly nasty for env-gate parsing: a float knob read with
`atof` silently becomes 1.0, so an experiment you believe is running at 32x scale is running at 1x
and you conclude the setting had no effect. Print the parsed value back before using it.

**Familiar libc names can resolve to game functions.** `math_asinacos.c` defines `u16 acos(s16)`
and `s16 asin(s16)` — angle-table lookups, not libm. Code that calls `acos` expecting a double
gets nonsense; the symptom that caught this was an angle reported as 1877468 degrees, which is
32767 times 180/pi. Before calling any libc-looking function from game code, check whether the
decompilation defines its own. `atan2f` and `sqrtf` are unambiguous.

**`<PR/os.h>` and `<string.h>` will not coexist.** The N64 OS header redeclares `bcopy`, `bcmp`,
`bzero` and `sprintf` with the N64's `int`-length signatures, while macOS's fortified headers
define the same names as macros. Including them in either order fails to compile. Platform-layer
files that need `memcpy` more than they need the OS header include `<PR/ultratypes.h>` alone.

**`offsetof` in a static assertion is not what you want.** Use `__builtin_offsetof`, and verify
the assertion is armed by deliberately breaking it before trusting a layout proof.

**Struct layout differs from N64 everywhere.** `sizeof(Gfx)` is 8 on N64 and 16 here, pointers are
8 bytes rather than 4, and `tools/audit_struct_layout.py` counts 251 real layout hazards across 39
structs. Seventeen of those are file-backed and must keep N64 offsets exactly; the rest are
runtime-only and may grow. Anything that sizes a buffer in bytes for a struct that changed size is
a latent bug — that exact mistake halved every stage's display-list capacity for months, silently,
with no terminator and no error when it overflowed.

**Link order is load-bearing.** The build produces a static archive rather than linking objects
directly, and that is not a packaging preference. A direct object link pulls in files the port
compiles but never calls, and fails on around thirty N64 linker-script and hardware symbols. For
the same reason `-dead_strip` is required at the link step: `ld64` strips before it checks for
undefined symbols, so a reference from a dead atom is not an error. Removing the flag does not link
more of the game, it just breaks the build.

Also: the asset object directories contain colliding bare symbols (`header`, `room_data_table`),
harmless only because those objects are never pulled in. Do not add `-force_load`.

## Reading further

`docs/ROADMAP.md` is the working log — what is fixed, what is open, and the reasoning behind most
of the gates. `docs/research/` holds background on the engine, the N64 RCP, and the toolchain.
Both are internal working documents rather than user documentation, but they are where the detail
is.
