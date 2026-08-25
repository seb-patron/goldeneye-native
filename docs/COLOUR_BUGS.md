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
  Bunker 1 fog(0,16,64) → (153,76,168); Jungle fog(24,32,0) → (127,78,176); Caverns
  fog(8,0,8) → (152,87,191). If fog were tinting it, the colour would follow the fog.
- **Not RGBA32.** `GETV_RGBA32BE` was added for this and all three modes gave a
  byte-identical frame. `GETV_LIGHTTRACE` then showed why: **there are no RGBA32 uploads at
  all** in the scene. The probe was inert, which is the useful result.

### Why `GETV_RGBA16BE=1` is NOT yet promoted to the default

**The census has no coverage.** Five levels compared at mode 0 and mode 1 gave byte-identical
frames, which reads like proof of safety and is not: `GETV_LIGHTTRACE` reports **zero RGBA16
uploads** in those idle frames. The only RGBA16 consumer observed anywhere is the explosion
flare, where the count moves 8 → 9 as the explosion appears.

### 🔑 Update: a second RGBA16 consumer exists, and it is trivially reachable

The paragraph above asks for "frames that actually exercise RGBA16 elsewhere". There are two,
in the intro, before any level loads:

```c
/* title.c:363 and title.c:367 -- the Rareware logo */
gDPLoadTextureBlock(gdl++, &D_02004FE8, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0,
                    (G_TX_NOMIRROR | G_TX_WRAP), (G_TX_NOMIRROR | G_TX_WRAP), 5, 5,
                    G_TX_NOLOD, G_TX_NOLOD);
```

Two 32×32 RGBA16 tiles under `G_TX_WRAP` with `masks = maskt = 5`, drawn under
`G_CC_MODULATEI` with a `gDPSetPrimColor` ramp. So the census claim should read "no RGBA16
consumer was observed **in the idle level frames sampled**", which is a statement about the
sample and not about the game.

This makes promoting mode 1 to default cheaper to justify than the paragraph above assumes:
capture the Rareware logo at modes 0, 1 and 2 and you have a second consumer, a wrapped tile
and a colour ramp all in one frame, with no level load involved.

So what is established is "mode 1 fixes explosions" and "nothing else measured uses RGBA16
yet". Promoting it needs frames that actually exercise RGBA16 elsewhere -- the wall-hole
impact rows 8..15 in `s_impactimages` are RGBA/16b and are the obvious next subject.

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

---

## 3. Paintball colours reported in play, and what they are NOT

Reported again during real play, on **both** explosions and bullet impact flashes. Not
reproducible headlessly across every angle tried. These negatives are recorded so nobody
spends the same hours on them again.

| ruled out | the measurement that ruled it out |
|---|---|
| apptype 2, the real paintball path | its tripwire is unconditional and stays **silent** on every scripted run, including dual RC-P90 held 200 frames while looking up |
| the paintball **cheat** | `cheatIsActive` returns 0; the id table is right (paintball is 15 against a 76-entry array); the parser uses exact `strcmp`; the commented example line in `goldeneye.cfg` does not apply |
| RGBA16 byte order **for impacts** | a gunfire frame is **byte-identical** with `GETV_RGBA16BE` 0 and 1 |
| RGBA32 | **no RGBA32 texture is uploaded at all** in any measured scene |
| CI palette format | `GETV_TLUTFMT` defaults ON, so IA16 palettes are already read as IA16 |

`GETV_RGBA16BE=1` is still correct **for explosions**: 43.3% of the explosion frame changes,
purple (64,64,128) to orange (192,192,64).

**The useful fact.** The formats actually uploaded during gunfire are `CI/4b`, `CI/8b`,
`IA/8b`, `I/4b` and `I/8b` -- **no RGBA of any width**. So whatever colours the impact flash is
a palette or intensity path, and the RGBA16 fix cannot reach it. Those are different bugs and
should stop being treated as one.

**The gap is the harness, not the game.** Scripted runs shoot walls and characters on a
fixed path; a person plays levels, surfaces and angles a script never reaches. Impact rows are
chosen by the surface struck, and scripted runs only ever resolve rows 1, 2 and 7 of sixteen.
`~/Desktop/GoldenEye-Diagnose.command` captures a real session with `GETV_IMPACT`,
`GETV_CIPROBE` and `GETV_LIGHTTRACE` together, which is worth more than another round of
guessing.

---

## 4. The impact DECAL cannot be the paintball colours. Proof.

`tex.c` holds every hit-type group, and each lists the impact rows that surface may use:

```
default {0x7}   stone {0x1}   wood {0x1}   metal {0x7}   glass {0x4,0x5,0x6}
water {0}       snow {0x1}    dirt {0x2}   mud {0x2}     tile {0x1}
metalobj {0x1,0x7}            character {0x2}            glass_xlu {0x11,0x12,0x13}
```

**No group contains `0x10`.** Row 16 is the only row with `apptype 2`, the per-channel
random 0/0xff that produces the paintball palette, and **no surface in the game can select
it**. The single way in is `cheatIsActive(CHEAT_PAINTBALL)` overwriting `impact_type = 0x10`
(`explosion.c:2025`), and that measures 0.

Every reachable row is `apptype` 0 or 1, both greyscale. So the impact **decal** is incapable
of drawing the reported colours, which explains why the unconditional apptype-2 tripwire never
fires and why the harness could not reproduce it.

**Rows 8-15 are also unreachable** -- no group references them. Those are the RGBA16
wall-holes, which is the independent reason `GETV_RGBA16BE` cannot affect impacts: the rows
that use that format are never selected.

**So the coloured flash is a different object drawn at the same moment as the decal.**
`chrprop.c` calls `check_if_imageID_is_light()` immediately after creating the impact, and
there is a spark/light path there. That is where to look next, not at `g_ImpactTypes`.

## 5. A real bug found on the way: the hit-type byte pun

`chrprop.c` selected the hit type with `((u8 *) (&g_Textures[n]))[0] & 0xf`.

`struct image_entry` is a **bitfield**: `hitSound:4`, then `hitTexture:4`, then
`dataoffset:24`. On big-endian MIPS the first-declared bitfield takes the HIGH bits of byte 0,
so that expression yields **hitTexture**. On little-endian arm64 the first field takes the LOW
bits, so the identical expression yields **hitSound**. Same source, different field.

Every other site already uses the names, and consistently: `chr.c:4252` and `:4270` take
**hitTexture** for the visual impact, `gunfire.c:3134-3139` take **hitSound** for the audio.
The pun was the only site not doing so, and it feeds a visual, so it means `hitTexture`.

Now selected by name, with `GETV_HITSEL` to A/B it (0 = the pun, 1 = hitSound, 2 = hitTexture,
default 2).

**Scope, honestly measured: 3 textures of 2698.** `hitSound` and `hitTexture` are equal
everywhere except three `HIT_GLASS` / `HIT_GLASS_XLU` pairs, so the pun was accidentally right
2695 times. Shooting translucent glass got the wrong impact group; nothing else did. **This is
not the paintball cause** and must not be reported as one.


---

## 6. SOLVED: the paintball impact flash was a struct offset, not a texture

**`glass2.c:648` read the bullet spark's colour at hardcoded byte offsets `0x28..0x2b`.**

`s_bullet_spark` carries a `void *unk0C`. That is 4 bytes on the N64 and 8 here, so every
field after it shifts and the struct grows:

| | N64 | this port |
|---|---|---|
| `sizeof(s_bullet_spark)` | 0x2c | **0x38** |
| colour bytes at | 0x28 | **0x30** |
| what the code read | 0x28 | 0x28 = **`unk20`, a float** |

So every bullet spark took its RGBA from the raw bytes of a float. Measured, with the named
fields and the old pun printed side by side on the same sparks:

```
byname = (255,255,200,255)   every spark -- a warm white, which is what a spark is
oldpun = (97,59,47,194)  (96,116,190,65)  (93,181,212,65)  (8,220,6,66) ...
                                            cyan            NEON GREEN
```

Arbitrary, different per spark because `unk20` differs per spark, and appearing wherever a
shot lands. Reported as "pink and neon green paintball", which is exactly what those bytes
decode to. Fixed by reading `thing->unk28..unk2B` by name.

### Why this took so long to find

**Every previous investigation looked at the impact DECAL, and the decal was innocent.**
Section 4 proves it cannot draw those colours: no surface can select the one impact row with
random colours. All the effort went into `g_ImpactTypes`, the paintball cheat, RGBA16, RGBA32
and CI palettes -- and every one of those negatives was correct. The spark is a **different
object drawn at the same instant**, and nobody had separated the two.

**The user's own reproduction is what cracked it**: "pointed the PP7 at the ground and
fired". Sparks land on every surface; the decal rows differ by surface. That distinction is
what pointed at an object the harness had never been measuring.

**The lesson worth generalising: a byte offset into a struct is only valid for the ABI it
was written against.** This port has already been bitten by `long` width, by bitfield order
and now by pointer width moving every field behind it. Grep for the SHAPE --
`((u8 *) x)[0xNN]` -- not for this instance.

Historical note: paintball genuinely was a GameShark function on hardware, which is why
the cheat exists in the codebase at all. That made the symptom read as a cheat firing on its
own, and it never was: `cheatIsActive(CHEAT_PAINTBALL)` measures 0 throughout.

---

## 7. The tree-wide sweep for the same pattern

Prompted by section 6: a byte offset into a struct is only valid for the ABI it was written
against, so the whole decomp was swept for the shape rather than for that one instance.
477 source files, four shapes.

### Fixed

| site | was | why it was wrong here |
|---|---|---|
| `glass2.c:648` | `((u8 *) thing)[0x28..0x2b]` | pointer width grew the struct 0x2c→0x38; 0x28 became a **float**. The paintball flash. |
| `propobj.c:2284` | `((u8 *) obj)[3] == 1` | byte 3 is `type` in FILE order, but `GE_SUBWORD3` reorders to `type state extrascale` on little-endian, so byte 3 is the **high byte of extrascale** |
| `vtxstore.c:309,314` | `((s16 *) &<s32 global>)[1]` | halfword 1 is the LOW half on MIPS and the **HIGH** half here, which for a count of 0x50 or 0x14 is always **0** |
| `chrprop.c:1344` | `((u8 *) &g_Textures[n])[0] & 0xf` | bitfield order: yields `hitTexture` on MIPS, `hitSound` here (section 5) |

### Checked and cleared, with the reason

- **`options.c` ×12** -- `(u8 *) game_control_styles + n * 20 + k`. Looks exactly like the bad
  shape and is fine: `game_control_styles` is a plain `u16[]`, so a 20-byte stride is 10
  entries on every platform. **Struct-free arithmetic does not have this bug.**
- **`gunfire.c:1293-1316`** -- `((f32 *) nodepos)[0..2]` reads a `coord3d` as three floats.
  Layout-independent.
- **`gunfire.c:6939, 7012, 7126`** -- `((u8 *) imageoffset)[4]` indexes ROM asset bytes, not a
  host struct. The asset layout is fixed by the file format.
- **`propobj.c:9152`** -- `((f32 *) rodata)[n]` indexes a float table.
- **`model.c:251`** -- genuinely wrong (`*(model + 0x14)` on a `Model *`) but inside
  `#ifdef DEBUG`, which this port never defines. Dead, left alone, noted here so the next
  sweep does not re-flag it.
- **`unk_092E50.c`, `propobj.c:8565`** -- already carry written-up notes about this exact class
  from earlier work.

### How to run it again

```bash
grep -nE '\(\((u8|s8|u16|s16|u32|s32|f32|char) ?\*\)[^)]*\) ?\[ ?(0x[0-9a-fA-F]+|[1-9][0-9]*) ?\]' <files>
grep -nE '\* ?\( ?(u8|s8|u16|s16|u32|s32|f32) ?\* ?\) ?\( ?\( ?(u8|char) ?\* ?\)' <files>
```

**Triage on one question: does the offset land inside a struct whose layout the host ABI
can change?** Asset bytes, `u16[]` tables and float triples are all fine. Structs with
pointers, bitfields, or `long` are not.

Smoke-tested after the changes on stages 9, 25, 33, 37 and 41: all boot, render and exit
cleanly (`drawn` = 1, 2, 5, 5, 31).
