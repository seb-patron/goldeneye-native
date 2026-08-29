# Two colour defects, measured

Both were reported as "confetti explosions and a paintball effect on by default". Neither is
the cheat system, and they are not the same bug. Numbers here come from `GETV_SHOTFRAME`
captures at a fixed frame, so any two runs are comparable.

## 1. Explosions render magenta instead of orange

**Cause: RGBA16 texel byte order.** `GETV_RGBA16BE=1` fixes it.

Reproduce with a rocket launcher, which is what makes an explosion available headlessly:

```
GETV_GIVE=25 GETV_STAGE=9 GETV_EXIT_FRAME=681 GETV_SHOTFRAME=680 \
GETV_SHOTPATH=/tmp/x.bmp GETV_SCRIPT="620:Z:20" ./getv/build-mac/goldeneye
```

Mean colour of strongly chromatic pixels in the explosion, Bunker 1, frame 680:

| `GETV_RGBA16BE` | frame md5 | chromatic px | mean RGB | reads as |
|---|---|---|---|---|
| 0 (current) | b5256214 | 207304 | (153, 76, 168) | magenta |
| 1 | 99248376 | 156078 | (204, 157, 66) | orange |
| 2 | 837fd2ce | 168327 | (206, 164, 71) | orange |

### What was ruled out, with the measurement that did it

- **Not the paintball cheat.** `GETV_IMPACT=1` on six PP7 hits: `type=7 apptype=1
  paintball_cheat=0`. apptype 2 is the random-colour row and only row 16 carries it.
- **Not the cheat flag array overrunning.** `CHEAT_PAINTBALL` evaluates to 15 against a
  76-entry array. The `/* 0x66 */` comments in `cheat.c`'s table are text ids, not enum
  values, and reading them as indices is the trap here.
- **Not fog.** Three levels with three different fog colours give the same magenta:
  Bunker 1 fog(0,16,64) -> (153,76,168); Jungle fog(24,32,0) -> (127,78,176); Caverns
  fog(8,0,8) -> (152,87,191). If fog were tinting it, the colour would follow the fog.
- **Not RGBA32.** `GETV_RGBA32BE` was added for this and all three modes gave a
  byte-identical frame. `GETV_LIGHTTRACE` then showed why: **there are no RGBA32 uploads at
  all** in the scene. The probe was inert, which is the useful result.

### `GETV_RGBA16BE=1` is the default now

The old census could not support promoting it, and said so: five levels compared at mode 0 and
mode 1 gave byte-identical frames, but `GETV_LIGHTTRACE` reported **zero RGBA16 uploads** in
those idle frames. Identical output from a setting that nothing exercises is not evidence of
safety, it is evidence the comparison was of the wrong frames.

Redone against frames that do reach it. Three stages, idle and firing a rocket, both modes,
`GETV_EXIT_FRAME=681` with the capture at 680:

| stage | firing | RGBA16 uploads | mode 0 vs mode 1 |
|---|---|---|---|
| 9 Bunker 1 | no | 0 | byte-identical |
| 9 Bunker 1 | yes | 166 | differ |
| 20 Silo | no | 0 | byte-identical |
| 20 Silo | yes | 0 | byte-identical |
| 25 Train | no | 0 | byte-identical |
| 25 Train | yes | 150 | differ |

The rule holds in all six pairs: **where no RGBA16 is uploaded the two modes are byte-identical,
and they differ only where it is.** So the setting cannot disturb anything that does not use the
format. And the only consumer found anywhere is the explosion and impact path -- a 681-frame
Bunker 1 run uploads no RGBA16 at all until the rocket is fired, which is why the earlier idle
census found nothing to compare.

Where it does apply it is the fix and not merely a change, on two independent stages:

| stage | mode 0 | mode 1 |
|---|---|---|
| 9 Bunker 1 | (140, 82, 167) magenta | (186, 154, 74) orange |
| 25 Train | (143, 84, 159) magenta | (179, 148, 71) orange |

A default build with no environment variable set now produces a byte-identical frame to an
explicit `GETV_RGBA16BE=1`. `GETV_RGBA16BE=0` restores the old behaviour, and mode 2 is still
there as the u16-store control.

The remaining subject, unchanged: the wall-hole impact rows 8..15 in `s_impactimages` are
RGBA/16b, and no scenario here has been shown to upload them specifically. Firing a rocket
exercises the format; which rows it exercises has not been separated.

## 2. Blue strips at Bunker doorways are the sky, not a texture

Reported as transparent textures or doorways shading blue. They are neither.

6722 pixels in an idle Bunker 1 frame are **exactly (0,16,65)**. Bunker 1's documented
flat-sky and fog colour is **(0,16,64)**; the one-unit difference is the 5-bit to 8-bit
scale on the way out. Every other blue pixel in the frame is a one-off.

An indoor level showing its sky colour means geometry that should occlude the background is
not being drawn there, so this is a room/portal fill question and **not** the texture
pipeline. It is a separate hunt from defect 1 and should not be folded into it.

Cross-check available: DAM and FRIGATE show their own sky as a large flat fill --
(15,46,93) x37162 and (16,48,96) x136107 -- which is correct outdoors and is what the same
measurement looks like when nothing is wrong.
