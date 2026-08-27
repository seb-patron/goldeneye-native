/* What a body can act on, backed by the level knowledge rather than the engine.
 *
 * ge_sense_api declares gePortUsableCount/gePortUsableAt as decomp symbols, on the reasonable
 * assumption that the game keeps a list of interactive objects. It does not keep one a caller can
 * read: props live behind the object handler and the only global arrays are the tank props.
 *
 * But the answer is already packed. The world API carries every door, key and collectable in the
 * level with its position, room and nav node -- 4,871 props across 28 levels -- so the honest
 * implementation reads that rather than inventing an engine list. It also means "usable" stays a
 * question about the LEVEL, which is where a modder would expect to change it.
 *
 * ⚠️ It therefore describes what is PLACED, not what is currently interactive: a door already
 * open still appears. Runtime state needs the engine, and when a readable list turns up this
 * should move onto it. The distinction matters for a bot that would otherwise keep pressing use
 * on a door it has already opened.
 */
#include <stddef.h>

#include "ge_world_api.h"
#include "ge_sense_api.h"

int gePortUsableCount(void)
{
    return geWorldPropCount();
}

int gePortUsableAt(int index, float *out)
{
    GeWorldProp pr;

    if (out == NULL || !geWorldProp(index, &pr)) { return 0; }

    out[0] = pr.x;
    out[1] = pr.y;
    out[2] = pr.z;

    /* Only kinds a body can actually do something with. Everything else reports NONE rather than
     * a guessed category -- a caller will act on a kind, so a wrong one is worse than no one. */
    switch (pr.kind) {
    case GE_PROP_DOOR:
        out[3] = (float) GE_USABLE_DOOR;
        break;
    case GE_PROP_KEY:
    case GE_PROP_COLLECTABLE:
    case GE_PROP_AMMOBOX:
    case GE_PROP_AMMOMAG:
    case GE_PROP_ARMOUR:
        out[3] = (float) GE_USABLE_PICKUP;
        break;
    default:
        out[3] = (float) GE_USABLE_NONE;
        break;
    }

    /* The setup tag, so a caller can tie this back to an objective's target list. -1 when the
     * prop carries none; 0 is a real tag. */
    out[4] = (float) pr.tag;
    return 1;
}
