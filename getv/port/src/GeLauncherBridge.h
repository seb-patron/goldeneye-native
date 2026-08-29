#ifndef GE_LAUNCHER_BRIDGE_H
#define GE_LAUNCHER_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Swift-facing view of ge_launcher.cpp's Model -- see that file's "Swift bridge" comment
 * (right above these functions' definitions) for why this exists and how it stays in
 * sync with the same ImGui UI's own copy of the settings. */

void geBridgeLoad(void);
void geBridgeSave(void);

int geBridgeStageCount(void);
const char *geBridgeStageName(int i);
const char *geBridgeStagePlace(int i);
int geBridgeStageMission(int i);
int geBridgeStageMpOnly(int i);

int geBridgeRulesetCount(void);
const char *geBridgeRulesetName(int i);

int geBridgeCheatCount(void);
const char *geBridgeCheatLabel(int i);
int geBridgeCheatLive(int i);

int  geBridgeGetPickStage(void);
void geBridgeSetPickStage(int v);
int  geBridgeGetStageIdx(void);
void geBridgeSetStageIdx(int v);

int  geBridgeGetProfile(void);
void geBridgeSetProfile(int v);

int  geBridgeGetRuleset(void);
void geBridgeSetRuleset(int v);

int  geBridgeGetHorde(void);
void geBridgeSetHorde(int v);
int  geBridgeGetHordePerKill(void);
void geBridgeSetHordePerKill(int v);
int  geBridgeGetHordePerKillCap(void);
void geBridgeSetHordePerKillCap(int v);
int  geBridgeGetHordeMaxAlive(void);
void geBridgeSetHordeMaxAlive(int v);
int  geBridgeGetHordeWaveKills(void);
void geBridgeSetHordeWaveKills(int v);
int  geBridgeGetHordeGrowth(void);
void geBridgeSetHordeGrowth(int v);

int  geBridgeGetSupersample(void);
void geBridgeSetSupersample(int v);
int  geBridgeGetFov(void);
void geBridgeSetFov(int v);
int  geBridgeGetFramerate(void);
void geBridgeSetFramerate(int v);
int  geBridgeGetMsaa(void);
void geBridgeSetMsaa(int v);
int  geBridgeGetFxaa(void);
void geBridgeSetFxaa(int v);
int  geBridgeGetHdTextures(void);
void geBridgeSetHdTextures(int v);
const char *geBridgeGetTexpackPath(void);
void geBridgeSetTexpackPath(const char *path);

int  geBridgeGetAniso(void);
void geBridgeSetAniso(int v);
int  geBridgeGetFiltering(void);
void geBridgeSetFiltering(int v);
int  geBridgeGetWidescreen(void);
void geBridgeSetWidescreen(int v);
int  geBridgeGetMipmaps(void);
void geBridgeSetMipmaps(int v);
int  geBridgeGetParallax(void);
void geBridgeSetParallax(int v);
int  geBridgeGetCrosshairScalePct(void);
void geBridgeSetCrosshairScalePct(int v);
/* RGB, each 0..1. Three scalar accessors rather than one struct/array crossing the bridge --
 * matches every other field here, and Swift's C interop has no clean way to receive a
 * fixed-size C array by value anyway. */
float geBridgeGetCrosshairR(void);
float geBridgeGetCrosshairG(void);
float geBridgeGetCrosshairB(void);
void  geBridgeSetCrosshairColor(float r, float g, float b);
int  geBridgeGetFullscreen(void);
void geBridgeSetFullscreen(int v);
const char *geBridgeGetResolution(void);
void geBridgeSetResolution(const char *wh);
int  geBridgeGetUncapped(void);
void geBridgeSetUncapped(int v);
int  geBridgeGetDevOverlay(void);
void geBridgeSetDevOverlay(int v);

int  geBridgeGetRsCustom(void);
void geBridgeSetRsCustom(int v);
int  geBridgeGetEnemyHealth(void);
void geBridgeSetEnemyHealth(int v);
int  geBridgeGetEnemyDamage(void);
void geBridgeSetEnemyDamage(int v);
int  geBridgeGetEnemyAccuracy(void);
void geBridgeSetEnemyAccuracy(int v);
int  geBridgeGetEnemyReaction(void);
void geBridgeSetEnemyReaction(int v);
int  geBridgeGetPlayerHealth(void);
void geBridgeSetPlayerHealth(int v);
int  geBridgeGetPlayerArmour(void);
void geBridgeSetPlayerArmour(int v);
int  geBridgeGetAmmoPct(void);
void geBridgeSetAmmoPct(int v);
int  geBridgeGetExplosionDamage(void);
void geBridgeSetExplosionDamage(int v);
int  geBridgeGetTurretDamage(void);
void geBridgeSetTurretDamage(int v);

int  geBridgeGetMouse(void);
void geBridgeSetMouse(int v);
int  geBridgeGetMouseSens(void);
void geBridgeSetMouseSens(int v);
int  geBridgeGetMouseInvert(void);
void geBridgeSetMouseInvert(int v);
int  geBridgeGetKeyboard(void);
void geBridgeSetKeyboard(int v);

const char *geBridgeGetModDir(void);
void geBridgeSetModDir(const char *dir);
void geBridgeRescanMods(void);

/* Control rebinding -- the six bindable actions (kActions in ge_launcher.cpp) and the
 * eleven named sources (kSources) a player can point one at. bindTab: 0 = ALL, 1..4 = that
 * player, matching ge_launcher.cpp's own ImGui tab exactly. get/setBindAll and
 * get/setBindP's `src` is an index into the source list, or -1 for "unset" (falls back to
 * the action's own default on the ALL tab, or to whatever ALL resolves to on a player tab). */
int geBridgeActionCount(void);
const char *geBridgeActionLabel(int i);
const char *geBridgeActionDefault(int i);
int geBridgeSourceCount(void);
const char *geBridgeSourceName(int i);
int  geBridgeGetBindTab(void);
void geBridgeSetBindTab(int v);
int  geBridgeGetBindAll(int action);
void geBridgeSetBindAll(int action, int src);
int  geBridgeGetBindP(int player, int action);
void geBridgeSetBindP(int player, int action, int src);
void geBridgeResetBindTab(void);

int geBridgeModCount(void);
const char *geBridgeModName(int i);
int  geBridgeGetModOn(int i);
void geBridgeSetModOn(int i, int on);

int  geBridgeGetCheatOn(int i);
void geBridgeSetCheatOn(int i, int on);

/* ge_virtual_controller.mm -- forces the device out of the portrait it launches in. Only
 * gfx_sdl2.c's game-window init called this before; the launcher's own UIWindow never did,
 * so it rendered sideways (Info.plist/SDL_HINT_ORIENTATIONS only restrict which
 * orientations are ALLOWED, neither one actually rotates an already-portrait launch). */
void gePortForceLandscapeOrientation(void);

#ifdef __cplusplus
}
#endif

#endif /* GE_LAUNCHER_BRIDGE_H */
