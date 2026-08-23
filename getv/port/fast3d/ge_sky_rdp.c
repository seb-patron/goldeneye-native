/* GoldenEye port - decoder for sky.c's hand-assembled RDP triangle commands.
 * See ge_sky_rdp.h for the format, the evidence it is the stock RDP layout, and why a
 * three-vertex reconstruction loses half the sky. Kept in its own translation unit so
 * gfx_pc.c needs only a few lines of hook. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_sky_rdp.h"

/* The F3D immediate opcodes, AFTER u8 truncation of G_IMMFIRST-11/-12/-13.
 * Written as literals with the derivation spelled out, because these values cannot be
 * found by grepping gbi.h -- they exist there only as negative expressions. */
#define GE_RDPHALF_1     0xB4u   /* G_IMMFIRST-11 */
#define GE_RDPHALF_2     0xB3u   /* G_IMMFIRST-12 */
#define GE_RDPHALF_CONT  0xB2u   /* G_IMMFIRST-13 */

/* 4 edge + 8 shade + 8 texture + 2 zbuf = 22 words = 44 halves, worst case. */
#define GE_SKY_MAX_HALVES 64

unsigned long geSkyRdpTris;
unsigned long geSkyRdpCmds;
unsigned long geSkyRdpHalves;
unsigned long geSkyRdpAborted;

static uint32_t ge_halves[GE_SKY_MAX_HALVES];
static int      ge_nhalves;

void geSkyRdpReset(void)
{
    ge_nhalves = 0;
}

/* GETV_SKYQUAD=0 -> the 3-vertex reconstruction (one triangle per command), so a single
 * binary can A/B the polygon fix. Same pattern as GETV_SKY / GETV_TEXSWAP / GETV_VTXSWAP. */
static int ge_sky_quad_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_SKYQUAD"); on = !(e && *e == '0'); }
    return on;
}

/* GETV_SKYTRACE=1 -> dump the decoded corners + attributes (shares the name sky.c uses). */
static int ge_sky_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_SKYTRACE"); on = (e && *e && *e != '0'); }
    return on;
}

/* GETV_SKYSHADE=0 -> flat placeholder blue instead of the decoded shade gradient. */
static int ge_sky_shade_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_SKYSHADE"); on = !(e && *e == '0'); }
    return on;
}

/* GETV_SKYPERSP=1 -> divide S and T by W. The default is 0: use S and T directly.
 *
 * The RDP divides the texture coefficients by W and the result is S10.5, but the decoded
 * W here is ~16383.5 (~2^14) while S is ~1143, so S/W lands at 0.07 -- a fraction of one
 * texel across the whole sky. That predicts an essentially flat colour, and the rendered
 * output with =1 matches that prediction. Retail shows soft cloud banding, so dividing by
 * W is the wrong default. Using S directly gives a 23..36 texel span, which is the
 * plausible mapping for a 32x32 cloud tile.
 *
 * The resulting cloud contrast is closer to retail than the =1 arm was, but is not proven
 * exact; a normalisation shift may still be missing. Sky colour itself is not at issue,
 * only cloud contrast. When comparing against the reference captures, note that those
 * PNGs are ColorMatch RGB rather than sRGB and must be converted first. */
static int ge_sky_persp_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_SKYPERSP"); on = (e && *e == '1'); }
    return on;
}

/* GETV_SKYTEX=0 -> zero texcoords (untextured), to isolate shade from texture. */
static int ge_sky_tex_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GETV_SKYTEX"); on = !(e && *e == '0'); }
    return on;
}

/* s11.2 screen coordinate -> pixels. The field is 16 bits and signed: sky triangles
 * legitimately start above the top of the screen, so a naive unsigned read puts the
 * top vertex at y=+16000 and the triangle vanishes. */
static float ge_s11_2(uint32_t v)
{
    int32_t s = (int32_t)(v & 0xFFFFu);
    if (s & 0x8000) { s -= 0x10000; }
    return (float)s / 4.0f;
}

/* s15.16 -> float. sub_GAME_7F094298() (sky.c:215) is the exact inverse. */
static float ge_s15_16(uint32_t v)
{
    return (float)(int32_t)v / 65536.0f;
}

/* Shade and texture coefficient packing.
 *
 * Every attribute is one s16.16 value split across two words: the signed integer half
 * sits in one word and the unsigned fractional half in the word 4 later. That is why the
 * coefficient block is 8 words rather than 4 -- ints first, then fracs.
 *
 *   shade   w4 = R.i G.i B.i A.i        w6  = R.f G.f B.f A.f
 *           w5 = DRDX.i ... DADX.i      w7  = DRDX.f ...
 *           w8 = DRDE.i ...             w10 = DRDE.f ...
 *           w9 = DRDY.i ...             w11 = DRDY.f ...
 *
 * This is checked against the emitter, not only against RDP documentation. sky.c's
 * skyRenderFull() builds exactly this: `sp160 = arg1->r * 65536.0f` is R (the colour at
 * arg1), `sp150 = (r_at_arg2 - arg1->r) / dx_pixels` is DRDX, and
 * `sp140 = (arg3->r - arg1->r) / dy_pixels` is DRDE. The emission order is
 * (int,int) (int,int) (frac,frac) (frac,frac) for the first four words and the same
 * again for the last four. */
static float ge_attr(const uint32_t *h, int int_half, int frac_half, int hi)
{
    uint32_t iw = h[int_half], fw = h[frac_half];
    int32_t  ip = hi ? (int32_t)(iw >> 16) : (int32_t)(iw & 0xFFFFu);
    uint32_t fp = hi ? (fw >> 16) : (fw & 0xFFFFu);
    if (ip & 0x8000) { ip -= 0x10000; }
    return (float)ip + (float)fp / 65536.0f;
}

/* One reconstructed polygon corner, with every attribute already evaluated there. */
struct GeSkyPt { float x, y, r, g, b, a, s, t; };

/* The attribute reference point is (XH, YH), the top of the major edge.
 *
 * The RDP span walker starts each scanline at the major edge holding
 * `C + DcDe*(y-YH)`, then adds DcDx once per pixel as it crosses the span. So the
 * value anywhere is
 *      c(x,y) = C + DcDe*(y - YH) + DcDx*(x - x_major(y))
 * and sky.c confirms the reference point: it stores R as `arg1->r`, YH as `arg1->unk2c`
 * and XH as arg1's x in both winding branches of skyRenderFull().
 *
 * Evaluating this per corner and letting Fast3D interpolate linearly between corners
 * reproduces the RDP's gradient exactly -- the function is affine in (x,y), and linear
 * interpolation of an affine function is that function. */
struct GeSkyShade {
    float c[4], dcdx[4], dcde[4];   /* r,g,b,a */
    float s, t, w, dsdx, dtdx, dwdx, dsde, dtde, dwde;
    int   have_shade, have_tex, persp;
    float xh, yh, dxhdy;
};

static void ge_eval(struct GeSkyPt *p, const struct GeSkyShade *sh, float x, float y)
{
    float dy = y - sh->yh;
    float dx = x - (sh->xh + sh->dxhdy * dy);

    p->x = x;
    p->y = y;

    if (sh->have_shade) {
        p->r = sh->c[0] + sh->dcde[0] * dy + sh->dcdx[0] * dx;
        p->g = sh->c[1] + sh->dcde[1] * dy + sh->dcdx[1] * dx;
        p->b = sh->c[2] + sh->dcde[2] * dy + sh->dcdx[2] * dx;
        p->a = sh->c[3] + sh->dcde[3] * dy + sh->dcdx[3] * dx;
    } else {
        p->r = p->g = p->b = p->a = 255.0f;
    }

    if (sh->have_tex) {
        float s = sh->s + sh->dsde * dy + sh->dsdx * dx;
        float t = sh->t + sh->dtde * dy + sh->dtdx * dx;
        float w = sh->w + sh->dwde * dy + sh->dwdx * dx;
        /* Perspective divide, as the RDP's texture unit does. Evaluating it at the
         * corner is exact there; the residual error is only the nonlinearity across
         * the interior, which Fast3D cannot express with w=1 backdrop vertices anyway.
         * Guard w: the sky quad's w is ~1 but a degenerate command could zero it. */
        if (sh->persp && (w > 1e-6f || w < -1e-6f)) { s /= w; t /= w; }
        p->s = s;
        p->t = t;
    } else {
        p->s = p->t = 0.0f;
    }
}

static void ge_put(struct GeSkyTri *out, int k, const struct GeSkyPt *p)
{
    out->x[k] = p->x; out->y[k] = p->y;
    out->r[k] = p->r; out->g[k] = p->g; out->b[k] = p->b; out->a[k] = p->a;
    out->s[k] = p->s; out->t[k] = p->t;
}

static float ge_area2(const struct GeSkyPt *a, const struct GeSkyPt *b,
                      const struct GeSkyPt *c)
{
    float v = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    return v < 0.0f ? -v : v;
}

/* Decode the accumulated command into 0..GE_SKY_MAX_TRIS triangles. */
static int ge_decode(struct GeSkyTri *out)
{
    uint32_t hi = ge_halves[0], lo = ge_halves[1];
    unsigned cmd = (hi >> 24) & 0xFFu;
    struct GeSkyShade sh;
    struct GeSkyPt pt[6];
    int npt = 0, ntri = 0, i, tbase;
    float yl, ym, yh, xl, dxldy, xh, dxhdy, xm, dxmdy;
    int textured = (cmd & 0x02u) != 0;
    int shaded   = (cmd & 0x04u) != 0;

    yl = ge_s11_2(hi);
    ym = ge_s11_2(lo >> 16);
    yh = ge_s11_2(lo);

    xl    = ge_s15_16(ge_halves[2]);  dxldy = ge_s15_16(ge_halves[3]);
    xh    = ge_s15_16(ge_halves[4]);  dxhdy = ge_s15_16(ge_halves[5]);
    xm    = ge_s15_16(ge_halves[6]);  dxmdy = ge_s15_16(ge_halves[7]);

    memset(&sh, 0, sizeof(sh));
    sh.xh = xh; sh.yh = yh; sh.dxhdy = dxhdy;
    sh.w = 1.0f;

    if (shaded && ge_sky_shade_on()) {
        sh.have_shade = 1;
        for (i = 0; i < 4; i++) {
            /* i = 0..3 -> r,g,b,a. Each word packs two attributes (hi/lo 16 bits), so
             * attribute i lives in word (i>>1) of the group, half (i&1)==0 -> hi. */
            int wi = i >> 1, hihalf = ((i & 1) == 0);
            sh.c[i]    = ge_attr(ge_halves,  8 + wi, 12 + wi, hihalf);
            sh.dcdx[i] = ge_attr(ge_halves, 10 + wi, 14 + wi, hihalf);
            sh.dcde[i] = ge_attr(ge_halves, 16 + wi, 20 + wi, hihalf);
        }
    }

    tbase = 8 + (shaded ? 16 : 0);
    if (textured && ge_sky_tex_on()) {
        sh.have_tex = 1;
        sh.persp = ge_sky_persp_on();
        /* The fractional half is at +4, not +8. Each coefficient group is four halves of
         * integer parts followed by the same four halves of fractional parts, and there
         * are two such groups (base/DcDx, then DcDe/DcDy). Reading the fraction 8 halves
         * later silently pairs S's integer with DSDX's fraction, and the resulting values
         * stay plausibly small, so this has to be cross-checked against the emitter rather
         * than eyeballed. sky.c emits
         *   h0 S.i T.i | h1 W.i | h2 DSDX.i DTDX.i | h3 DWDX.i
         *   h4 S.f T.f | h5 W.f | h6 DSDX.f DTDX.f | h7 DWDX.f
         *   h8 DSDE.i  | h9 DWDE.i | h10 DSDY.i | h11 DWDY.i | h12..15 their fracs
         * The texture group carries four values per word pair (sp254[0..3]), not three:
         * S, T, W and a fourth the sky never uses. */
        sh.s    = ge_attr(ge_halves, tbase + 0, tbase +  4, 1);
        sh.t    = ge_attr(ge_halves, tbase + 0, tbase +  4, 0);
        sh.w    = ge_attr(ge_halves, tbase + 1, tbase +  5, 1);
        sh.dsdx = ge_attr(ge_halves, tbase + 2, tbase +  6, 1);
        sh.dtdx = ge_attr(ge_halves, tbase + 2, tbase +  6, 0);
        sh.dwdx = ge_attr(ge_halves, tbase + 3, tbase +  7, 1);
        sh.dsde = ge_attr(ge_halves, tbase + 8, tbase + 12, 1);
        sh.dtde = ge_attr(ge_halves, tbase + 8, tbase + 12, 0);
        sh.dwde = ge_attr(ge_halves, tbase + 9, tbase + 13, 1);
    }

    if (!ge_sky_quad_on()) {
        /* The three-vertex reconstruction, kept verbatim so the A/B is honest. This is
         * the code that produced a single flat triangle on DAM. */
        struct GeSkyPt a, b, c;
        ge_eval(&a, &sh, xh,                       yh);
        ge_eval(&b, &sh, xl,                       ym);
        ge_eval(&c, &sh, xh + dxhdy * (yl - yh),   yl);
        ge_put(out, 0, &a); ge_put(out, 1, &b); ge_put(out, 2, &c);
        out->textured = textured; out->shaded = shaded;
        out->tile = (int)((hi >> 16) & 0x07u);
        out->dir  = (int)((hi >> 23) & 1u);
        out->cmd  = cmd;
        return 1;
    }

    /* The span region's corners; see the header. Walk the minor side down
     * (B,C,D,E) then the major side back up (F,A). */
    ge_eval(&pt[npt++], &sh, xh,                       yh);              /* A major@YH  */
    ge_eval(&pt[npt++], &sh, xm,                       yh);              /* B minor1@YH */
    ge_eval(&pt[npt++], &sh, xm + dxmdy * (ym - yh),   ym);              /* C minor1@YM */
    ge_eval(&pt[npt++], &sh, xl,                       ym);              /* D minor2@YM */
    ge_eval(&pt[npt++], &sh, xl + dxldy * (yl - ym),   yl);              /* E minor2@YL */
    ge_eval(&pt[npt++], &sh, xh + dxhdy * (yl - yh),   yl);              /* F major@YL  */

    /* Fan from A. Zero-area fans are the normal case, not an error: a genuine triangle
     * has A==B and E==F and collapses to exactly one output triangle, which is why this
     * cannot regress the shapes that already worked. */
    for (i = 1; i + 1 < npt && ntri < GE_SKY_MAX_TRIS; i++) {
        if (ge_area2(&pt[0], &pt[i], &pt[i + 1]) < 0.01f) {
            continue;
        }
        ge_put(&out[ntri], 0, &pt[0]);
        ge_put(&out[ntri], 1, &pt[i]);
        ge_put(&out[ntri], 2, &pt[i + 1]);
        out[ntri].textured = textured;
        out[ntri].shaded   = shaded;
        out[ntri].tile     = (int)((hi >> 16) & 0x07u);
        /* `dir` (bit 55) selects which edge the RDP treats as major. The microcode copies
         * the word through untouched, and rendering uses geometry_mode = 0 (no backface
         * culling), so it only ever affects winding. */
        out[ntri].dir      = (int)((hi >> 23) & 1u);
        out[ntri].cmd      = cmd;
        ntri++;
    }

    /* GETV_SKYTRACE=1: the decoded corners with their shade and texture attributes.
     * This is how the coefficient transcription is checked -- a wrong int/frac pairing
     * shows up here as a colour in the thousands, or as an s/t that does not vary across
     * the quad, long before it is visible as a wrong pixel. */
    if (ge_sky_trace_on()) {
        static int n = 0;
        if (n < 3) {
            n++;
            printf("[getv][skyrdp] cmd=0x%02X shade=%d tex=%d tile=%d -> %d tris\n",
                   cmd, shaded, textured, (int)((hi >> 16) & 0x07u), ntri);
            printf("[getv][skyrdp]   RAW s=%.4f t=%.4f w=%.4f  dsdx=%.6f dtdx=%.6f dwdx=%.6f  dsde=%.6f dtde=%.6f dwde=%.6f\n",
                   sh.s, sh.t, sh.w, sh.dsdx, sh.dtdx, sh.dwdx, sh.dsde, sh.dtde, sh.dwde);
            printf("[getv][skyrdp]   RAW C=(%.2f,%.2f,%.2f,%.2f) dcdx0=%.5f dcde0=%.5f\n",
                   sh.c[0], sh.c[1], sh.c[2], sh.c[3], sh.dcdx[0], sh.dcde[0]);
            for (i = 0; i < npt; i++) {
                printf("[getv][skyrdp]   pt%d (%7.2f,%7.2f) rgba=(%6.1f,%6.1f,%6.1f,%6.1f) st=(%8.2f,%8.2f)\n",
                       i, pt[i].x, pt[i].y, pt[i].r, pt[i].g, pt[i].b, pt[i].a,
                       pt[i].s, pt[i].t);
            }
            fflush(stdout);
        }
    }
    return ntri;
}

/* How many 64-bit RDP words does this triangle command carry?
 *
 * Derived from the microcode. vendor/pd-port/src/rsp/gsp.s is an annotated copy of the
 * same microcode GoldenEye runs:
 *     imm_rdphalf_1:     sw t8, sp_n04(sp)          -- stash one word, back to main_loop
 *     imm_rdphalf_cont:  li v0, 0                   -- falls through to:
 *     imm_rdphalf_2:     lw t9, sp_n04(sp)  ->  dispatch_rdp_novirtaddr
 *     dispatch_rdp_novirtaddr:  sw t9,0(s7); sw t8,4(s7); s7 += 8
 * So a RDPHALF_1 + (RDPHALF_CONT | RDPHALF_2) pair emits exactly one 64-bit RDP word,
 * and _2 has no terminating semantics. The length comes from the command byte:
 * bit0 = z-buffer, bit1 = texture, bit2 = shade.
 * 0xCE = shade+texture, no z -> 4+8+8 = 20 words = 40 halves, which is what DAM emits
 * per command. */
static int ge_words_for_cmd(unsigned cmd)
{
    int words = 4;
    if (cmd & 0x04u) { words += 8; }   /* shade   */
    if (cmd & 0x02u) { words += 8; }   /* texture */
    if (cmd & 0x01u) { words += 2; }   /* zbuffer */
    return words;
}

int geSkyRdpFeedTris(unsigned opcode, uint32_t w1, struct GeSkyTri *out)
{
    unsigned cmd;
    int needed, n;

    if (opcode != GE_RDPHALF_1 && opcode != GE_RDPHALF_CONT && opcode != GE_RDPHALF_2) {
        /* Any other opcode ends the run. A partial run is dropped rather than guessed
         * at -- inventing geometry from half a command is worse than drawing nothing. */
        if (ge_nhalves > 0) { geSkyRdpAborted++; ge_nhalves = 0; }
        return 0;
    }

    if (ge_nhalves >= GE_SKY_MAX_HALVES) {
        geSkyRdpAborted++;
        ge_nhalves = 0;
        return 0;
    }
    ge_halves[ge_nhalves++] = w1;
    geSkyRdpHalves++;

    /* Need the header pair before the length is knowable. */
    if (ge_nhalves < 2) {
        return 0;
    }

    cmd = (ge_halves[0] >> 24) & 0xFFu;
    /* RDP triangle commands are 0xC8..0xCF. Anything else means this run is not a
     * triangle (or we latched mid-stream); drop it rather than fabricate. */
    if (cmd < 0xC8u || cmd > 0xCFu) {
        geSkyRdpAborted++;
        ge_nhalves = 0;
        return 0;
    }

    needed = ge_words_for_cmd(cmd) * 2;
    if (ge_nhalves < needed) {
        return 0;
    }

    /* Complete. Reset so back-to-back commands inside one run each decode. */
    ge_nhalves = 0;
    n = ge_decode(out);
    if (n > 0) { geSkyRdpCmds++; geSkyRdpTris += (unsigned long)n; }
    else       { geSkyRdpAborted++; }
    return n;
}

int geSkyRdpFeed(unsigned opcode, uint32_t w1, struct GeSkyTri *out)
{
    struct GeSkyTri tmp[GE_SKY_MAX_TRIS];
    int n = geSkyRdpFeedTris(opcode, w1, tmp);
    if (n > 0) { *out = tmp[0]; return 1; }
    return 0;
}
