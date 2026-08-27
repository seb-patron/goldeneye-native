/* GL error hunting. See ge_gl_debug.h for what this is chasing and why. */
#include <stdio.h>
#include <stdlib.h>

/* GL headers follow the same rule as gfx_opengl.c: GLEW where it is used to load entry points,
 * the platform's own headers otherwise. Including <GL/glew.h> unconditionally builds on Windows
 * and fails everywhere else, which is how this file arrived. */
#if defined(_WIN32) || defined(WIN32) || defined(OSX_BUILD)
# define GLEW_STATIC
# include <GL/glew.h>
#else
# define GL_GLEXT_PROTOTYPES 1
# if defined(__APPLE__) && defined(GE_PLATFORM_MAC)
#  include <OpenGL/gl3.h>
#  include <OpenGL/gl3ext.h>
# elif !defined(__APPLE__)
#  include <GL/gl.h>
#  include <GL/glext.h>
# endif
#endif

#include "ge_gl_debug.h"

/* No GL on this target (RAPI_METAL, or an Apple platform other than macOS -- iOS/tvOS have no
 * <OpenGL/gl3.h>): nothing above declared GLenum, so the real implementation below cannot compile
 * here. Same empty-stub pattern as ge_lua.c/ge_net_enet.c for a feature that is a no-op on this
 * target rather than a build error. */
#if !defined(RAPI_GL) || (defined(__APPLE__) && !defined(GE_PLATFORM_MAC))
int  geGlDebugEnabled(void) { return 0; }
void geGlDebugInstall(void) { }
int  geGlDebugPoll(const char *where, int frame) { (void) where; (void) frame; return 0; }
#else

/* The debug callback must carry the GL calling convention: the driver invokes it, so getting this
 * wrong corrupts the stack on any target where __stdcall and the C default differ.
 *
 * GLEW spells it GLAPIENTRY. APIENTRY is the Win32 name and arrives only if <windows.h> has been
 * pulled in, which glew.h does not guarantee here -- that is exactly how this file failed to
 * compile first time. Preferring GLEW's own macro keeps it correct without dragging in a header
 * for one token. The empty fallback is harmless on x86-64, where there is a single calling
 * convention and __stdcall is ignored. */
#if defined(GLAPIENTRY)
#define GE_GLCB GLAPIENTRY
#elif defined(APIENTRY)
#define GE_GLCB APIENTRY
#else
#define GE_GLCB
#endif

static int ge_gld_on = -1;
static int ge_gld_installed;
static int ge_gld_reported;     /* cap the callback's output; see below */

#define GE_GLD_MAX_REPORTS 40

int geGlDebugEnabled(void)
{
    if (ge_gld_on < 0) {
        const char *e = getenv("GETV_GLDEBUG");
        ge_gld_on = (e != NULL && *e == '1');
    }
    return ge_gld_on;
}

/* KHR_debug is GL 4.3. The macOS headers stop at 4.1 and the GL 2.0/3.0 contexts this port asks
 * SDL for do not declare these names at all, so the callback half is compiled only where it can
 * exist. Polling is glGetError, which is GL 1.0 and works everywhere -- and per the header it is
 * the half that answers the question that decides where to look: once at init, or every frame? */
#ifdef GL_DEBUG_SOURCE_API
# define GE_GLD_HAVE_KHR 1
#else
# define GE_GLD_HAVE_KHR 0
#endif

#if GE_GLD_HAVE_KHR
static const char *src_name(GLenum s)
{
    switch (s) {
    case GL_DEBUG_SOURCE_API:             return "API";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   return "window-system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER: return "shader-compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:     return "third-party";
    case GL_DEBUG_SOURCE_APPLICATION:     return "application";
    default:                              return "other";
    }
}

static const char *type_name(GLenum t)
{
    switch (t) {
    case GL_DEBUG_TYPE_ERROR:               return "ERROR";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "deprecated";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  return "UNDEFINED-BEHAVIOUR";
    case GL_DEBUG_TYPE_PORTABILITY:         return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE:         return "performance";
    default:                                return "other";
    }
}

static void GE_GLCB ge_gld_cb(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei length, const GLchar *message, const void *user)
{
    (void) length; (void) user;

    /* NOTIFICATION severity is dropped. Intel's driver emits a steady stream of them ("buffer
     * object will use video memory" and similar), and at one line per draw they would bury the one
     * message this exists to find. Errors, undefined behaviour and performance warnings are kept.
     * Filtering by severity rather than by message text, so a driver update cannot quietly change
     * what gets hidden. */
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) { return; }

    /* Capped, and the cap ANNOUNCES ITSELF. A per-draw error would otherwise emit tens of
     * thousands of lines -- each costing ~24 ms flushed on this machine -- and turn a diagnostic
     * run into an hour. Silently stopping would be worse: a reader would think it happened 40
     * times. */
    if (ge_gld_reported >= GE_GLD_MAX_REPORTS) {
        if (ge_gld_reported == GE_GLD_MAX_REPORTS) {
            printf("[getv][gldebug] ... more than %d messages; further ones suppressed. "
                   "This is a PER-CALL fault, not a one-off.\n", GE_GLD_MAX_REPORTS);
            fflush(stdout);
            ge_gld_reported++;
        }
        return;
    }
    ge_gld_reported++;

    printf("[getv][gldebug] %s/%s id=%u sev=0x%04x: %s\n",
           src_name(source), type_name(type), (unsigned) id, (unsigned) severity,
           message ? message : "(no message)");
    fflush(stdout);
}

#endif /* GE_GLD_HAVE_KHR */

void geGlDebugInstall(void)
{
    if (!geGlDebugEnabled() || ge_gld_installed) { return; }
    ge_gld_installed = 1;

#if !GE_GLD_HAVE_KHR
    printf("[getv][gldebug] built without KHR_debug -- per-frame polling only, "
           "which says WHEN but not WHICH call\n");
    fflush(stdout);
    return;
#else
    if (!GLEW_KHR_debug) {
        printf("[getv][gldebug] driver has no KHR_debug -- falling back to per-frame polling, "
               "which says WHEN but not WHICH call\n");
        fflush(stdout);
        return;
    }

    glEnable(GL_DEBUG_OUTPUT);
    /* SYNCHRONOUS matters. Without it the driver may batch messages and deliver them later, on
     * another thread, with the call that caused them long gone -- which turns "who did this" back
     * into guesswork. It costs performance, which is fine: this is a diagnostic mode, and the
     * measurement runs are made with it off. */
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(ge_gld_cb, NULL);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
    printf("[getv][gldebug] KHR_debug installed (synchronous)\n");
    fflush(stdout);
#endif
}

int geGlDebugPoll(const char *where, int frame)
{
    GLenum e;
    int n = 0;

    if (!geGlDebugEnabled()) { return 0; }

    /* Loop: the queue holds more than one and glGetError returns them one at a time. Bounded, so
     * a driver stuck returning an error forever cannot hang the frame. */
    while ((e = glGetError()) != GL_NO_ERROR && n < 16) {
        const char *name =
            (e == GL_INVALID_ENUM)                  ? "GL_INVALID_ENUM" :
            (e == GL_INVALID_VALUE)                 ? "GL_INVALID_VALUE" :
            (e == GL_INVALID_OPERATION)             ? "GL_INVALID_OPERATION" :
            (e == GL_INVALID_FRAMEBUFFER_OPERATION) ? "GL_INVALID_FRAMEBUFFER_OPERATION" :
            (e == GL_OUT_OF_MEMORY)                 ? "GL_OUT_OF_MEMORY" : "unknown";
        printf("[getv][gldebug] frame %d, at %s: %s (0x%04x)\n", frame, where, name, (unsigned) e);
        fflush(stdout);
        n++;
    }
    return n;
}
#endif /* !RAPI_GL || (Apple && !GE_PLATFORM_MAC) */
