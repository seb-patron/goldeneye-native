#include <stdio.h>
#include "objecthandler.h"

#ifndef GE_NATIVE_SLOT_METADATA
#define GE_SLOT_RWDATA(slot) ((slot).unk10)
#define GE_SLOT_FREE(slot) ((slot).ge_inuse == 0)
#define GE_SLOT_CLAIM(slot, rwdata) \
    do { \
        (slot).ge_inuse = 1; \
        (slot).unk10 = (rwdata); \
    } while (0)
#define GE_SLOT_RELEASE(slot) ((slot).ge_inuse = 0)
#define GE_SLOT_CAN_FIT(slot, required) \
    ((required) <= 0 || (GE_SLOT_RWDATA(slot) != NULL \
        && (slot).unk02 >= (required)))

static s32 geModelSlotPoolIndex(const void *model, const void *slots,
        u32 slotsize, s32 count)
{
    s32 i;

    for (i = 0; i < count; i++) {
        if (model == (const void *)((const u8 *)slots + i * slotsize)) {
            return i;
        }
    }

    return -1;
}
#endif

static int checks;
static int failures;

static void check(int condition, const char *message)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    checks++;
    failures += !condition;
}

static s32 first_eligible_anim_slot(struct AnimModelSlot *slots, s32 count, s16 required)
{
    s32 i;

    for (i = 0; i < count; i++) {
        if (GE_SLOT_FREE(slots[i]) && GE_SLOT_CAN_FIT(slots[i], required)) {
            return i;
        }
    }

    return -1;
}

int main(void)
{
    struct AnimModelSlot slots[2] = {0};
    struct ModelSlot modelslots[2] = {0};
    u32 rwdata0[420];
    u32 rwdata1[420];
    ModelFileHeader object_marker = {0};
    Model *model = (Model *)&slots[0];

    slots[0].unk02 = 420;
    slots[1].unk02 = 420;
    GE_SLOT_RWDATA(slots[0]) = rwdata0;
    GE_SLOT_RWDATA(slots[1]) = rwdata1;

    check(GE_SLOT_FREE(slots[0]), "fresh slot is free");
    check(GE_SLOT_CAN_FIT(slots[0], 155), "fresh slot has eligible rwdata capacity");
    check(first_eligible_anim_slot(slots, 2, 155) == 0,
        "allocator selects the first eligible slot");

    GE_SLOT_CLAIM(slots[0], rwdata0);
    model->obj = &object_marker;
    check(!GE_SLOT_FREE(slots[0]), "claimed slot is unavailable");
    check(GE_SLOT_RWDATA(slots[0]) == rwdata0,
        "Model.obj write does not overwrite rwdata backing");

    model->obj = NULL;
    GE_SLOT_RELEASE(slots[0]);
    check(GE_SLOT_FREE(slots[0]), "released slot becomes free");
    check(GE_SLOT_RWDATA(slots[0]) == rwdata0,
        "release preserves rwdata backing");
    check(GE_SLOT_CAN_FIT(slots[0], 155),
        "released slot remains rwdata-eligible");
    check(first_eligible_anim_slot(slots, 2, 155) == 0,
        "released slot is selected for reuse");

    check(geModelSlotPoolIndex(&slots[0], slots, sizeof(slots[0]), 2) == 0,
        "pool lookup identifies the first exact element");
    check(geModelSlotPoolIndex(&slots[1], slots, sizeof(slots[0]), 2) == 1,
        "pool lookup identifies the second exact element");
    check(geModelSlotPoolIndex((u8 *)&slots[0] + 1, slots,
        sizeof(slots[0]), 2) == -1, "pool lookup rejects an interior pointer");
    check(geModelSlotPoolIndex(modelslots, slots,
        sizeof(slots[0]), 2) == -1, "pool lookup rejects external storage");

    modelslots[0].unk02 = 60;
    GE_SLOT_CLAIM(modelslots[0], rwdata1);
    ((Model *)&modelslots[0])->obj = &object_marker;
    ((Model *)&modelslots[0])->obj = NULL;
    GE_SLOT_RELEASE(modelslots[0]);
    check(GE_SLOT_FREE(modelslots[0]) && GE_SLOT_RWDATA(modelslots[0]) == rwdata1,
        "non-animated slot has the same independent lifecycle metadata");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0 || checks == 0;
}
