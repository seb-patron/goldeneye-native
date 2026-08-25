/* GoldenEye native port - Fast3D's SDL backend registers keyboard callbacks.
 *
 * The Apple TV has no keyboard: input comes from an MFi/PS5/Xbox gamepad through
 * the GameController framework, which SDL surfaces as a joystick. These exist only
 * so gfx_sdl2.c compiles; they are wired to no-ops in port/src/port_support.c.
 * Real input belongs in the controller layer once the game boots. */
#ifndef GE_PORT_CONTROLLER_KEYBOARD_H
#define GE_PORT_CONTROLLER_KEYBOARD_H

#include <stdbool.h>

bool keyboard_on_key_down(int scancode);
bool keyboard_on_key_up(int scancode);
void keyboard_on_all_keys_up(void);

#endif
