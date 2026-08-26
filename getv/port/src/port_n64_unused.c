/* Definitions for four N64 symbols this port never reaches.
 *
 * Why this file exists, rather than the linker sorting it out
 * ------------------------------------------------------------
 * build_mac.sh passes -dead_strip, and that flag is necessary rather than an
 * optimisation: ld64 dead-strips before it checks for undefined symbols, so a reference
 * from an unreachable function is not an error. Four references rely on that today.
 * Linking the archive without -dead_strip produces exactly:
 *
 *     __efontchardataSegmentRomStart   from langGetJpnCharPixels  (src/game/language.o)
 *     __jfontchardataSegmentRomStart   from langGetJpnCharPixels  (src/game/language.o)
 *     _osPiReadIo                      from tokenReadIo           (src/token.o)
 *     _osViSetMode                     from viVsyncRelated        (src/fr.o)
 *
 * Every one is reachable only from a function this port never calls: the Japanese font
 * path, a PI register read, and a VI mode set. On macOS that is invisible. It will not
 * stay invisible: neither lld-link /OPT:REF nor GNU ld --gc-sections promises to
 * diagnose unresolved externals only after garbage collection, so a Windows or Linux
 * link is entitled to fail on all four. Defining them here removes the dependency on
 * linker ordering everywhere, macOS included.
 *
 * The two font symbols are DATA. Emitting a data symbol as a function is the tempting
 * shortcut and it is wrong: any read of it returns code bytes, which surfaces as a
 * SIGBUS that reads like a port bug rather than a missing definition.
 *
 * These are not stubs to be filled in later. If the Japanese font, PI reads or VI mode
 * setting are ever wired up, the real definitions replace these and this file shrinks.
 */

#include <PR/ultratypes.h>

/* Segment markers for the Japanese font data, referenced by langGetJpnCharPixels. The
 * port builds VERSION_US and never selects a Japanese language bank, so the segment is
 * absent and its start address is legitimately zero. */
u32 _efontchardataSegmentRomStart = 0;
u32 _jfontchardataSegmentRomStart = 0;

/* Peripheral Interface register read, referenced by tokenReadIo. There is no PI, and
 * nothing this port runs asks for one. */
void osPiReadIo(u32 devAddr, u32 *data)
{
    (void) devAddr;
    if (data != NULL) {
        *data = 0;
    }
}

/* Video Interface mode set, referenced by viVsyncRelated. Video mode here is SDL's
 * concern and is established long before this could be called. */
void osViSetMode(void *mode)
{
    (void) mode;
}
