/* GoldenEye tvOS port - the N64 Video Interface, replaced.
 *
 * src/fr.c is GoldenEye's video layer. Most of it is game logic we want: view
 * sizes, split-screen viewports, FOV, Z range, and the display lists it emits. But
 * underneath, it programs the N64's VI registers directly -- osViBlack(),
 * osViSetYScale(), osViSetSpecialFeatures(), and mode selection out of
 * osViModeTable[].
 *
 * On tvOS that hardware does not exist and Fast3D + SDL already IS the video
 * interface: it owns the drawable, the swap, and the 2x supersampled 3840x2160 ->
 * 1920x1080 scaling. So rather than link libultra's VI (a bridge to nowhere), the
 * register-level calls become no-ops here and fr.c's game logic runs unchanged.
 *
 * The mode table is plausible rather than real. fr.c reads comRegs.hStart and
 * fldRegs[].vStart from the selected mode and writes comRegs.width; those values
 * only ever reached VI registers, so sane NTSC numbers keep fr.c's arithmetic well
 * defined without pretending to drive hardware.
 *
 * Known consequence: viShake() works by offsetting hStart/vStart, i.e. by moving
 * the VI's scanout window. With no VI there is nothing to offset, so screen shake
 * is inert. Re-implement it as a view-matrix or viewport offset when the game runs.
 */
#include <PR/ultratypes.h>
#include <PR/os.h>

/* NUM_VIDEO_FRAME_BUFFERS worth of per-buffer VI state. Defined in src/sched.c on
 * the N64, which is the scheduler this port does not build. */
#define GE_VI_BUFFERS 2

f32       g_ViXScales[GE_VI_BUFFERS]        = { 1.0f, 1.0f };
f32       g_ViYScales[GE_VI_BUFFERS]        = { 1.0f, 1.0f };
s32       g_ViChangeVideoModes[GE_VI_BUFFERS] = { 0, 0 };
OSViMode  g_ViModes[GE_VI_BUFFERS];
OSViMode *g_ViModePtrs[GE_VI_BUFFERS]       = { &g_ViModes[0], &g_ViModes[1] };

/* 1 = NTSC. GoldenEye branches on MPAL vs NTSC when choosing a mode; NTSC is the
 * US build this port targets. */
u32 osTvType = 1;

/* Indices used by fr.c reach OS_VI_MPAL_HAF1 (39), so the table must be at least
 * 40 entries. Only hStart/vStart are ever read back, and every entry carries the
 * standard NTSC 320x240 values -- mode choice cannot matter when nothing downstream
 * programs a VI. */
#define GE_VI_MODE_COUNT 44

OSViMode osViModeTable[GE_VI_MODE_COUNT];

__attribute__((constructor))
static void ge_vi_mode_table_init(void)
{
    for (int i = 0; i < GE_VI_MODE_COUNT; i++) {
        osViModeTable[i].comRegs.width      = 320;
        osViModeTable[i].comRegs.hStart     = 0x006C02EC;  /* NTSC LAN1 */
        osViModeTable[i].fldRegs[0].vStart  = 0x002501FF;
        osViModeTable[i].fldRegs[1].vStart  = 0x002501FF;
    }
    for (int i = 0; i < GE_VI_BUFFERS; i++) {
        g_ViModes[i] = osViModeTable[OS_VI_NTSC_LAN1];
    }
}

/* ---- register-level calls: no-ops on tvOS ------------------------------- */

/* Blanks VI output. Fast3D owns the drawable; there is nothing to blank. */
void osViBlack(u8 active) { (void)active; }

/* Vertical scaling, which Fast3D's supersampling path already handles. */
void osViSetYScale(f32 scale) { (void)scale; }

/* Dither / gamma / interlace filters, all VI hardware features. */
void osViSetSpecialFeatures(u32 func) { (void)func; }
