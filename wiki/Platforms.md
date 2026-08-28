# Platforms and renderers

One source tree. The platform layer under `getv/port/` is the only part that changes, and the
game itself is the same C everywhere.

## Where it runs

| Platform | Renderer | State |
|---|---|---|
| macOS, Apple silicon and Intel | OpenGL | builds and plays, this is the primary target |
| Linux, x86-64 and arm64 | OpenGL | builds and renders |
| Windows, x86-64 | OpenGL | builds and plays, native mingw-w64 |
| tvOS, Apple TV | GL ES or Metal | harness builds and deploys to a real device |
| iOS | Metal by default | `lib` and `app` build; deploy needs a paired device |

Linux says "renders" rather than "plays" on purpose. It has been built and run, on Debian 12
aarch64 with clang 14: 167 game objects, 746 assets, 40 audio, 60 port layer, no failures, and
the binary boots, loads Dam, renders under software Mesa and exits cleanly. Nobody has sat down
and played it with a keyboard, so that is as far as the claim goes.

macOS, Linux and Windows are the ones to use. The Apple mobile targets are a real harness with
a real build rather than a plan, but they are bring-up: tvOS defaults to the GL ES path for
historical reasons and iOS defaults to Metal, because iOS work started after Metal was already
the proven renderer.

There is an iOS Simulator build too, which needs no paired hardware and can be screenshotted,
which makes it the practical way to look at the Apple path.

## Two renderers

Both descend from the same Fast3D display-list interpreter. `gfx_rendering_api.h` is the seam:
a struct of function pointers that either backend fills in, so the game and the display-list
translation are shared and only the last step differs.

**OpenGL** (`gfx_opengl.c`) is the default on desktop and the older of the two.

**Metal** (`gfx_metal.mm`) is a native backend, not MoltenVK over Vulkan. Select it with
`GETV_RENDERER=metal`, which compiles `-DRAPI_METAL` and points `gfx_init()` at the Metal API
struct instead of the GL one. It matters beyond tidiness: desktop GL is deprecated on Apple
platforms, and GL ES on tvOS is deprecated too, so Metal is the path that does not have an
expiry date attached.

The two backends are kept at parity deliberately. The parallax height-map path exists in both,
written the same way, with the Metal version citing the GL one in its comments so the two do
not drift. Where they differ it is because the APIs differ:

- Metal has no global "active texture unit". Textures are bound per draw, by index, into the
  encoder. This is not a small detail: a bug that made every textured surface render black on
  the GL path was exactly a leaked active unit, and that class of fault cannot occur on Metal.
- Metal validates scissor rectangles against the render target, where GL silently clamps them,
  so `gfx_metal_set_scissor` clamps explicitly before handing the rect over.
- Metal checks both shader compilation and pipeline-state creation and fails loudly with the
  generated source printed. The GL path now does the same after a missing link-status check
  was found.

## Building for the Apple targets

```bash
cd getv
./build.sh all                     # tvOS device
GETV_RENDERER=metal ./build.sh all # tvOS, Metal instead of GL ES
./build_ios.sh all                 # iOS device, Metal by default
./build_ios_sim.sh all             # iOS Simulator, no hardware needed
```

Deployment to a real Apple TV or iPhone needs a development team and a paired device.
`devicectl` and `xcodebuild` use different identifiers for the same device, which is worth
knowing before debugging why one of them cannot see it.

One constraint that does not go away: tvOS blocks reading the app container, and the device
cannot be screenshotted, so on-device rendering can only be confirmed by looking at the TV. The
Simulator is the answer when something needs actually inspecting.
