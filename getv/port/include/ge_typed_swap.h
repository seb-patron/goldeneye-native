/* Typed byteswap with a fail-loud default.
 *
 * Adapted from perfect-dark-pc-port/perfect_dark @ 514bf7a,
 * port/include/preprocess/common.h (PD_SWAPPED_VAL/PD_SWAP_VAL). MIT, (c) 2022 Ryan Dwyer.
 * See docs/PERFECT_DARK.md section 2.2 and docs/LICENSING.md section 6.
 *
 * ---------------------------------------------------------------- why
 *
 * This port's existing byte-order conversions -- bgBE32 (bg.c), geAnimDescBE16 (model.c),
 * the GE_SUBWORD2/3/4 macros (bondtypes.h) -- are one-off, untyped readers: each is written
 * for one field at one call site, and nothing stops a future conversion site from reusing the
 * wrong one. Swapping a 4-byte value with a 2-byte helper compiles cleanly and corrupts data
 * silently; the only way to notice is a wrong pixel or a wrong angle much later, with no link
 * back to the swap that caused it.
 *
 * GE_SWAP(x) dispatches on the type of x rather than trusting the call site: pass a u32, get
 * swapU32; pass an f32, get swapF32. Passing anything this header does not know about still
 * compiles -- _Generic requires every arm to be a valid expression even though only one runs
 * -- but the default arm is ge_swap_unk, which asserts immediately rather than silently
 * reinterpreting the bytes at the wrong width. That trade -- a compile that always succeeds,
 * an assert that fires the first time the wrong arm actually runs -- is deliberate: it is the
 * same fail-loud-at-first-use shape as this port's other budget/overflow assertions
 * (port_assets.c's romCopy clamp, gfx_pc.c's shader-program-pool exhaustion), not a new
 * pattern invented for this file.
 *
 * ---------------------------------------------------------------- what this does not do
 *
 * This does not replace bgBE32/geAnimDescBE16/GE_SUBWORD* at their existing call sites. Those
 * are working code with no reported defect; PD's own doc is explicit that adopting the
 * technique does not require adopting the code, and rewriting working conversions carries risk
 * for no measured benefit. This header exists for the NEXT conversion site, so that site has a
 * typed option instead of another bespoke untyped reader.
 *
 * This also does not attempt PD's PD_CONV_ARRAY/PD_CONV_ARRAY2D/PD_CONV_PTR layer -- those
 * assume PD's preprocess/ pipeline (a single load-time transcode funnel, docs/PERFECT_DARK.md
 * section 2.1), which this port does not have and is largely forced not to have: assets here
 * are extracted to compiled-in C source, not loaded from a live ROM, so there is no equivalent
 * single hook point. GE_SWAP is useful standalone at any inline conversion site regardless. */
#ifndef GE_TYPED_SWAP_H
#define GE_TYPED_SWAP_H

#include <assert.h>
#include <ultra64.h>

static inline u32 ge_swap_u32(u32 x)
{
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8)  | ((x & 0xff000000u) >> 24);
}

static inline s32 ge_swap_s32(s32 x)
{
    u32 u = ge_swap_u32((u32) x);
    s32 r;
    __builtin_memcpy(&r, &u, sizeof r);
    return r;
}

static inline f32 ge_swap_f32(f32 x)
{
    u32 u;
    __builtin_memcpy(&u, &x, sizeof u);
    u = ge_swap_u32(u);
    __builtin_memcpy(&x, &u, sizeof x);
    return x;
}

static inline u16 ge_swap_u16(u16 x)
{
    return (u16) (((x & 0x00ffu) << 8) | ((x & 0xff00u) >> 8));
}

static inline s16 ge_swap_s16(s16 x)
{
    u16 u = ge_swap_u16((u16) x);
    s16 r;
    __builtin_memcpy(&r, &u, sizeof r);
    return r;
}

/* Any type this header does not name lands here. The message is deliberately unconditional
 * rather than describing which type was wrong: _Generic selects the arm at compile time, so by
 * the time this body runs the caller has already told us the value it wanted swapped was one
 * of the five above, and the assert firing at all is the fact worth knowing. */
static inline u32 ge_swap_unk(u32 x)
{
    assert(0 && "GE_SWAP: unhandled type -- add an arm to ge_typed_swap.h rather than widen an existing one");
    return x;
}

#define GE_SWAP(x) _Generic((x), \
    u32: ge_swap_u32, \
    s32: ge_swap_s32, \
    f32: ge_swap_f32, \
    u16: ge_swap_u16, \
    s16: ge_swap_s16, \
    default: ge_swap_unk \
)(x)

#endif /* GE_TYPED_SWAP_H */
