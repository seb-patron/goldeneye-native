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
