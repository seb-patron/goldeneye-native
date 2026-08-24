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

/* The launcher, for --launcher or GETV_LAUNCHER=1. Returns 0 to carry on into the game and
 * non-zero if the user closed the window without playing. It normally does not return at
 * all: it sets the environment and execv()s this binary with --launcher removed, because
 * 76 of the GETV_ gates are read once into a static and cannot be changed after the game
 * has started. See getv/port/src/ge_launcher.cpp.
 *
 * It runs after geConfigInit so that every control opens showing the value the config layer
 * just resolved, rather than a second set of defaults that could disagree with the file. */
extern int gePortLauncherRun(int argc, char **argv);

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
    if (gePortLauncherRun(argc, argv) != 0) { return 0; }
    return SDL_main(argc, argv);
}
