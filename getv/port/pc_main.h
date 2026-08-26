/* GoldenEye tvOS port - Fast3D's window backend calls back into the game to shut
 * down (e.g. when the OS asks the app to quit). sm64ex declares these in
 * src/pc/pc_main.h; GoldenEye has no equivalent, so the port owns them.
 * Implemented in port/src/port_support.c. */
#ifndef GE_PORT_PC_MAIN_H
#define GE_PORT_PC_MAIN_H

void game_deinit(void);
void game_exit(void);

/* gfx_sdl2.c calls this but does not include platform.h -- in sm64ex it arrives
 * transitively. Declared here so the dependency is explicit. */
void sys_sleep(double sec);

#endif
