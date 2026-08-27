// GoldenEye on tvOS - harness entry point.
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

/* glibc hides dladdr() (dlfcn.h) and the REG_* mcontext indices (sys/ucontext.h)
 * behind _GNU_SOURCE, so it has to be defined before the first system header.
 * Darwin exposes both unconditionally and does not need it. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/* The crash handler is the only genuinely per-OS part of this file. Windows has no
 * execinfo/ucontext/dlfcn and no sigaction: a fault arrives as a structured exception, not a
 * signal, so it gets its own registration and its own walk further down rather than a shim.
 * Everything else here is portable. */
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <process.h>   /* _exit */
#else
#include <execinfo.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <unistd.h>   /* _exit */
#endif
#include <signal.h>
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
#if !defined(_WIN32)
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
    /* The shape of the machine context is per-OS and per-architecture, so the PC and
     * the register file are pulled out in an #ifdef ladder and everything after it --
     * symbolisation, formatting -- is shared. Four hosts have a branch here: Darwin on
     * arm64 and x86_64, glibc on aarch64 and x86_64. Anything else still gets the fault
     * address, the boot mark and the backtrace; it just loses the PC and the registers,
     * which is the right outcome, because a guessed PC is worse than no PC. */
    if (uctx) {
        ucontext_t *uc = (ucontext_t *) uctx;
        void *pc = NULL;
        Dl_info di;

        (void) uc;
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
        pc = (void *)(uintptr_t) __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
        /* Intel Mac: no pointer authentication, so the fields are read straight out of
         * the thread state. The get_pc/get_lr/get_sp helper family exists only on arm64,
         * where it strips the PAC bits. */
        pc = (void *)(uintptr_t) uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
        /* glibc/aarch64: mcontext_t is `struct sigcontext` by value, with a flat
         * regs[31] plus named sp/pc, and no PAC bits to strip. */
        pc = (void *)(uintptr_t) uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
        pc = (void *)(uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
#endif

        if (pc == NULL) {
            printf("[getv] FAULT PC: unavailable (no register-context branch for this "
                   "OS/architecture)\n");
        } else if (dladdr(pc, &di) && di.dli_sname) {
            printf("[getv] FAULT PC: %p = %s + %ld   (image %s)\n", pc, di.dli_sname,
                   (long)((uintptr_t) pc - (uintptr_t) di.dli_saddr),
                   di.dli_fname ? di.dli_fname : "?");
        } else {
            printf("[getv] FAULT PC: %p (no symbol)\n", pc);
        }

#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
        {
            _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
            int i;

            printf("[getv] regs:");
            for (i = 0; i <= 28; i++) {
                printf("%s x%-2d=0x%016llx", (i % 4 == 0) ? "\n[getv]   " : "  ",
                       i, (unsigned long long) ss->__x[i]);
            }
            printf("\n[getv]    lr=0x%016llx  sp=0x%016llx\n",
                   (unsigned long long) __darwin_arm_thread_state64_get_lr(*ss),
                   (unsigned long long) __darwin_arm_thread_state64_get_sp(*ss));
        }
#elif defined(__APPLE__) && defined(__x86_64__)
        {
            _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;

            printf("[getv] regs:\n"
                   "[getv]   rax=0x%016llx  rbx=0x%016llx  rcx=0x%016llx  rdx=0x%016llx\n"
                   "[getv]   rdi=0x%016llx  rsi=0x%016llx  rbp=0x%016llx  rsp=0x%016llx\n"
                   "[getv]   r8= 0x%016llx  r9= 0x%016llx  r10=0x%016llx  r11=0x%016llx\n"
                   "[getv]   r12=0x%016llx  r13=0x%016llx  r14=0x%016llx  r15=0x%016llx\n",
                   (unsigned long long) ss->__rax, (unsigned long long) ss->__rbx,
                   (unsigned long long) ss->__rcx, (unsigned long long) ss->__rdx,
                   (unsigned long long) ss->__rdi, (unsigned long long) ss->__rsi,
                   (unsigned long long) ss->__rbp, (unsigned long long) ss->__rsp,
                   (unsigned long long) ss->__r8,  (unsigned long long) ss->__r9,
                   (unsigned long long) ss->__r10, (unsigned long long) ss->__r11,
                   (unsigned long long) ss->__r12, (unsigned long long) ss->__r13,
                   (unsigned long long) ss->__r14, (unsigned long long) ss->__r15);
            printf("[getv]    rip=0x%016llx  rsp=0x%016llx\n",
                   (unsigned long long) ss->__rip, (unsigned long long) ss->__rsp);
        }
#elif defined(__linux__) && defined(__aarch64__)
        {
            int i;

            /* The same x0-x28 window as the Darwin branch, deliberately: the two dumps
             * have to be diffable when one fault is reproduced on both hosts. x29/x30
             * come out below as fp/lr rather than as numbered registers. */
            printf("[getv] regs:");
            for (i = 0; i <= 28; i++) {
                printf("%s x%-2d=0x%016llx", (i % 4 == 0) ? "\n[getv]   " : "  ",
                       i, (unsigned long long) uc->uc_mcontext.regs[i]);
            }
            printf("\n[getv]    lr=0x%016llx  sp=0x%016llx\n",
                   (unsigned long long) uc->uc_mcontext.regs[30],
                   (unsigned long long) uc->uc_mcontext.sp);
        }
#elif defined(__linux__) && defined(__x86_64__)
        {
            /* No flat register array to loop over here, so the names are spelled out,
             * System V argument order first (rdi, rsi, rdx, rcx, r8, r9), because that
             * is the order a wrong argument is read in. There is no link register: the
             * return address is on the stack, and backtrace() below is what recovers it. */
            static const struct { const char *name; int idx; } gp[] = {
                { "rdi", REG_RDI }, { "rsi", REG_RSI }, { "rdx", REG_RDX },
                { "rcx", REG_RCX }, { "r8 ", REG_R8  }, { "r9 ", REG_R9  },
                { "rax", REG_RAX }, { "rbx", REG_RBX }, { "rbp", REG_RBP },
                { "r10", REG_R10 }, { "r11", REG_R11 }, { "r12", REG_R12 },
                { "r13", REG_R13 }, { "r14", REG_R14 }, { "r15", REG_R15 },
            };
            int i;

            printf("[getv] regs:");
            for (i = 0; i < (int)(sizeof(gp) / sizeof(gp[0])); i++) {
                printf("%s %s=0x%016llx", (i % 4 == 0) ? "\n[getv]   " : "  ",
                       gp[i].name,
                       (unsigned long long) uc->uc_mcontext.gregs[gp[i].idx]);
            }
            printf("\n[getv]    rsp=0x%016llx  eflags=0x%016llx\n",
                   (unsigned long long) uc->uc_mcontext.gregs[REG_RSP],
                   (unsigned long long) uc->uc_mcontext.gregs[REG_EFL]);
        }
#else
        printf("[getv] regs: no register-dump branch for this OS/architecture\n");
#endif
    }

    // What the renderer was executing, if anything. See gfx_pc.c's trace ring.
    { extern void gfx_dump_trace(void); gfx_dump_trace(); }
    fflush(stdout);
    backtrace_symbols_fd(frames, n, 1 /* stdout */);
    fflush(stdout);
    _exit(128 + sig);
}
#else  /* _WIN32 */

/* The Windows equivalent. A fault here is a structured exception rather than a signal, so
 * the entry point is an unhandled-exception filter and the register file arrives in a
 * CONTEXT rather than a ucontext_t.
 *
 * dbghelp does the symbolisation that dladdr does elsewhere. SymInitialize is called at
 * fault time rather than at startup on purpose: it loads and parses symbol files, which is
 * work this process should not do on every launch to serve a case that normally never
 * happens. The risk of initialising inside a fault is accepted for the same reason the
 * POSIX side calls printf there -- a best-effort report beats a silent exit, and this
 * handler's whole job is the report.
 *
 * A build without debug info still gets the module and the offset, which is enough to feed
 * back into a disassembler; SYMOPT_UNDNAME plus the .pdb gets the function name. MinGW emits
 * DWARF rather than PDB by default, so expect module+offset in a stock build. That is stated
 * plainly rather than promising names this toolchain will not produce. */
static LONG WINAPI ge_win_exception_filter(EXCEPTION_POINTERS *ep)
{
    static const int MAXF = 64;
    void *frames[64];
    USHORT n;
    HANDLE proc = GetCurrentProcess();
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;

    printf("\n[getv] ===== EXCEPTION 0x%08lx =====\n", (unsigned long) code);

    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2 &&
        (code == EXCEPTION_ACCESS_VIOLATION)) {
        ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        printf("[getv] fault addr: %p%s\n", (void *) addr,
               addr < 0x10000 ? "   <-- NULL-BASE OFFSET" : "");
    }
    { extern const char *gePortLastBootMark(void); (void) 0; }

#if defined(_M_X64) || defined(__x86_64__)
    if (ep && ep->ContextRecord) {
        CONTEXT *c = ep->ContextRecord;
        printf("[getv] FAULT PC: %p\n", (void *) (uintptr_t) c->Rip);
        printf("[getv] regs:\n"
               "[getv]   rcx=0x%016llx  rdx=0x%016llx  r8 =0x%016llx  r9 =0x%016llx\n"
               "[getv]   rax=0x%016llx  rbx=0x%016llx  rbp=0x%016llx  rsp=0x%016llx\n"
               "[getv]   rsi=0x%016llx  rdi=0x%016llx  r10=0x%016llx  r11=0x%016llx\n"
               "[getv]   r12=0x%016llx  r13=0x%016llx  r14=0x%016llx  r15=0x%016llx\n",
               (unsigned long long) c->Rcx, (unsigned long long) c->Rdx,
               (unsigned long long) c->R8,  (unsigned long long) c->R9,
               (unsigned long long) c->Rax, (unsigned long long) c->Rbx,
               (unsigned long long) c->Rbp, (unsigned long long) c->Rsp,
               (unsigned long long) c->Rsi, (unsigned long long) c->Rdi,
               (unsigned long long) c->R10, (unsigned long long) c->R11,
               (unsigned long long) c->R12, (unsigned long long) c->R13,
               (unsigned long long) c->R14, (unsigned long long) c->R15);
    }
#endif

    /* Same renderer trace the POSIX path dumps: what Fast3D was executing is usually more
     * informative than the stack on this project. */
    { extern void gfx_dump_trace(void); gfx_dump_trace(); }
    fflush(stdout);

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    n = CaptureStackBackTrace(0, (DWORD) MAXF, frames, NULL);
    {
        USHORT i;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *) buf;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;

        for (i = 0; i < n; i++) {
            DWORD64 disp = 0;
            if (SymFromAddr(proc, (DWORD64) (uintptr_t) frames[i], &disp, sym)) {
                printf("%-3u %s + %llu   (%p)\n", (unsigned) i, sym->Name,
                       (unsigned long long) disp, frames[i]);
            } else {
                printf("%-3u %p (no symbol)\n", (unsigned) i, frames[i]);
            }
        }
    }
    fflush(stdout);
    _exit(128 + 11);
    return EXCEPTION_EXECUTE_HANDLER;   /* not reached */
}
#endif /* _WIN32 */

int SDL_main(int argc, char *argv[])
{
#if defined(_WIN32)
    /* Windows delivers a fault as a structured exception, so there is nothing for sigaction
     * to catch. SIGABRT still exists and still comes through the CRT, but the interesting
     * faults -- access violations -- only reach the filter below. */
    SetUnhandledExceptionFilter(ge_win_exception_filter);
#else
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
#endif

    // devicectl --console is block-buffered, so without this the tail before any
    // crash is lost, which can make a SIGABRT look like a clean exit 0.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[getv] GoldenEye tvOS harness starting\n");

    /* Must run before any game code: it lays down the 0xFF poison the stub tables are
     * scanned against and the canaries that catch a stub overrunning its neighbour. */
    { extern void gePortStubInit(void); gePortStubInit(); }

    /* Mods load before any game code runs, so a mod's chunk body can configure things
     * that are only read at startup. Silent and free when there is no mods/ directory. */
    { extern void gePortLuaInit(void); gePortLuaInit(); }

    /* Hand the live-enemy accessors to ge_enemy_api. They live game-side because ChrRecord is a
     * game type the port layer cannot name (see objective_status.c), and they are REGISTERED
     * rather than linked so that ge_enemy_api stays free of game symbols and testable without
     * one. Installing here rather than per level is safe: the accessors read g_ChrSlots and
     * g_NumChrSlots each call, so between levels they simply report an empty world. */
    {
        extern int  gePortEnemyCount(void);
        extern int  gePortEnemyAt(int index, float *out, int count);
        extern void geEnemySourceInstall(int (*count)(void),
                                         int (*at)(int, float *, int));
        geEnemySourceInstall(gePortEnemyCount, gePortEnemyAt);
    }

    printf("[getv] libge.a linked; bossGetStageNum() -> %d\n", bossGetStageNum());

    /* macOS is the only target with a real window, so it is the only one with
     * anything to configure. GE_PLATFORM_DESKTOP is defined by the desktop build scripts --
     * the tvOS device and simulator builds do not define it and are unaffected. */
#ifdef GE_PLATFORM_DESKTOP
    { extern void gePortMacWindowConfig(void); gePortMacWindowConfig(); }
#endif

    // Fast3D brings up SDL, the GL ES/Metal context and the window itself.
#ifdef RAPI_METAL
    extern struct GfxRenderingAPI gfx_metal_api;
    gfx_init(&gfx_sdl, &gfx_metal_api, "GoldenEye 007");
#else
    gfx_init(&gfx_sdl, &gfx_opengl_api, "GoldenEye 007");
#endif
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
#if defined(_WIN32)
                /* Sleep() is milliseconds, so 16667us becomes 16ms. The rounding is
                 * acceptable here -- this is the idle path, not frame pacing -- and
                 * Windows' default timer granularity is coarser than that anyway. */
                Sleep(16);
#else
                usleep(16667);
#endif
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
