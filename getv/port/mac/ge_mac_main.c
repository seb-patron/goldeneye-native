/* GoldenEye macOS port - the real main().
 *
 * On tvOS, entry comes from libSDL2main.a: its main() calls UIApplicationMain, installs
 * SDLUIKitDelegate, and only then calls SDL_main(). getv/Sources/ge_tvos_main.c defines
 * SDL_main() and is shared verbatim with this build.
 *
 * On macOS there is no UIApplicationMain to get in the way, so all that is needed is a
 * plain main() that forwards. This lives in its own translation unit on purpose: <SDL.h>
 * #defines main to SDL_main, so a file that includes it cannot also define the real
 * main(). Declaring SDL_main by hand and never including SDL.h avoids that.
 *
 * SDL's own SDL2main on macOS is now a no-op shim, but linking it would still rename main
 * and produce a duplicate-symbol link error, so it is not linked.
 */
extern int SDL_main(int argc, char *argv[]);

/* The config layer. Declared by hand for the same reason SDL_main is: this file must not
 * include a header that could pull in <SDL.h>. See getv/port/src/ge_config.c. It is
 * expressed entirely as setenv() on the existing GETV_* gates, with overwrite=0 for file
 * values and 1 for CLI values, so the environment still wins over the file and no existing
 * getenv() call site changes behaviour. It must run before SDL_main(), because gfx_init(),
 * osGetCount() and front.c all read their gates on first use. */
extern int geConfigInit(int argc, char **argv);

/* The launcher. Two different UIs share this one call site, chosen at compile time --
 * this file is NOT macOS-exclusive despite its directory (see the SDL_SetMainReady block
 * below: the same translation unit also supplies Windows's real main()), and Swift/SwiftUI
 * only exists in the macOS build, so the choice has to be a preprocessor gate, not a
 * runtime one.
 *
 * GE_HAS_NATIVE_LAUNCHER (defined only by project-mac.yml's GCC_PREPROCESSOR_DEFINITIONS --
 * deliberately NOT the same macro as GE_PLATFORM_MAC, which build_mac.sh's own plain-binary
 * build also defines despite having no Swift toolchain at all; see that yml's own comment
 * on the distinction) selects GeNativeLauncher.swift's macOS branch (see that file's own
 * header comment, and its GeLauncherBridgeRunner class specifically) -- retired here in
 * favour of the same SwiftUI launcher tvOS/iOS already use, since the settings surface
 * (Model, via GeLauncherBridge.h) is shared byte-for-byte across every platform's UI
 * regardless of which one is active. It runs unconditionally:
 * unlike the old gePortLauncherRun(argc, argv), which only showed a window behind
 * --launcher/GETV_LAUNCHER (opt-in, because a double-click launch is the common case and a
 * debug overlay should stay out of the way by default), this always presents the launcher
 * first -- matching tvOS/iOS, where GeNativeLauncherBridgeRunner has to run unconditionally
 * because there is no argv to gate on at all. This is a deliberate, visible behaviour
 * change on macOS specifically: a plain double-click now opens the mission/profile picker
 * instead of jumping straight to gameplay. GETV_LAUNCHER_AUTOPLAY=1 still skips the window
 * entirely for scripted/headless runs, exactly as it did before. Declared with `void` (no
 * argc/argv) to match its real signature -- Swift's @_cdecl export
 * (`public func gePortNativeLauncherRun() -> Int32`) takes none, since the old --launcher
 * argv scan has no equivalent here.
 *
 * Windows, Linux, and build_mac.sh's own plain-binary macOS build (GE_HAS_NATIVE_LAUNCHER
 * undefined in all three) keep gePortLauncherRun(argc, argv) -- ge_launcher.cpp's ImGui UI
 * -- entirely unchanged: no Swift toolchain exists to build the other branch in any of
 * them, and this pass's scope is the new Xcode-built .app (project-mac.yml) only.
 *
 * Both share the same contract: 0 to carry on into the game, non-zero if the user closed
 * the window without playing (GeLauncherBridgeRunner's NSWindowDelegate conformance is what
 * detects that outcome on the Swift side). Both fall through into the SAME process either
 * way on macOS -- the Swift path never execv()s at all (it touches no SDL/GL state -- see
 * GeNativeLauncherBridgeRunner's own header comment on tvOS/iOS for why that removes the
 * need relaunch() exists to work around), unlike gePortLauncherRun()'s own GE_PLATFORM_DESKTOP
 * relaunch() path, which Windows/Linux still take.
 *
 * Runs after geConfigInit so that every control opens showing the value the config layer
 * just resolved, rather than a second set of defaults that could disagree with the file. */
#ifdef GE_HAS_NATIVE_LAUNCHER
extern int gePortNativeLauncherRun(void);
#else
extern int gePortLauncherRun(int argc, char **argv);
#endif

#if defined(_WIN32)
/* SDL on Windows defines SDL_MAIN_NEEDED, which means SDL_Init() refuses to run unless
 * either SDL2main supplied the entry point or the application says it has done the setup
 * itself. This file deliberately provides the real main() and does not link SDL2main -- two
 * definitions of main would collide -- so SDL has to be told. Declared by hand for the same
 * reason SDL_main is: including <SDL.h> here would #define main to SDL_main and there would
 * be no real main() left.
 *
 * Without this the game exits immediately with "Application didn't initialize properly",
 * which reads like a missing DLL rather than a missing one-line call. */
extern void SDL_SetMainReady(void);
#endif

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    SDL_SetMainReady();
#endif
    int rc = geConfigInit(argc, argv);
    if (rc < 0) { return 0; }     /* clean stop: --help / --write-config / --list-cheats */
    if (rc > 0) { return rc; }    /* fatal config error */
#ifdef GE_HAS_NATIVE_LAUNCHER
    if (gePortNativeLauncherRun() != 0) { return 0; }
#else
    if (gePortLauncherRun(argc, argv) != 0) { return 0; }
#endif
    return SDL_main(argc, argv);
}
