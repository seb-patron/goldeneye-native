/* Give a window the launcher icon.
 *
 * Two windows want it -- the launcher's and the game's -- and both would otherwise show whatever
 * the platform hands an SDL application with no icon set, which on a Mac is a blank rocket and on
 * Linux is nothing at all. One function, called from both, so the two can never drift apart.
 *
 * The pixels are compiled in (see tools/make_icons.sh); nothing is read from disk, so a build
 * copied to another machine keeps its icon.
 */
#include <SDL2/SDL.h>

#include "ge_icon.h"

void gePortSetWindowIcon(SDL_Window *win)
{
    SDL_Surface *s;

    if (win == NULL) { return; }

    /* Masks given explicitly rather than by pixel format: SDL_CreateRGBSurfaceFrom's format
     * argument is byte-order sensitive, and the generator writes R,G,B,A in memory order on
     * every platform. Spelling the masks out means the icon does not come out blue on one of
     * them. */
    s = SDL_CreateRGBSurfaceFrom((void *) ge_icon_rgba, GE_ICON_SIZE, GE_ICON_SIZE, 32,
                                 GE_ICON_SIZE * 4,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
                                 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);
#else
                                 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
#endif
    if (s == NULL) { return; }

    SDL_SetWindowIcon(win, s);
    SDL_FreeSurface(s);   /* SDL copies it; holding on would just leak. */
}
