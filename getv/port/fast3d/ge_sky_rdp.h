/* GoldenEye port - decoder for sky.c's hand-assembled RDP triangle commands.
 *
 * GoldenEye's sky is not drawn with F3D vertex/triangle commands. sky.c hand-packs raw
 * RDP triangle commands and passes them through the display list as a run of
 * G_RDPHALF_1 / G_RDPHALF_CONT immediate commands, each carrying 32 bits in w1. The RSP
 * Microcode reassembles the halves and pushes them straight into the rdp fifo. An hle
 * renderer like Fast3D only walks the F3D switch, never sees a triangle, and draws no
 * sky at all. This is the same gap GoldenRecomp/RT64 documented and never closed
 * and it is why DAM has a black sky.
 *
 * All 88 G_RDPHALF sites in the tree are in sky.c.
 *
 * The opcodes are not literals in gbi.h. They are G_IMMFIRST-11/-12/-13 - negative ints
 * that only become 0xB4 / 0xB3 / 0xB2 after u8 truncation, so grepping gbi.h for "0xB4"
 * finds nothing.
 *
 * G_TEXRECT is not affected and must not be routed here: gfx_pc.c already consumes its
 * two trailing words with ++cmd, so those halves never reach the opcode switch. sky.c's
 * are standalone commands (w0 = 0xB4000000 - opcode in the top byte, rest zero).
 *
 * The packing is the stock RDP layout:
 *   word0 = [63:56]=cmd  [55]=dir  [53:51]=level  [50:48]=tile  [47:32]=YL
 *           [31:16]=YM   [15:0]=YH                       (YL/YM/YH are s11.2 screen y)
 *   word1 = XL(s15.16)   DxLDy(s15.16)
 *   word2 = XH           DxHDy
 *   word3 = XM           DxMDy
 * then, in this order, 8 shade words if cmd bit2, then 8 texture words if cmd bit1.
 *
 * A run is not terminated by G_RDPHALF_2. The microcode gives _2 no terminating
 * semantics: `imm_rdphalf_cont` falls straight through into `imm_rdphalf_2` and both
 * reach `dispatch_rdp_novirtaddr`, which emits one 64-bit word per RDPHALF_1+X pair.
 * The length comes from the command byte: 4 + 8*shade + 8*texture + 2*zbuf words.
 *
 * An RDP "triangle" command does not describe a triangle. It describes a span-fill
 * bounded by three independent edges, and GoldenEye uses it to fill a rectangle.
 *
 *   major edge   x = XH + DxHDy*(y-YH),   spans YH -> YL
 *   minor edge 1 x = XM + DxMDy*(y-YH),   spans YH -> YM
 *   minor edge 2 x = XL + DxLDy*(y-YM),   spans YM -> YL
 *
 * The RDP fills between the major edge and whichever minor edge is live at that
 * scanline. Only when XH == XM (the two edges start at the same point) and the minor
 * edges meet the major edge at YL is the result a triangle.
 *
 * On DAM, frame 1, the single command skyRenderFull() emits:
 *   YH=10.00  YM=YL=212.75   XH=319.75 DxHDy=0   XM=0 DxMDy=0   XL=0
 * The major edge is the vertical line x=319.75; minor edge 1 is the vertical line x=0;
 * minor edge 2 has zero height (YM==YL). Every scanline from y=10 to y=212.75 is filled
 * from x=0 to x=319.75 - a full-width rectangle, which matches the full gradient across
 * the upper frame that the retail screenshot shows.
 *
 * An earlier decoder reconstructed three vertices from those edges -
 *   (XH,YH), (XL,YM), (XH+DxHDy*(YL-YH),YL)
 * - which for this command is (319.75,10) (0,212.75) (319.75,212.75), exactly half the
 * rectangle. That, and not any missing second mechanism, is why DAM rendered one flat
 * triangle where retail has a full sky. The 65 Gfx commands the cloud path emits are 25
 * state-setup commands plus this one command's 40 halves, not a separate path.
 *
 * The fix is to reconstruct the real region as a polygon. Its corners are the endpoints
 * of the three edges:
 *   A = major  @ YH   B = minor1 @ YH   C = minor1 @ YM
 *   D = minor2 @ YM   E = minor2 @ YL   F = major  @ YL
 * walked A->B->C->D->E->F (down the minor side, back up the major side), then
 * fan-triangulated from A. Degenerate corners collapse automatically, so a genuine
 * triangle still decodes to exactly one triangle and nothing regresses.
 *
 * GETV_SKYQUAD=0 restores the single-triangle reconstruction for a same-binary A/B.
 * GETV_SKYSHADE=0 restores the flat placeholder blue.
 */
#ifndef GE_SKY_RDP_H
#define GE_SKY_RDP_H

#include <stdint.h>

/* A->B->C->D->E->F fanned from A is at most 4 triangles. */
#define GE_SKY_MAX_TRIS 4

/* Screen-space triangle recovered from an RDP edge-walk command.
 *
 * x/y are N64 320x240 screen pixels. r/g/b/a are 0..255 shade, evaluated per corner
 * from the RDP's own coefficients so Fast3D's linear interpolation reproduces the
 * RDP's linear gradient exactly. s/t are texel coordinates in S10.5 (i.e. texels*32),
 * which is the same unit gfx_pc.c's `LoadedVertex.u/v` already carries. */
struct GeSkyTri {
    float x[3], y[3];
    float r[3], g[3], b[3], a[3];
    float s[3], t[3];
    int   textured;         /* command bit1 -- carried 8 texture coefficient words */
    int   shaded;           /* command bit2 -- carried 8 shade coefficient words   */
    int   dir;              /* bit 55: which edge the RDP calls major. Winding only. */
    int   tile;
    unsigned cmd;           /* the raw RDP command byte, for diagnostics */
};

/* Feed one immediate command. `opcode` is the truncated u8 from w0>>24.
 * Returns the number of triangles written to `out` (0..GE_SKY_MAX_TRIS); `out` must
 * have room for GE_SKY_MAX_TRIS. Any non-RDPHALF opcode resets the accumulator. */
int geSkyRdpFeedTris(unsigned opcode, uint32_t w1, struct GeSkyTri *out);

/* Back-compatible single-triangle wrapper: returns 1 and fills *out with the first
 * triangle. Kept so an un-updated caller still links and still draws something. */
int geSkyRdpFeed(unsigned opcode, uint32_t w1, struct GeSkyTri *out);

/* Diagnostic counters. */
extern unsigned long geSkyRdpTris;      /* triangles successfully decoded */
extern unsigned long geSkyRdpCmds;      /* RDP triangle COMMANDS decoded */
extern unsigned long geSkyRdpHalves;    /* halves consumed */
extern unsigned long geSkyRdpAborted;   /* runs abandoned mid-way */

void geSkyRdpReset(void);

#endif
