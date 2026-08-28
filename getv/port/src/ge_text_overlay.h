/* See ge_text_overlay.c for the design. Plain C linkage throughout: textrelated.c (vendor,
 * C) and gfx_pc.c (port, C) both call into this, and neither is C++. */
#ifndef GE_TEXT_OVERLAY_H
#define GE_TEXT_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* GETV_REAL_FONTS=1, and the font actually loaded -- checked, not assumed, so opting in can
 * never make text vanish, only switch which renderer draws it. Textrelated.c's own glyph-draw
 * gates call this to decide whether to suppress the baked bitmap quads. */
int gePortTextEnabled(void);

/* Queue one string for the overlay pass, in the same native-space (x, y) and RGBA colour
 * textRender()/textRenderOutlined() were themselves given. A no-op whenever
 * gePortTextEnabled() is false, so callers do not need their own guard.
 *
 * lineheight is the caller's resolved lineheight (chars['['].height + baseline when the
 * caller passed 0) -- the original bitmap font's own cell height for whichever bank drew
 * this string, in native pixels. The baked replacement glyphs have no relationship to that
 * size on their own; the overlay scales each string by lineheight / GE_TEXT_BAKE_PX so a HUD
 * string drawn with the small font bank prints small and a title drawn with the large one
 * prints large, instead of every call site coming out at one fixed size. */
void gePortTextQueue(const char *text, int x, int y,
                      unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                      int lineheight);

/* ---------------------------------------------------------------- for gfx_pc.c only
 *
 * The queue and the atlas, read back by the one file that already owns the native-to-screen
 * transform this needs. Not for use outside gfx_render_text_overlay(). */
int          gePortTextQueueCount(void);
void         gePortTextQueueGet(int i, const char **text, float *x, float *y,
                                 unsigned char *r, unsigned char *g, unsigned char *b,
                                 unsigned char *a, float *lineheight);
void         gePortTextQueueClear(void);
unsigned int gePortTextAtlasTexture(void);
int          gePortTextGlyphQuad(int ch, float *xpos, float *ypos, float *x0, float *y0,
                                  float *x1, float *y1, float *s0, float *t0, float *s1,
                                  float *t1);

#ifdef __cplusplus
}
#endif
#endif /* GE_TEXT_OVERLAY_H */
