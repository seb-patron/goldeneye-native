// GoldenEye on tvOS — harness entry point.
//
// This is not the game's entry point. GoldenEye's decomp has no PC port layer: its
// real entry is src/init.c running against N64 hardware (RCP, PI/SI DMA, an OS
// scheduler), none of which exists here. Standing that up needs the asset converter
// (see docs/asset-converter-spec.md) and a boot path.
//
// What this file does is exercise the pipeline underneath the game: build libge.a for
// arm64 tvOS, link Fast3D, sign, install, launch, and put a frame on the TV. Getting
// that loop working before any game logic is what made the Perfect Dark and Mario 64
// ports quick.
//
// Entry comes from libSDL2main.a: its main() calls UIApplicationMain, installs
// SDLUIKitDelegate, then calls SDL_main below.

#include <execinfo.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>   /* _exit */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#include <SDL.h>

// Fast3D's headers use Gfx (the N64 display-list command), which comes from the
// decomp's own GBI. port/include exposes PR/ via a symlink, deliberately not the
// whole of the decomp's include/, whose math.h/string.h/stdlib.h/stddef.h shadow
// the system headers.
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "gfx_opengl.h"
#include "gfx_sdl.h"
#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

// From libge.a. A plain getter over a global: safe with no N64 hardware, allocator
// or scheduler running. Referencing it forces the archive to link and proves the
// game's own decompiled code really is in this binary.
extern int bossGetStageNum(void);

// The game's real entry chain, found via mainproc() -> bossEntry() (init.c:256,
// boss.c:263). bossEntry() is just these three inits plus while(1){bossMainloop();}.
// The loop is driven here instead, because the frame loop belongs to the harness.
extern void bossInitMainthreadData(void);
extern void rspAllocateBuffers(void);
extern void musicSeqPlayerInit(void);
extern void bossMainloop(void);

// Perfect Dark's own crash handler is #elif defined(PLATFORM_LINUX), so it is never
// installed on Apple platforms and a crash gives no backtrace at all. The same would
// apply here, hence this handler. backtrace() is enough for the harness; PD's full
// ucontext handler is not needed.
static void ge_crash_handler(int sig, siginfo_t *info, void *uctx)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    printf("\n[getv] ===== SIGNAL %d =====\n", sig);
    /* The faulting address separates the two failure shapes that dominate this port:
     * a small value is an offset from a NULL base (an allocation that returned NULL,
     * or a pointer truncated to zero); a plausible-looking heap address is a real
     * overrun. Without it every fault looks the same in the backtrace. */
    if (info) {
        printf("[getv] fault addr: %p%s\n", info->si_addr,
               ((uintptr_t)info->si_addr < 0x10000) ? "   <-- NULL-BASE OFFSET" : "");
    }
    // The last boot mark reached, including ones the rate limiter suppressed. Once the
    // frame loop is running the printed trace goes quiet, so without this the crash
    // appears to happen wherever the log happens to end.
    {
        extern const char *ge_last_mark;
        extern unsigned long ge_mark_seq;
        printf("[getv] last boot mark: %s  (mark #%lu)\n", ge_last_mark, ge_mark_seq);
    }
    /* Report the faulting instruction, not just the frame it happened in. backtrace()
     * alone names the return address of the innermost call, which points at the wrong
     * function whenever the fault is inside a small inlined callee: it gets reported as
     * `caller + <offset of the instruction after the bl>`. The PC from the ucontext is
     * exact, and `sym + off` maps straight onto `otool -tV` output for that offset.
     * Registers are dumped too: the difference between a NULL base, a pointer truncated
     * to 32 bits, and a real overrun is usually visible in them. */
    if (uctx) {
        ucontext_t *uc = (ucontext_t *) uctx;
        _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
        void *pc = (void *)(uintptr_t) __darwin_arm_thread_state64_get_pc(*ss);
        Dl_info di;
        int i;

        if (dladdr(pc, &di) && di.dli_sname) {
            printf("[getv] FAULT PC: %p = %s + %ld   (image %s)\n", pc, di.dli_sname,
                   (long)((uintptr_t) pc - (uintptr_t) di.dli_saddr),
                   di.dli_fname ? di.dli_fname : "?");
        } else {
            printf("[getv] FAULT PC: %p (no symbol)\n", pc);
        }
        printf("[getv] regs:");
        for (i = 0; i <= 28; i++) {
            printf("%s x%-2d=0x%016llx", (i % 4 == 0) ? "\n[getv]   " : "  ",
                   i, (unsigned long long) ss->__x[i]);
        }
        printf("\n[getv]    lr=0x%016llx  sp=0x%016llx\n",
               (unsigned long long) __darwin_arm_thread_state64_get_lr(*ss),
               (unsigned long long) __darwin_arm_thread_state64_get_sp(*ss));
    }

    // What the renderer was executing, if anything. See gfx_pc.c's trace ring.
    { extern void gfx_dump_trace(void); gfx_dump_trace(); }
    fflush(stdout);
    backtrace_symbols_fd(frames, n, 1 /* stdout */);
    fflush(stdout);
    _exit(128 + sig);
}

int SDL_main(int argc, char *argv[])
{
    {   /* sigaction, not signal(): SA_SIGINFO is what carries si_addr. */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = ge_crash_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }

    // devicectl --console is block-buffered, so without this the tail before any
    // crash is lost, which can make a SIGABRT look like a clean exit 0.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[getv] GoldenEye tvOS harness starting\n");

    /* Must run before any game code: it lays down the 0xFF poison the stub tables are
     * scanned against and the canaries that catch a stub overrunning its neighbour. */
    { extern void gePortStubInit(void); gePortStubInit(); }
    printf("[getv] libge.a linked; bossGetStageNum() -> %d\n", bossGetStageNum());

    /* macOS is the only target with a real window, so it is the only one with
     * anything to configure. GE_PLATFORM_MAC is defined by build_mac.sh alone --
     * the tvOS device and simulator builds do not define it and are unaffected. */
#ifdef GE_PLATFORM_MAC
    { extern void gePortMacWindowConfig(void); gePortMacWindowConfig(); }
#endif

    // Fast3D brings up SDL, the GL ES context and the window itself.
    gfx_init(&gfx_sdl, &gfx_opengl_api, "GoldenEye 007");
    printf("[getv] Fast3D up: %dx%d internal, %dx%d output, supersample %u\n",
           gfx_current_dimensions.width, gfx_current_dimensions.height,
           gfx_output_dimensions.width, gfx_output_dimensions.height,
           gfx_supersample);

    // A minimal but valid display list: one G_ENDDL. Passing NULL to gfx_run()
    // segfaults -- gfx_run_dl() walks the list unconditionally.
    //
    // A display list that actually paints, so a black screen means "broken" rather
    // than "nothing was drawn". Commands are hand-built: gsDPFillRectangle() and
    // friends need _SHIFTL from PR/mbi.h, which drags in platform_info.h and the
    // N64 build plumbing.
    //
    // The (uint8_t) cast on every opcode is essential. GoldenEye is F3D, not F3DEX2,
    // and its G_* opcodes expand to negative ints. Casting straight to uintptr_t
    // sign-extends, so `w0 >> 24` yields 0xFFFFxx00 instead of the opcode byte,
    // nothing matches, and gfx_run_dl walks off the end into a SIGSEGV.
    #define GE_CMD(op) ((uintptr_t)(uint8_t)(op) << 24)

    // gfx_dp_fill_rectangle() bails out when the colour image address equals the
    // Z-buffer address -- both are 0 at startup, so without G_SETCIMG the fill is
    // silently skipped and the screen stays black with no error.
    static unsigned char fake_color_image[8];

    // 319<<2 / 239<<2: fill-rect coordinates are 10.2 fixed point, and Fast3D maps
    // the N64's 320x240 space onto the real drawable.
    static const Gfx dl_fill[] = {
        { .words = { GE_CMD(G_SETCIMG), (uintptr_t)fake_color_image } },
        // RGBA5551 packed into both halves: r=31,g=0,b=0,a=1 -> solid red.
        { .words = { GE_CMD(G_SETFILLCOLOR), 0xF801F801u } },
        { .words = { GE_CMD(G_FILLRECT) | (1276u << 12) | 956u, 0 } },
        { .words = { GE_CMD(G_ENDDL), 0 } },
    };

    // Boot experiment. This is expected to stop somewhere: bossInitMainthreadData()
    // calls tlbmanageEstablishManagementTable() and romCreateMesgQueue(), which are
    // N64 hardware, and nothing has loaded any assets. The point is to learn where
    // the boot path first hits something the port has not replaced yet; the backtrace
    // handler above turns that into an actionable answer.
    // Fast3D is up, so display lists arriving from the game can now be executed.
    { extern void gePortRenderReady(void); gePortRenderReady(); }

    /* Input self-test, GETV_INPUT_PROBE=<seconds>.
     *
     * Input cannot otherwise be verified until rendering works, and that is structural
     * rather than a scheduling accident: joyPoll() runs from gePortRenderDisplayList()
     * (this port's retrace handler), so every read of the pad sits downstream of
     * lvlRender. While lvlRender crashes, osContGetReadData is never called at all and
     * GETV_INPUT_DEBUG prints nothing, which looks identical to broken input.
     *
     * So poll the pad directly here, where SDL is up and no game code has run yet. This
     * answers the two questions that do not need the game: does SDL enumerate a
     * controller on this machine, and do the axes and buttons decode. Off unless the env
     * var is set, and it runs before the game boots, so it cannot perturb anything.
     *
     * tvOS delivers pads asynchronously, so a probe of 0 seconds proves nothing. Allow a
     * few seconds and press buttons during it. */
    {
        const char *probe = getenv("GETV_INPUT_PROBE");
        if (probe != NULL && atoi(probe) > 0) {
            extern void osContGetReadData(void *pad);
            /* OSContPad is 6 bytes; 4 of them plus slack. Declared as raw storage to
             * avoid dragging <PR/os.h> into this TU next to the system headers. */
            unsigned char padbuf[64];
            int secs = atoi(probe);
            int i, iters = secs * 60;

            printf("[getv] input probe: %d seconds, polling at ~60Hz "
                   "(set GETV_INPUT_DEBUG=1 or 2 to see the decoded pad)\n", secs);
            for (i = 0; i < iters; i++) {
                osContGetReadData(padbuf);
                usleep(16667);
            }
            printf("[getv] input probe: done\n");
        }
    }

    printf("[getv] --- boot experiment: calling bossInitMainthreadData() ---\n");
    bossInitMainthreadData();
    printf("[getv] survived bossInitMainthreadData()\n");

    printf("[getv] calling rspAllocateBuffers()\n");
    rspAllocateBuffers();
    printf("[getv] survived rspAllocateBuffers()\n");

    // Audio is real now. This call was skipped for as long as the audio ROM segments
    // were a zeroed port buffer, because musicSeqPlayerInit() parses the sfx and
    // instrument bank structures out of them and would otherwise walk garbage.
    //
    // It sets up the whole subsystem in one call: both banks (converted from the
    // ROM's 32-bit big-endian layout by gePortAudioBankNew), the three compact-MIDI
    // sequence players, Rare's custom reverb, snd.c's sfx player, and finally
    // amStartAudioThread() -- which in this port just arms gePortAudioFrame().
    printf("[getv] calling musicSeqPlayerInit()\n");
    musicSeqPlayerInit();
    { extern int gePortAudioIsLive(void);
      printf("[getv] survived musicSeqPlayerInit() -- audio backend %s\n",
             gePortAudioIsLive() ? "LIVE" : "did not come up"); }

    // init.c's schedulerInitThread() normally creates gfxFrameMsgQ. The harness skips
    // init.c entirely, so it must do that itself -- see port_os.c for why an
    // uninitialised queue is a SIGSEGV rather than a harmless no-op.
    { extern void gePortCreateHarnessQueues(void); gePortCreateHarnessQueues(); }

    printf("[getv] calling bossMainloop() ONCE\n");
    bossMainloop();
    printf("[getv] survived bossMainloop() -- the game booted\n");

    // Render real frames. The game cannot produce a display list until it boots, so
    // each frame is just Fast3D's clear and present. This confirms the GL ES path
    // works on the device.
    for (int frame = 0; frame < 600; frame++) {
        if (frame == 0) { printf("[getv] -> gfx_start_frame\n"); }
        gfx_start_frame();
        if (frame == 0) { printf("[getv] -> gfx_run\n"); }
        gfx_run((Gfx *)dl_fill);
        if (frame == 0) { printf("[getv] -> gfx_end_frame\n"); }
        gfx_end_frame();
        if (frame == 0) { printf("[getv] -> first frame complete\n"); }

        if (frame == 0 || frame % 120 == 0) {
            printf("[getv] frame %d: %dx%d internal -> %dx%d output\n", frame,
                   gfx_current_dimensions.width, gfx_current_dimensions.height,
                   gfx_output_dimensions.width, gfx_output_dimensions.height);
        }
    }

    printf("[getv] rendered 600 frames without error\n");
    gfx_shutdown();
    return 0;
}
