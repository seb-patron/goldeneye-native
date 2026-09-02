/* Native fixups for setup sections backed by statically linked generated assets. */
#ifndef GE_SETUP_FIXUPS_H
#define GE_SETUP_FIXUPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 exactly once for each non-NULL setup-section address, then 0 for later loads.
 * Stage setup runs on the game thread, so this process-lifetime registry needs no locking. */
int gePortSetupSectionNeedsScale(const void *section);

#ifdef __cplusplus
}
#endif
#endif /* GE_SETUP_FIXUPS_H */
