/* Dear ImGui overlay for the native build.
 *
 * Why this exists
 * ---------------
 * Everything this port can be told to do is an env gate or a goldeneye.cfg key, and the
 * only way to see what it did is stdout or a BMP written at a fixed frame. That is fine
 * for scripted measurement and useless for anything interactive: there is no way to watch
 * a counter move, flip a renderer flag and see the result, or pick a level without an
 * environment variable and a relaunch. This is the substrate for that -- a launcher and a
 * dev overlay are the intended users. It draws one small window today on purpose; the
 * value here is the wiring, not the widgets.
 *
 * Why the file is C++ and the header is not
 * -----------------------------------------
 * ImGui is C++ and the port layer is C. Rather than spread that through the tree, the
 * boundary is exactly this file: ge_imgui.h is plain C with void* parameters, gfx_sdl2.c
 * includes it and contains no #ifdef, and build_mac.sh's port loop stays a C loop with one
 * added C++ step. -lc++ was already on the link line (Fast3D needs it), so the link is
 * unchanged in kind.
 *
 * GL state, which is the only genuinely delicate part
 * ---------------------------------------------------
 * This uses imgui_impl_opengl2 -- the fixed-function backend -- because build_mac.sh
 * deliberately takes macOS's legacy 2.1 context (gfx_opengl.c emits `#version 120`
 * shaders, which a 3.2 core profile rejects), and imgui_impl_opengl3 unconditionally calls
 * glGenVertexArrays, a GL 3.0 entry point that context does not have. tools/fetch_imgui.sh
 * documents that choice at length.
 *
 * The GL2 backend saves and restores a lot of state but explicitly cannot save what the
 * legacy API has no getter for -- its own source says so and names the two:
 *
 *   1. the bound shader program. Fast3D leaves one bound (gfx_opengl.c:132). Fixed-function
 *      drawing with a program bound runs ImGui's vertices through GoldenEye's combiner
 *      shader, which expects attribute arrays this backend never sets.
 *   2. the bound array buffer. Fast3D keeps its VBO bound for the life of the process
 *      (gfx_opengl.c:849-851). glVertexPointer with a VBO bound treats its pointer as a
 *      byte OFFSET into that VBO, so ImGui's client-memory vertex pointers would be read
 *      as offsets in the tens of gigabytes.
 *
 * Both are saved, cleared and restored below. A third is added on top of the backend's
 * own list: Fast3D enables generic vertex attribute arrays and never disables them, and in
 * a compatibility profile generic attribute 0 aliases glVertex on some drivers, so any
 * still-enabled attribute array would fight the client arrays ImGui sets. They are
 * disabled and restored too.
 *
 * If the overlay ever renders but the game goes black afterwards, this restore block is
 * the first place to look.
 *
 * Proving it draws
 * ----------------
 * "The init printf appeared" proves initialisation, not rendering, and the existing
 * GETV_SHOTFRAME capture cannot help: it runs in gfx_opengl_end_frame(), which is before
 * swap_buffers_begin() and therefore before this draws. GETV_IMGUI_PROBE=<frame> reads the
 * colour buffer immediately before and immediately after the ImGui draw on exactly that
 * frame and reports how many pixels changed. A non-zero count is direct evidence that
 * these draw calls put pixels on the screen; zero means they did not, whatever the log
 * says. It costs two glReadPixels on one frame and is off unless asked for.
 */

#include "ge_imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GE_WITH_IMGUI)

#include <SDL2/SDL.h>

/* Same order and the same one-line preamble gfx_opengl.c uses. Without
 * GL_GLEXT_PROTOTYPES, SDL_opengl.h pulls in macOS's GL 1.1 header and declares everything
 * past it as function-pointer typedefs only, and the state save/restore below fails to
 * compile on glUseProgram -- which is a confusing way to be told "you forgot the define". */
#define GL_GLEXT_PROTOTYPES 1

/* Desktop GL. USE_GLES is defined only by the tvOS targets, which do not build this file
 * today (their Xcode projects compile getv/port/**.c); the branch is here so that adding
 * it there later is a build-script change and not a source change. */
#if defined(USE_GLES)
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

namespace {

bool g_active;              /* built in, enabled, and init succeeded */
bool g_frame_open;          /* ImGui::NewFrame() called and not yet Render()ed */
unsigned long g_frames;
int  g_probe_frame = -2;    /* -2 = env not read yet, -1 = off */

/* Number of generic vertex attribute arrays saved across the draw. Fast3D's shader
 * generator uses at most a handful (position, colour, up to two texture coordinate sets);
 * 8 is the minimum GL_MAX_VERTEX_ATTRIBS any implementation may report, so this is a
 * bound that cannot under-cover on a conforming driver without also breaking Fast3D. */
const int GE_IMGUI_ATTRIBS = 8;

bool ge_imgui_env_on(void)
{
    const char *e = getenv("GETV_IMGUI");
    return e != NULL && *e != '\0' && strcmp(e, "0") != 0;
}

/* The probe rectangle: the top-left corner of the framebuffer, which is where the overlay
 * window is pinned below. GL's origin is bottom-left, hence the height subtraction. */
void ge_imgui_probe_rect(int *x, int *y, int *w, int *h)
{
    int fw = 0, fh = 0;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (win) SDL_GL_GetDrawableSize(win, &fw, &fh);
    if (fw <= 0 || fh <= 0) { *x = *y = *w = *h = 0; return; }
    int rw = fw < 900 ? fw : 900;
    int rh = fh < 400 ? fh : 400;
    *x = 0; *y = fh - rh; *w = rw; *h = rh;
}

} /* namespace */

extern "C" void gePortImguiInit(void *window, void *glctx)
{
    if (g_active) return;
    if (!ge_imgui_env_on()) return;
    if (window == NULL) {
        printf("[getv][imgui] GETV_IMGUI set but there is no window -- overlay OFF\n");
        fflush(stdout);
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    /* No imgui.ini. The overlay has no persistent layout worth keeping yet, and writing a
     * dotfile into whatever directory the game happened to be launched from is a surprise
     * nobody asked for. A launcher that grows real layout state should set this to a path
     * under the port's own config directory (port_paths.c), not to the default. */
    ImGui::GetIO().IniFilename = NULL;

    if (!ImGui_ImplSDL2_InitForOpenGL((SDL_Window *)window, glctx)) {
        printf("[getv][imgui] SDL2 backend init FAILED -- overlay OFF\n");
        fflush(stdout);
        ImGui::DestroyContext();
        return;
    }
    if (!ImGui_ImplOpenGL2_Init()) {
        printf("[getv][imgui] GL2 backend init FAILED -- overlay OFF\n");
        fflush(stdout);
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    g_active = true;
    printf("[getv][imgui] overlay ON (GETV_IMGUI) -- Dear ImGui %s, backends: %s + %s\n",
           IMGUI_VERSION,
           ImGui::GetIO().BackendPlatformName ? ImGui::GetIO().BackendPlatformName : "?",
           ImGui::GetIO().BackendRendererName ? ImGui::GetIO().BackendRendererName : "?");
    printf("[getv][imgui] GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
    fflush(stdout);
}

extern "C" void gePortImguiNewFrame(void)
{
    if (!g_active || g_frame_open) return;

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    g_frame_open = true;
    g_frames++;

    /* The window itself. Pinned rather than free-floating so the probe below knows where
     * to look, and because a dev overlay that reopens in a different place every launch is
     * annoying. FirstUseEver, so it can still be dragged. */
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 110.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("GoldenEye -- dev overlay")) {
        const ImGuiIO &io = ImGui::GetIO();
        ImGui::Text("frame   %lu", g_frames);
        ImGui::Text("fps     %.1f  (%.2f ms/frame)",
                    (double)io.Framerate, 1000.0 / (double)(io.Framerate > 0.0f ? io.Framerate : 1.0f));
        ImGui::Text("display %.0f x %.0f", (double)io.DisplaySize.x, (double)io.DisplaySize.y);
    }
    ImGui::End();
}

extern "C" void gePortImguiRender(void)
{
    if (!g_active || !g_frame_open) return;
    g_frame_open = false;

    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (dd == NULL) return;

    if (g_probe_frame == -2) {
        const char *e = getenv("GETV_IMGUI_PROBE");
        g_probe_frame = (e && *e) ? atoi(e) : -1;
    }
    const bool probing = (g_probe_frame > 0 && (long)g_frames == (long)g_probe_frame);

    int px = 0, py = 0, pw = 0, ph = 0;
    unsigned char *before = NULL;
    if (probing) {
        ge_imgui_probe_rect(&px, &py, &pw, &ph);
        if (pw > 0 && ph > 0) {
            before = (unsigned char *)malloc((size_t)pw * ph * 3);
            if (before) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(px, py, pw, ph, GL_RGB, GL_UNSIGNED_BYTE, before);
            }
        }
    }

    /* ---- state the GL2 backend cannot save for itself (see the header comment) ------- */
    GLint  last_program = 0, last_array_buffer = 0, last_element_buffer = 0;
    GLint  last_active_texture = GL_TEXTURE0;
    GLint  attrib_enabled[GE_IMGUI_ATTRIBS];

    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
    for (int i = 0; i < GE_IMGUI_ATTRIBS; i++) {
        attrib_enabled[i] = 0;
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib_enabled[i]);
    }

    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < GE_IMGUI_ATTRIBS; i++) {
        if (attrib_enabled[i]) glDisableVertexAttribArray((GLuint)i);
    }

    ImGui_ImplOpenGL2_RenderDrawData(dd);

    for (int i = 0; i < GE_IMGUI_ATTRIBS; i++) {
        if (attrib_enabled[i]) glEnableVertexAttribArray((GLuint)i);
    }
    glActiveTexture((GLenum)last_active_texture);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_element_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)last_array_buffer);
    glUseProgram((GLuint)last_program);
    /* --------------------------------------------------------------------------------- */

    if (probing) {
        unsigned long changed = 0;
        if (before) {
            unsigned char *after = (unsigned char *)malloc((size_t)pw * ph * 3);
            if (after) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(px, py, pw, ph, GL_RGB, GL_UNSIGNED_BYTE, after);
                for (size_t i = 0; i < (size_t)pw * ph; i++) {
                    if (before[i * 3] != after[i * 3] ||
                        before[i * 3 + 1] != after[i * 3 + 1] ||
                        before[i * 3 + 2] != after[i * 3 + 2]) changed++;
                }
                free(after);
            }
            free(before);
        }
        printf("[getv][imgui] probe frame %lu: rect %dx%d at (%d,%d), "
               "%lu pixels changed by the overlay draw, %d draw list(s), %d vertices\n",
               g_frames, pw, ph, px, py, changed,
               dd->CmdListsCount, dd->TotalVtxCount);
        fflush(stdout);
    }
}

extern "C" void gePortImguiEvent(void *sdl_event)
{
    if (!g_active || sdl_event == NULL) return;
    ImGui_ImplSDL2_ProcessEvent((const SDL_Event *)sdl_event);
}

extern "C" void gePortImguiShutdown(void)
{
    if (!g_active) return;
    g_active = false;
    g_frame_open = false;
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    printf("[getv][imgui] overlay shut down after %lu frames\n", g_frames);
    fflush(stdout);
}

extern "C" int gePortImguiActive(void) { return g_active ? 1 : 0; }

#else /* !GE_WITH_IMGUI ------------------------------------------------------------------
 *
 * The absent case, and the one that must stay the default. Nothing here is #ifdef'd at the
 * call site, so a tree without tools/fetch_imgui.sh having been run builds and runs exactly
 * as it did before this file existed. The single printf is not noise: GETV_IMGUI=1 doing
 * nothing at all would look like a bug in the overlay rather than a binary built without
 * it, and that is a confusing hour nobody needs to spend.
 */

extern "C" void gePortImguiInit(void *window, void *glctx)
{
    (void)window; (void)glctx;
    const char *e = getenv("GETV_IMGUI");
    if (e != NULL && *e != '\0' && strcmp(e, "0") != 0) {
        printf("[getv][imgui] GETV_IMGUI is set but this binary was built without ImGui.\n");
        printf("[getv][imgui] run tools/fetch_imgui.sh, then ./getv/build_mac.sh all\n");
        fflush(stdout);
    }
}

extern "C" void gePortImguiNewFrame(void) {}
extern "C" void gePortImguiRender(void) {}
extern "C" void gePortImguiEvent(void *sdl_event) { (void)sdl_event; }
extern "C" void gePortImguiShutdown(void) {}
extern "C" int  gePortImguiActive(void) { return 0; }

#endif /* GE_WITH_IMGUI */
