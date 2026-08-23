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

int main(int argc, char *argv[])
{
    int rc = geConfigInit(argc, argv);
    if (rc < 0) { return 0; }     /* clean stop: --help / --write-config / --list-cheats */
    if (rc > 0) { return rc; }    /* fatal config error */
    return SDL_main(argc, argv);
}
