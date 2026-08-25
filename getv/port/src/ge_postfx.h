/* Post-process parameters, owned by the port layer rather than by the renderer.
 *
 * The renderer half of this lives in getv/port/fast3d/gfx_opengl.c, which is fetched from
 * sm64ex and carried as a patch (see getv/patches/thirdparty/). Keeping the tunable state
 * out of that file means the plugin surface is ordinary tracked source that both other
 * platforms build unchanged, and the patched file only ever asks "what are the values this
 * frame?".
 *
 * Read every frame, not latched at startup. That is what lets a Lua mod turn the CRT on, or
 * change its numbers, at any point in the run -- which is the whole reason mods/crt_screen
 * exists as a mod instead of as a launcher checkbox.
 */
#ifndef GE_POSTFX_H
#define GE_POSTFX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GePostfx {
    int   crt;        /* master switch for the four CRT terms below */
    float scanline;   /* 0..1, depth of the scanline darkening        */
    float mask;       /* 0..1, aperture-grille strength               */
    float curve;      /* 0..0.1, barrel distortion                    */
    float vignette;   /* 0..1                                         */
    float lines;      /* virtual scanline count; 240 is the console's */
    int   fxaa;       /* edge antialiasing over the finished frame    */
} GePostfx;

/* Seed from GETV_CRT / GETV_CRT_* / GETV_FXAA. Called once by the renderer at init; safe to
 * call again. */
void gePostfxInit(void);

/* Current values. Never NULL. */
const GePostfx *gePostfxGet(void);

/* Replace all of them. Values are clamped, so a mod cannot ask for a scanline depth of 40
 * and get a black screen. */
void gePostfxSet(const GePostfx *p);

/* Is any effect asking for the offscreen target this frame? Supersampling needs it too, but
 * that is the renderer's own question and is not answered here. */
int gePostfxWanted(void);

#ifdef __cplusplus
}
#endif
#endif /* GE_POSTFX_H */
