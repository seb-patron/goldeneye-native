/* GENERATED DIAGNOSTIC by tools/gen_link_stubs.py - not part of the port.
 *
 * One stub per still-undefined symbol so the app links and the boot path can be
 * traced. Symbols are classified from the decomp's own declarations: DATA gets a
 * real zeroed object, FUNCTIONS get a stub that logs once and returns 0 so the
 * boot continues.
 *
 * Emitting data symbols as functions (the naive approach) makes any read of them
 * return code bytes, which shows up as a SIGBUS that looks like a port bug.
 *
 * Delete once the real implementations land. */
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>

/* Over-allocated -- the real sizes are unknown, and a stub that is
 * too SMALL corrupts whatever global follows it (this is exactly how g_Props
 * ate the memory-pool table). Uninitialised, so it lives in BSS and the
 * generous size costs nothing in the binary. */
#define GE_STUB_BYTES  (256 * 1024)
#define GE_STUB_POISON 4096
#define GE_STUB_CANARY 16
#define GE_STUB_MAGIC  "GETVcanary_v1!!"

static void ge_stub_hit(const char *name)
{
    static const char *seen[1024];
    static int n = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (seen[i] == name) return;
    }
    if (n < 1024) seen[n++] = name;
    printf("[getv] STUB: %s\n", name);
    fflush(stdout);
}

unsigned char LaztJ[GE_STUB_BYTES];   /* unclassified */
unsigned char LsevbJ[GE_STUB_BYTES];   /* unclassified */
unsigned char LstatJ[GE_STUB_BYTES];   /* unclassified */
/* GETV: UsetuparchZ / UsetuplenZ stubs removed. They were stale --
 * assets/obseg/setup/{UsetuparchZ.c,{u,j,e}/UsetuplenZ.c} define these for real.
 * A tentative `unsigned char X[256K]` is a COMMON symbol, so ld merged it into the
 * real __data object without a duplicate-symbol error -- and then gePortStubInit()
 * memset 0xFF over the first 4096 bytes of the real stagesetup (and wrote a canary
 * 256KB downstream). That is why ARCHIVES' stagesetup read all-0xFF from boot.
 * gen_link_stubs.py must not re-emit a stub for a symbol the assets define. */
void *_rarewarelogoSegmentEnd = 0;   /* pointer global: NULL, not 0xFF */
void *_rarewarelogoSegmentRomStart = 0;   /* pointer global: NULL, not 0xFF */
void *_rarewarelogoSegmentStart = 0;   /* pointer global: NULL, not 0xFF */
unsigned char bg_ear_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_imp_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_lee_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_lip_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_pam_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_rit_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_sho_all_p_seg[GE_STUB_BYTES];   /* unclassified */
unsigned char bg_wax_all_p_seg[GE_STUB_BYTES];   /* unclassified */
long check_ramrom_flags(void) { ge_stub_hit("check_ramrom_flags"); return 0; }
long chrObjRandomGetNext(void) { ge_stub_hit("chrObjRandomGetNext"); return 0; }
long chrObjRandomSetSeed(void) { ge_stub_hit("chrObjRandomSetSeed"); return 0; }
long clear_ramrom_block_buffer_heading_ptrs(void) { ge_stub_hit("clear_ramrom_block_buffer_heading_ptrs"); return 0; }
long crashInit(void) { ge_stub_hit("crashInit"); return 0; }
long get_counters(void) { ge_stub_hit("get_counters"); return 0; }
long get_is_ramrom_flag(void) { ge_stub_hit("get_is_ramrom_flag"); return 0; }
long get_recording_ramrom_flag(void) { ge_stub_hit("get_recording_ramrom_flag"); return 0; }
unsigned char gfxFrameMsgQ[GE_STUB_BYTES];
unsigned char gsp3DDataStart[GE_STUB_BYTES];
unsigned char gsp3DTextStart[GE_STUB_BYTES];
void *indycommHostCheckFileExists(void *passthrough) { ge_stub_hit("indycommHostCheckFileExists"); return passthrough; }
long indycommHostLoadFile(void) { ge_stub_hit("indycommHostLoadFile"); return 0; }
void *indycommHostSendCmd(void *passthrough) { ge_stub_hit("indycommHostSendCmd"); return passthrough; }
long indycommHostSendDump(void) { ge_stub_hit("indycommHostSendDump"); return 0; }
long indycommInit(void) { ge_stub_hit("indycommInit"); return 0; }
long init_spectrum_game(void) { ge_stub_hit("init_spectrum_game"); return 0; }
long interface_menu0B_runstage(void) { ge_stub_hit("interface_menu0B_runstage"); return 0; }
long iterate_ramrom_entries_handle_camera_out(void) { ge_stub_hit("iterate_ramrom_entries_handle_camera_out"); return 0; }
long load_ramrom_from_devtool(void) { ge_stub_hit("load_ramrom_from_devtool"); return 0; }
unsigned char ob__ob_end_seg[GE_STUB_BYTES];   /* unclassified */
long objectiveGetStatus_WEAK(void) { ge_stub_hit("objectiveGetStatus_WEAK"); return 0; }
/* osEepromProbe / osEepromLongRead / osEepromLongWrite are REAL now -- see
 * port_save.c, which backs the cartridge EEPROM with a file under
 * ~/Library/Application Support/Goldeneye-Native/. They used to be zero-returning stubs
 * here, which made fileGamePakProbe() false and every save slot blank on every
 * launch. Do not re-add them: a second definition is a duplicate-symbol link error,
 * and the archive would resolve whichever member the linker reached first. */
long permit_stderr(void) { ge_stub_hit("permit_stderr"); return 0; }
long replay_recorded_ramrom_from_indy(void) { ge_stub_hit("replay_recorded_ramrom_from_indy"); return 0; }
long rmonGetToken(void) { ge_stub_hit("rmonGetToken"); return 0; }
long romCreateMesgQueue(void) { ge_stub_hit("romCreateMesgQueue"); return 0; }
unsigned char rspbootTextEnd[GE_STUB_BYTES];
unsigned char rspbootTextStart[GE_STUB_BYTES];
long run_spectrum_game(void) { ge_stub_hit("run_spectrum_game"); return 0; }
long save_ramrom_to_devtool(void) { ge_stub_hit("save_ramrom_to_devtool"); return 0; }
long select_ramrom_to_play(void) { ge_stub_hit("select_ramrom_to_play"); return 0; }
long setRamRomRecordSlot(void) { ge_stub_hit("setRamRomRecordSlot"); return 0; }
void *spectrum_draw_screen(void *passthrough) { ge_stub_hit("spectrum_draw_screen"); return passthrough; }
long stop_demo_playback(void) { ge_stub_hit("stop_demo_playback"); return 0; }
long stop_recording_ramrom(void) { ge_stub_hit("stop_recording_ramrom"); return 0; }
long test_if_recording_demos_this_stage_load(void) { ge_stub_hit("test_if_recording_demos_this_stage_load"); return 0; }
long tlbmanageResetCurrentEntriesCount(void) { ge_stub_hit("tlbmanageResetCurrentEntriesCount"); return 0; }
void *unknown2 = 0;   /* pointer global: NULL, not 0xFF */
void *unknown2_end = 0;   /* pointer global: NULL, not 0xFF */

/* ---- stub storage registry (see comment in gen_link_stubs.py) ---- */
static unsigned char *const ge_stub_ptrs[] = {
    LaztJ,
    LsevbJ,
    LstatJ,
    bg_ear_all_p_seg,
    bg_imp_all_p_seg,
    bg_lee_all_p_seg,
    bg_lip_all_p_seg,
    bg_pam_all_p_seg,
    bg_rit_all_p_seg,
    bg_sho_all_p_seg,
    bg_wax_all_p_seg,
    gfxFrameMsgQ,
    gsp3DDataStart,
    gsp3DTextStart,
    ob__ob_end_seg,
    rspbootTextEnd,
    rspbootTextStart,
    0
};
static const char *const ge_stub_names[] = {
    "LaztJ",
    "LsevbJ",
    "LstatJ",
    "bg_ear_all_p_seg",
    "bg_imp_all_p_seg",
    "bg_lee_all_p_seg",
    "bg_lip_all_p_seg",
    "bg_pam_all_p_seg",
    "bg_rit_all_p_seg",
    "bg_sho_all_p_seg",
    "bg_wax_all_p_seg",
    "gfxFrameMsgQ",
    "gsp3DDataStart",
    "gsp3DTextStart",
    "ob__ob_end_seg",
    "rspbootTextEnd",
    "rspbootTextStart",
    0
};

void gePortStubInit(void)
{
    int i;
    for (i = 0; ge_stub_ptrs[i]; i++) {
        /* Poison only the first GE_STUB_POISON bytes: that is where a struct's fields
         * and a table's first entries live, so a sentinel scan still terminates. The
         * tail stays zero and exists purely to absorb overruns. */
        memset(ge_stub_ptrs[i], 0xFF, GE_STUB_POISON);
        memcpy(ge_stub_ptrs[i] + GE_STUB_BYTES - GE_STUB_CANARY, GE_STUB_MAGIC,
               GE_STUB_CANARY);
    }
    printf("[getv] stub storage: %d arrays x %d bytes, poison %d, canaried\n",
           i, GE_STUB_BYTES, GE_STUB_POISON);
    fflush(stdout);
}

/* Returns the name of the first overflowed stub, or 0. Cheap enough to call from
 * every boot mark, which is what pins an overflow to a single call. */
const char *gePortStubCheck(void)
{
    int i;
    for (i = 0; ge_stub_ptrs[i]; i++) {
        if (memcmp(ge_stub_ptrs[i] + GE_STUB_BYTES - GE_STUB_CANARY, GE_STUB_MAGIC,
                   GE_STUB_CANARY) != 0) {
            return ge_stub_names[i];
        }
    }
    return 0;
}



#if defined(_WIN32)
/* bcopy and bzero for MinGW.
 *
 * BSD functions that glibc and Darwin still provide and the Microsoft CRT does not. The
 * decomp calls both -- they are what the N64 SDK's own code reached for -- and declares
 * them in include/bstring.h and PR/os.h as taking an `int` length. The signatures here
 * match those declarations deliberately: with the prototype already visible tree-wide,
 * these are ordinary definitions and no header needs changing.
 *
 * bcopy takes source first, the reverse of memcpy, and is defined to tolerate overlapping
 * regions -- so memmove is the correct implementation and memcpy would be a real bug on
 * exactly the inputs bcopy exists to handle. */
void bcopy(const void *src, void *dst, int n)
{
    if (n > 0) memmove(dst, src, (size_t) n);
}

void bzero(void *dst, int n)
{
    if (n > 0) memset(dst, 0, (size_t) n);
}
#endif /* _WIN32 */
