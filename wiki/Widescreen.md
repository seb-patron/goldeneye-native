# Widescreen and video

## Widescreen

On by default. It widens the view rather than stretching or cropping it: the projection is
recomputed against the window's real aspect ratio, so a 16:9 window shows more of the room to
either side than a 4:3 one, with the geometry unstretched and the HUD placed against the true
window edges. Split screen gets the same treatment, so a two-player horizontal split fills the
window in both panes.

Set `widescreen = 0` in the config, or `GETV_WIDESCREEN=0`, for the retail 4:3 image
pillarboxed inside whatever window you gave it.

The way it works is worth knowing if you go looking in the renderer. The game describes
everything in its own 320x240 space and does not know the window exists. Rather than rewrite
that, the port treats the framebuffer as if the content's native width were
`height * window_aspect`, which makes the horizontal and vertical scale factors equal by
construction: one uniform fill scale, no pillarbox margin, no stretch, and no second code path
to keep in step with the first.

Three things have to agree for that to hold, and each was a separate bug at some point: the
projection, the viewport, and the scissor. If any one of them keeps the game's original 320
while the others use the effective width, the result is a black band down one side with the HUD
clipped off, which is exactly what a stretched-viewport-but-narrow-scissor looks like.

A four-player quadrant is roughly half the native width and is deliberately left alone. It
keeps clipping where the game told it to.

## Resolution and supersampling

| Key | Values | Default |
|---|---|---|
| `resolution` | `WIDTHxHEIGHT`, `fullscreen`, `native` | `1280x960` |
| `supersample` | `1`, `2` | `1` |
| `filtering` | `point`, `bilinear`, `three-point` | `three-point` |
| `fov` | degrees | the game's own |

`supersample = 2` renders at double size and downsamples. It is not a neutral speed knob: it
changes the framebuffer size and therefore the heap layout, so never compare a measurement
taken at one setting against a baseline taken at the other. State the setting alongside any
number you report.

`three-point` is the N64's real sampling, and it is the default because it is what the hardware
did. `bilinear` is the modern smoothing most people expect. `point` is unfiltered.

There is also anisotropic filtering behind `GETV_ANISO`, off by default. It is applied only
where the game asked for linear filtering, which keeps it away from the HUD, the watch faces
and text. Those are drawn with point sampling on purpose and anisotropy would only blur them.

## Frame rate

Covered properly in [Frame timing](Frame-timing), because in GoldenEye it is a gameplay
question rather than a video one. The short version: the renderer is quick, and the reason the
config declines rates above 60 is that the game counts in frames rather than seconds.

Measured on an M1 at 1280x960 on Dam, with the cap and vsync released: **394 to 433 fps**
across three runs, and 60.8 video fields a second against the correct 60.0. The spread is the
honest report rather than the best run, and frame rates vary by stage, so this is one stage's
number and not the renderer's ceiling. It has not been the limiting factor for some time.

`GETV_VSYNC=0` releases the swap interval. Without it the frame rate cannot exceed the display
whatever the cap says, which is worth knowing before you benchmark anything.
