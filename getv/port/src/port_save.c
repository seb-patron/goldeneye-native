/* GoldenEye port - the cartridge EEPROM, backed by a real file.
 *
 * What GoldenEye saves to, and why this is the right layer.
 *
 * GoldenEye does not use a Controller Pak. It saves to the cartridge's serial EEPROM,
 * through five libultra entry points that `src/joy.c:764-816` wraps:
 *
 * osEepromProbe / osEepromRead / osEepromWrite / osEepromLongRead / osEepromLongWrite
 *
 * and `src/game/file2.c` is the only consumer:
 *
 * fileWriteSmallSave  -> joyGamePakLongWrite(0, save, sizeof(smallSave))     32 B
 * fileWriteSave       -> joyGamePakLongWrite(slot*0x60/8 + 4, save, 0x60)    96 B
 * fileValidateSaves   -> joyGamePakLongRead(0, &joyChecksum, sizeof(smallSave))
 * joyGamePakLongRead(4, &saves, sizeof(save_data) * 5)
 *
 * The whole save is 512 bytes, which fits a 4 K EEPROM exactly.
 * smallSave  = 4 + 4 + 24            =  32 B = blocks 0..3
 * save_data  = 0x60 (verified below) =  96 B, five slots = 480 B = blocks 4..63
 *                                          --------
 *                                            512 B = 64 blocks of 8 = EEP4K
 * `save_data` contains no pointers (`src/game/file.h:7-21` is all s32/u8/u16 plus a
 * `u8 times[76]`), so it is 0x60 natively too; this is not bug family 1.3. The port layer
 * cannot include the game header to assert that, because PORTFLAGS has no decomp include
 * path, so the arithmetic is asserted against the constants instead, below.
 *
 * Not SDL_GetPrefPath: it was chosen for tvOS, where it lands in `Library/Caches`, which
 * the OS purges, so saves do not survive. macOS has a real filesystem and a real
 * convention, `~/Library/Application Support/Goldeneye-Native/`, which is what this uses. On
 * macOS SDL_GetPrefPath would in fact land in the same place, but going through it would
 * make the Mac path depend on tvOS's mistake.
 *
 * With the five entry points stubbed to 0, `fileGamePakProbe()` was false, so
 * `fileValidateSaves()` took the `GE_PORT_NATIVE` early-out at `file2.c:496-518` and every
 * slot came up blank on every launch. That is why the game boots with 19 of 20 levels
 * locked, and why `GETV_UNLOCKALL` had to exist as a diagnostic stand-in. A real EEPROM
 * makes progress persist.
 *
 * ENV GATES
 * GETV_SAVE=0 persistence OFF. Probe reports no EEPROM, so file2.c takes
 * exactly the pre-existing early-out. This is the A/B control.
 * GETV_SAVEDIR=<dir> override the directory (used by the round-trip test so it
 * never touches the user's real save).
 * GETV_SAVE_DEBUG=1 trace every block read/write and every flush.
 */
/* Deliberately not <PR/os.h>. That header redeclares bcopy/bcmp/bzero/sprintf with the
 * N64's `int`-length signatures, and macOS's _FORTIFY_SOURCE headers define the same names
 * as macros, so <string.h> and <PR/os.h> refuse to coexist in either order: string.h first
 * gives "expected parameter declarator" on bcopy, PR/os.h first gives "conflicting types * for bcmp". This file needs memcpy/memset/
 * strerror far more than it needs the N64 OS header.
 *
 * What it costs: `OSMesgQueue *` becomes `void *`. joy.c passes
 * `&g_ContInputMessageQueue` and we never dereference it -- there is no PIF and no SI
 * bus to arbitrate here -- so the parameter is genuinely unused. The pointer is
 * ABI-identical, the linker matches on name alone, and this is the whole divergence. */
#include <PR/ultratypes.h>

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* The real errno. getv/port/include/ge_win_compat.h undefines errno on Windows so that
 * PR/os.h's struct fields of that name can parse; MSVCRT exposes the value through
 * _errno(). Spelled once here rather than at each use. */
#if defined(_WIN32)
#define ge_errno (*_errno())
#else
#define ge_errno errno
#endif


#include <SDL.h>

/* The one place that knows about "$HOME/Library/Application Support" and about mkdir().
 * <sys/stat.h> used to be included here for mkdir() and is gone with it; see
 * getv/port/src/port_paths.c. The macOS path this produces is unchanged, character for
 * character. */
#include "port_paths.h"/* PR/os.h:571. Copied rather than included, for the reason above. */
#define EEPROM_TYPE_4K 0x01

/* 64 blocks x 8 bytes. */
#define GE_EEP_BLOCK   8
#define GE_EEP_BLOCKS  64
#define GE_EEP_BYTES   (GE_EEP_BLOCK * GE_EEP_BLOCKS)

/* 32 B smallSave + 5 x 0x60 save_data == 512 B == the whole 4 K part, to the byte.
 * If a future edit widens either record the game will start writing past block 63 and
 * this assert is the cheapest place to notice. */
_Static_assert(32 + 5 * 0x60 == GE_EEP_BYTES, "GE save layout no longer fills a 4K EEPROM");

/* Non-static on purpose, all three: these are the runtime discriminators that show a
 * port-layer rebuild landed and that the save path actually ran. `strings` is not evidence
 * here, because a static helper inlines away at -O1. */
int ge_eeprom_enabled = -1;   /* -1 = not yet resolved */
unsigned long ge_eeprom_flushes = 0;
unsigned long ge_eeprom_loaded_bytes = 0;
char ge_eeprom_path[1024] = { 0 };

static u8 ge_eep[GE_EEP_BYTES];
static int ge_eep_debug = 0;
static int ge_eep_dirty = 0;

/* Build the backing path and make sure its directory exists. Returns 0 on success. */
static int geSavePathInit(void)
{
 const char *dir = getenv("GETV_SAVEDIR");
 char base[900];

 if (dir != NULL && *dir != '\0') {
 snprintf(base, sizeof(base), "%s", dir);
    } else {
        /* One call, three hosts. On macOS this is the same
         * "$HOME/Library/Application Support/Goldeneye-Native" string the #ifdef used to
         * build inline; on tvOS it is still SDL's answer, which is the wrong answer
         * there -- see the header note -- but writing somewhere else on tvOS would be
         * worse than leaving the wrong path visible, so it is left as-is pending a
         * tvOS fix. On Windows and Linux SDL's answer is the right one. */
 if (gePortUserDataDir("Goldeneye-Native", "Goldeneye-Native", base, sizeof(base)) != 0) {
#ifdef GE_PLATFORM_MAC
            /* The only way the macOS branch fails is an unset or empty $HOME, so the
             * wording is exact. It stays behind the guard because it would be a lie
             * anywhere else -- this is a diagnostic, not a platform decision. */
 printf("[getv][save] no $HOME and no GETV_SAVEDIR -- persistence OFF\n");
#endif
 return -1;
        }
    }

    /* The project was renamed from Goldeneye-Native to Goldeneye-Native. An install that
     * predates the rename has its EEPROM under the old directory, and simply pointing at
     * the new one would present the player with an empty save slot and no explanation.
     * If the new directory has no save and the old one does, adopt the old path for this
     * run. Nothing is copied or deleted: the file keeps working where it is, and a user
     * who wants the new location can move it themselves. */
    {
        const char *home = getenv("HOME");
        char newfile[1024], oldbase[1024], oldfile[1024];
        struct stat st;
        snprintf(newfile, sizeof(newfile), "%s/eeprom.bin", base);
        if (home != NULL && *home != '\0' && stat(newfile, &st) != 0) {
            snprintf(oldbase, sizeof(oldbase),
                     /* Literal, and it must stay literal: this is the PRE-RENAME
                      * directory name. A tree-wide rename pass rewrote it once already,
                      * which silently disabled the migration it exists to perform. */
                     "%s/Library/Application Support/GoldenEyeTV", home);
            snprintf(oldfile, sizeof(oldfile), "%s/eeprom.bin", oldbase);
            if (stat(oldfile, &st) == 0) {
                printf("[getv][save] using the pre-rename save directory: %s\n", oldbase);
                snprintf(base, sizeof(base), "%s", oldbase);
            }
        }
    }

    /* mkdir the leaf only. "~/Library/Application Support" always exists on macOS,
     * and GETV_SAVEDIR is a test knob whose parent the caller owns. EEXIST is success.
     * Deliberately not gePortMakeDirTree(): a missing parent must stay a visible
     * failure, since creating it would paper over a wrong $HOME. */
 if (gePortMakeDir(base, 0755) != 0) {
 printf("[getv][save] cannot create %s: %s -- persistence OFF\n",
 base, strerror(ge_errno));
 return -1;
    }

 snprintf(ge_eeprom_path, sizeof(ge_eeprom_path), "%s/eeprom.bin", base);
 return 0;
}

static void geSaveLoad(void)
{
 FILE *f;
 size_t n;

    /* An absent file is a blank EEPROM, not an error. That is what a brand-new cartridge
     * is, and file2.c's CRC check then rewrites it correctly on first run. Reporting a
     * failure here would fall back to the no-EEPROM path permanently. */
 memset(ge_eep, 0, sizeof(ge_eep));

 f = fopen(ge_eeprom_path, "rb");
 if (f == NULL) {
 printf("[getv][save] no existing save at %s -- starting blank (%d bytes)\n",
 ge_eeprom_path, GE_EEP_BYTES);
 return;
    }
 n = fread(ge_eep, 1, sizeof(ge_eep), f);
 fclose(f);
 ge_eeprom_loaded_bytes = (unsigned long)n;
    /* A short read is not fatal either: the tail stays zero, and file2.c's per-slot CRC
     * rejects the damaged slots and calls fileResetSave() on exactly those. */
 printf("[getv][save] loaded %lu/%d bytes from %s\n",
 ge_eeprom_loaded_bytes, GE_EEP_BYTES, ge_eeprom_path);
}

/* Write the whole 512-byte image. Atomic via tmp+rename so a crash mid-save cannot
 * leave a torn file -- the game writes one 96-byte slot at a time and a torn image
 * would fail CRC and silently wipe a folder. 512 bytes is small enough that
 * rewriting all of it per block write is cheaper than tracking dirty ranges. */
static void geSaveFlush(void)
{
 char tmp[1100];
 FILE *f;

 if (!ge_eep_dirty || ge_eeprom_path[0] == '\0') { return; }

 snprintf(tmp, sizeof(tmp), "%s.tmp", ge_eeprom_path);
 f = fopen(tmp, "wb");
 if (f == NULL) {
 printf("[getv][save] cannot write %s: %s\n", tmp, strerror(ge_errno));
 return;
    }
 if (fwrite(ge_eep, 1, sizeof(ge_eep), f) != sizeof(ge_eep)) {
 printf("[getv][save] short write to %s\n", tmp);
 fclose(f);
 remove(tmp);
 return;
    }
 fflush(f);
 fclose(f);
 /* 🔴 THE SAVE HAS NEVER WORKED ON WINDOWS, AND IT FAILED SILENTLY.
  *
  * POSIX rename() atomically REPLACES the destination. Windows rename() REFUSES when the
  * destination exists, with EEXIST -- so every flush after the file first appeared failed.
  * Measured on a 900-frame Train run: 112 attempts, 112 failures, 0 successes, every one
  * reporting "File exists". No save data has ever persisted on this platform.
  *
  * It stayed invisible because two things compounded. ge_eep_dirty is cleared only on SUCCESS,
  * so a failed flush stays dirty and is retried on the next block write -- turning one broken
  * save into a permanent retry loop that reissued a write and a rename every few frames. And the
  * SUCCESS path prints only under ge_eep_debug while the FAILURE path always prints, so the log
  * filled with failures and never carried a baseline to compare them against.
  *
  * ⚠️ It was a real performance cost too, not just noise: 112 doomed write+rename pairs each
  * followed by an unconditional printf, on a box where a flushed stdout line costs about 24 ms.
  *
  * MoveFileExA with MOVEFILE_REPLACE_EXISTING is the Windows equivalent of POSIX rename -- it
  * replaces atomically, which is the property the tmp+rename dance exists for. Declared here
  * rather than including <windows.h>, because this file's errno is already a compat macro (see
  * ge_errno above) and pulling that header in beside it invites the collision the shim exists to
  * avoid. */
#if defined(_WIN32)
 {
  __declspec(dllimport) int __stdcall MoveFileExA(const char *, const char *, unsigned long);
  if (!MoveFileExA(tmp, ge_eeprom_path, 0x1u /* MOVEFILE_REPLACE_EXISTING */)) {
   printf("[getv][save] replace %s -> %s failed\n", tmp, ge_eeprom_path);
   remove(tmp);
   return;
  }
 }
#else
 if (rename(tmp, ge_eeprom_path) != 0) {
 printf("[getv][save] rename %s -> %s failed: %s\n",
 tmp, ge_eeprom_path, strerror(ge_errno));
 remove(tmp);
 return;
    }
#endif
 ge_eep_dirty = 0;
 ge_eeprom_flushes++;
 if (ge_eep_debug) {
 printf("[getv][save] flush #%lu -> %s\n", ge_eeprom_flushes, ge_eeprom_path);
 fflush(stdout);
    }
}

static void geSaveInit(void)
{
 const char *e;

 if (ge_eeprom_enabled != -1) { return; }

 e = getenv("GETV_SAVE_DEBUG");
 ge_eep_debug = (e != NULL && *e == '1');

 e = getenv("GETV_SAVE");
 if (e != NULL && *e == '0') {
 ge_eeprom_enabled = 0;
 printf("[getv][save] GETV_SAVE=0 -- EEPROM disabled, saves will NOT persist\n");
 fflush(stdout);
 return;
    }

 if (geSavePathInit() != 0) {
 ge_eeprom_enabled = 0;
 fflush(stdout);
 return;
    }
 geSaveLoad();
 ge_eeprom_enabled = 1;
 fflush(stdout);
}

/* ---- the five libultra entry points ------------------------------------- */

/* These replace the auto-generated zero-returning stubs that used to live in
 * ge_link_stubs.c. osEepromRead/osEepromWrite were never stubbed at all; they were only
 * linkable because `-dead_strip` dropped joy.c's unreachable save path (see the note in
 * build_mac.sh). Defining all five is what makes that path reachable. */

s32 osEepromProbe(void *mq)
{
    (void)mq;
 geSaveInit();
    /* 0 means no EEPROM. file2.c gates every read and write on this, so returning 0 is a
     * complete, safe off switch, and is exactly what GETV_SAVE=0 does. */
 return ge_eeprom_enabled ? EEPROM_TYPE_4K : 0;
}

s32 osEepromRead(void *mq, u8 address, u8 *buffer)
{
    (void)mq;
 geSaveInit();
 if (!ge_eeprom_enabled || buffer == NULL)  { return -1; }
 if (address >= GE_EEP_BLOCKS)              { return -1; }
 memcpy(buffer, ge_eep + (size_t)address * GE_EEP_BLOCK, GE_EEP_BLOCK);
 if (ge_eep_debug) {
 printf("[getv][save] read blk %2u\n", (unsigned)address);
    }
 return 0;
}

s32 osEepromWrite(void *mq, u8 address, u8 *buffer)
{
    (void)mq;
 geSaveInit();
 if (!ge_eeprom_enabled || buffer == NULL)  { return -1; }
 if (address >= GE_EEP_BLOCKS)              { return -1; }
 memcpy(ge_eep + (size_t)address * GE_EEP_BLOCK, buffer, GE_EEP_BLOCK);
 ge_eep_dirty = 1;
 if (ge_eep_debug) {
 printf("[getv][save] write blk %2u\n", (unsigned)address);
    }
    /* Flush per block, not at exit. The game has no clean shutdown path: the harness
     * _exit()s, GETV_EXIT_FRAME _exit()s, and a window can simply be closed, so an
     * atexit() flush would lose the save in the common case. 512 bytes to a page-cached
     * file is not worth deferring. */
 geSaveFlush();
 return 0;
}

/* libultra's own long forms (src/libultrare/io/conteeplong{read,write}.c) loop the
 * single-block call and gate on `address > 0x40`. Same shape here, minus the
 * inter-block 12 ms EEPROM settle timer, which was silicon and not semantics.
 * nbytes is not required to be a multiple of 8 by the caller; libultra just runs the loop
 * until it goes non-positive, so the final block is a full 8 bytes either way. Matching
 * that keeps the block addresses file2.c computes correct. */
s32 osEepromLongRead(void *mq, u8 address, u8 *buffer, int nbytes)
{
 s32 status = 0;

 if (address > 0x40) { return -1; }
 while (nbytes > 0) {
 status = osEepromRead(mq, address, buffer);
 if (status != 0) { return status; }
 nbytes  -= GE_EEP_BLOCK;
 address += 1;
 buffer  += GE_EEP_BLOCK;
    }
 return status;
}

s32 osEepromLongWrite(void *mq, u8 address, u8 *buffer, int nbytes)
{
 s32 status = 0;

 if (address > 0x40) { return -1; }
 while (nbytes > 0) {
 status = osEepromWrite(mq, address, buffer);
 if (status != 0) { return status; }
 nbytes  -= GE_EEP_BLOCK;
 address += 1;
 buffer  += GE_EEP_BLOCK;
    }
 return status;
}
