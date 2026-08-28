/* Real-font text overlay.
 *
 * All on-screen text in the game funnels through exactly two functions -- textRender() and
 * textRenderOutlined() in textrelated.c -- which draw the baked N64 bitmap glyph banks
 * (ptrFontBankGothic, ptrFontZurichBold) as RDP texture rectangles. 110 call sites across
 * front.c, options.c, mpmenu.c and others call those two functions and never touch a glyph
 * directly, which is the same "one seam" shape ge_player_api.c already uses for input. That
 * means real fonts do not need 110 call sites changed: hide what the two draw functions emit,
 * queue the same (string, position, colour) they were given, and draw that queue once per
 * frame with a rasterized TrueType font instead.
 *
 * This file owns the queue and the font atlas -- pure data and asset loading, no coordinate
 * math. The actual draw happens in gfx_pc.c (gfx_render_text_overlay), which already has the
 * native-to-screen 2D transform gfx_draw_rectangle() uses (ge_scale_2d(), the pillarbox-
 * preserving one, not ge_scale(), the widescreen-widened one 3D content needs -- see that
 * function's own comment for why using the wrong one stretches every glyph to the window's
 * raw aspect). Duplicating that transform here risked exactly the kind of silent drift this
 * project has hit before; calling into the file that already has it does not.
 *
 * Off by default (GETV_REAL_FONTS=1 to enable): textRender/textRenderOutlined are the only
 * two functions all 110 call sites share, so flipping this on replaces every piece of in-game
 * text at once. That needs to be provably right, not iterated on with half the HUD illegible
 * in the meantime.
 *
 * Known limitation: decorative elements positioned independently of the text they sit next
 * to do not move with it, because this seam only ever sees (string, position, colour) -- it
 * has no way to reach code that was never a textRender() call in the first place. Confirmed
 * example: front.c's difficulty-select highlight box (constructor_menu08_difficulty(),
 * ~line 3917) is drawn via microcode_constructor_related_to_menus() with hardcoded pixel
 * coordinates (row * 0x1E + 0xB2) tuned to sit behind wherever the bitmap font happened to
 * put that row's label. The replacement font's line spacing does not match that tuning
 * exactly, so by GETV_MENU=8 the highlight has visibly drifted from its label. Fixing this
 * would mean finding and either decoupling or re-tuning every such hardcoded position across
 * the UI -- a separate, open-ended effort from getting the text itself rendering correctly,
 * and not attempted here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif

#ifdef __MINGW32__
# define GLEW_STATIC
# include <GL/glew.h>
#endif
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define GE_TEXT_QUEUE_CAP   1024
#define GE_TEXT_STRING_CAP  160
#define GE_TEXT_ATLAS_SIZE  512
#define GE_TEXT_BAKE_PX     24.0f
#define GE_TEXT_FIRST_CHAR  32      /* ' ' */
#define GE_TEXT_NUM_CHARS   95      /* through '~' (126), matching stbtt_BakeFontBitmap's own default range */

typedef struct GeTextItem {
    char text[GE_TEXT_STRING_CAP];
    float x, y;                        /* native-space: the same x/y textRender() itself takes */
    unsigned char r, g, b, a;
    float lineheight;                  /* the caller's resolved lineheight, native pixels */
} GeTextItem;

static GeTextItem    ge_text_queue[GE_TEXT_QUEUE_CAP];
static int            ge_text_count;

static stbtt_bakedchar ge_text_baked[GE_TEXT_NUM_CHARS];
static unsigned char   ge_text_atlas_bitmap[GE_TEXT_ATLAS_SIZE * GE_TEXT_ATLAS_SIZE];
static unsigned int    ge_text_gl_tex;
static int             ge_text_ready;      /* font loaded, atlas uploaded: safe to hide the old glyphs */
static int             ge_text_load_tried;

static int ge_text_enabled_cached = -1;

/* GetModuleFileNameA rather than argv[0]: the same reason ge_launcher.cpp's self_path() gives
 * -- argv[0] may be relative to a directory the process has since left. Kept as a local copy
 * rather than exposed from ge_launcher.cpp: that file is C++ and actively edited elsewhere;
 * this one is plain C and self-contained on purpose. */
static int ge_text_self_dir(char *out, size_t n)
{
#if defined(_WIN32)
    char exe[1024];
    DWORD r = GetModuleFileNameA(NULL, exe, (DWORD) sizeof exe);
    char *cut;
    if (r == 0 || r >= sizeof exe) return 0;
    snprintf(out, n, "%s", exe);
    cut = strrchr(out, '\\');
    if (!cut) cut = strrchr(out, '/');
    if (cut) *cut = '\0'; else out[0] = '\0';
    return out[0] != '\0';
#else
    (void) out; (void) n;
    return 0;
#endif
}

static void ge_text_load_font(void)
{
    static const char *kRel[] = {
        "assets/fonts/RobotoCondensed-VF.ttf",
        "../port/assets/fonts/RobotoCondensed-VF.ttf",
        "port/assets/fonts/RobotoCondensed-VF.ttf",
        "getv/port/assets/fonts/RobotoCondensed-VF.ttf",
    };
    char dir[1024];
    int havedir;
    size_t i;

    havedir = ge_text_self_dir(dir, sizeof dir);

    for (i = 0; i < sizeof kRel / sizeof kRel[0]; i++) {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            char path[1200];
            FILE *fp;
            long len;
            unsigned char *buf;
            int rc;

            if (pass == 0) {
                if (!havedir) continue;
                snprintf(path, sizeof path, "%s/%s", dir, kRel[i]);
            } else {
                snprintf(path, sizeof path, "%s", kRel[i]);
            }

            fp = fopen(path, "rb");
            if (!fp) continue;
            fseek(fp, 0, SEEK_END);
            len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (len <= 0) { fclose(fp); continue; }

            buf = (unsigned char *) malloc((size_t) len);
            if (!buf || fread(buf, 1, (size_t) len, fp) != (size_t) len) {
                fclose(fp);
                free(buf);
                continue;
            }
            fclose(fp);

            rc = stbtt_BakeFontBitmap(buf, 0, GE_TEXT_BAKE_PX, ge_text_atlas_bitmap,
                                       GE_TEXT_ATLAS_SIZE, GE_TEXT_ATLAS_SIZE,
                                       GE_TEXT_FIRST_CHAR, GE_TEXT_NUM_CHARS, ge_text_baked);
            free(buf);
            if (rc <= 0) {
                printf("[getv][text] stbtt_BakeFontBitmap failed on %s (rc=%d)\n", path, rc);
                fflush(stdout);
                continue;
            }

            glGenTextures(1, &ge_text_gl_tex);
            glBindTexture(GL_TEXTURE_2D, ge_text_gl_tex);
            /* GL_ALPHA-only textures sample as (0,0,0,a) in the fixed-function pipeline -- RGB
             * is always black, so the default GL_MODULATE texture env zeroes out the vertex
             * color and every glyph draws pure black regardless of the requested tint.
             * GL_LUMINANCE_ALPHA carries the same coverage value in both channels so MODULATE
             * tints correctly; stb_truetype only gives one coverage byte per pixel, so expand
             * it into an LA pair here (one-time cost, not per frame). */
            {
                size_t npx = (size_t) GE_TEXT_ATLAS_SIZE * GE_TEXT_ATLAS_SIZE;
                unsigned char *la = (unsigned char *) malloc(npx * 2);
                if (la) {
                    size_t px;
                    for (px = 0; px < npx; px++) {
                        la[px * 2]     = ge_text_atlas_bitmap[px];
                        la[px * 2 + 1] = ge_text_atlas_bitmap[px];
                    }
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, GE_TEXT_ATLAS_SIZE, GE_TEXT_ATLAS_SIZE, 0,
                                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la);
                    free(la);
                }
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            /* rc is stbtt_BakeFontBitmap's row count, not a per-glyph success count -- positive
             * means all GE_TEXT_NUM_CHARS fit in that many atlas rows, negative means the atlas
             * was too small and some glyphs were skipped. */
            printf("[getv][text] real-font overlay ready: %s, %d chars baked at %.0fpx (%d atlas rows)\n",
                   path, GE_TEXT_NUM_CHARS, GE_TEXT_BAKE_PX, rc);
            fflush(stdout);
            ge_text_ready = 1;
            return;
        }
    }
    printf("[getv][text] GETV_REAL_FONTS=1 but RobotoCondensed-VF.ttf was not found -- "
           "leaving the original bitmap font in place\n");
    fflush(stdout);
}

int gePortTextEnabled(void)
{
    if (ge_text_enabled_cached < 0) {
        const char *e = getenv("GETV_REAL_FONTS");
        ge_text_enabled_cached = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    if (ge_text_enabled_cached && !ge_text_load_tried) {
        ge_text_load_tried = 1;
        ge_text_load_font();
    }
    /* Only hides the old glyphs once the replacement is actually ready -- opting in must never
     * be able to make text disappear entirely, only switch which renderer draws it. */
    return ge_text_enabled_cached && ge_text_ready;
}

void gePortTextQueue(const char *text, int x, int y, unsigned char r, unsigned char g,
                      unsigned char b, unsigned char a, int lineheight)
{
    GeTextItem *it;
    if (!gePortTextEnabled()) return;
    if (text == NULL || *text == '\0') return;
    if (ge_text_count >= GE_TEXT_QUEUE_CAP) return;   /* dropped: a menu screen never gets close */

    it = &ge_text_queue[ge_text_count++];
    snprintf(it->text, sizeof it->text, "%s", text);
    it->x = (float) x;
    it->y = (float) y;
    it->r = r; it->g = g; it->b = b; it->a = a;
    it->lineheight = (lineheight > 0) ? (float) lineheight : GE_TEXT_BAKE_PX;
}

/* ---------------------------------------------------------------- accessors for gfx_pc.c
 *
 * gfx_pc.c owns the coordinate transform and the GL draw calls; this file just hands back
 * what it queued and baked. Kept this way rather than drawing here directly so the one
 * native-to-screen formula stays in the one file that already has it right. */
int gePortTextQueueCount(void) { return ge_text_count; }

void gePortTextQueueGet(int i, const char **text, float *x, float *y,
                         unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a,
                         float *lineheight)
{
    GeTextItem *it = &ge_text_queue[i];
    *text = it->text;
    *x = it->x; *y = it->y;
    *r = it->r; *g = it->g; *b = it->b; *a = it->a;
    *lineheight = it->lineheight;
}

void gePortTextQueueClear(void) { ge_text_count = 0; }

unsigned int gePortTextAtlasTexture(void) { return ge_text_gl_tex; }

/* 0 on a char outside the baked range (space/newline are handled by the caller before this,
 * but a stray control byte or the >=0x80 Japanese path should not read out of bounds). */
int gePortTextGlyphQuad(int ch, float *xpos, float *ypos, float *x0, float *y0, float *x1,
                         float *y1, float *s0, float *t0, float *s1, float *t1)
{
    stbtt_aligned_quad q;
    if (ch < GE_TEXT_FIRST_CHAR || ch >= GE_TEXT_FIRST_CHAR + GE_TEXT_NUM_CHARS) return 0;
    stbtt_GetBakedQuad(ge_text_baked, GE_TEXT_ATLAS_SIZE, GE_TEXT_ATLAS_SIZE,
                        ch - GE_TEXT_FIRST_CHAR, xpos, ypos, &q, 1 /* opengl fill rule */);
    *x0 = q.x0; *y0 = q.y0; *x1 = q.x1; *y1 = q.y1;
    *s0 = q.s0; *t0 = q.t0; *s1 = q.s1; *t1 = q.t1;
    return 1;
}
