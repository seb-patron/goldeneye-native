#!/usr/bin/env python3
"""S7: does the derived wall set block a step the ENGINE considers walkable?

ANSWERED, AND THE ANSWER INVALIDATES THIS TOOL'S PREMISE. READ THIS BEFORE QUOTING ITS NUMBER.

A waypoint link does NOT promise a clear straight line. It promises REACHABILITY. Traced through
the decompilation rather than guessed:

  padhalllv.c   waypointFindNextStepToward returns a NEIGHBOUR of the current waypoint --
                waypointFindRandomByDist(pointa->neighbours, ...) -- not a direction to the goal.
  chraction.c   the caller (10495) stores it as `self->padpreset1`, a TARGET PAD.
  chr.c:1468    the guard then moves toward that pad through a collision-tested step:
                stanTestLineUnobstructed AND stanTestVolume, before every move it commits to.

That last one settles it. IF AN EDGE GUARANTEED A CLEAR STRAIGHT LINE, THE PER-STEP COLLISION TEST
WOULD BE POINTLESS. The engine tests because it does not assume, and a guard plots a course around
what it finds.

So the 76.5% this tool reports is measuring something the engine never claimed: straight segments
between graph nodes clipping interior geometry is EXPECTED, and the Mac build's wall derivation is not
indicted by it. The figure is kept because the tool is still the right instrument for a different
question -- "which links have obstructions between their endpoints" is useful for a follower that
DOES want to move in straight lines -- but it is not a wall-set defect rate and must not be quoted
as one.

The one genuine defect it found stands: the wall set contains DUPLICATE segments, a shared tile
edge emitted once from each side.


WHY THE ENGINE'S LINKS ARE THE RIGHT TEST. Validating walls against OUR generated routes only asks
whether two of our own derivations agree -- if the tile graph and the wall set share a mistake, they
agree loudly and prove nothing. g_CurrentSetup.pathwaypoints is different in kind: each link is a
hand-authored assertion by people who could playtest that a guard walks from this node to that one.
A wall crossing one of those is either a flaw in the wall derivation or a doorway the gap-cutting
missed, and both are worth knowing.

THIS MEASURES; IT DOES NOT FIX. tools/gen_level_walls.py is the Mac build's and is deliberately not
touched. A number and the worst offenders are more use to its author than a patch from someone who
did not write it.

SHARED ENDPOINTS ARE NOT CROSSINGS. Waypoint links very often begin or end ON a wall -- a node
against a corridor wall is normal, not a fault -- so an intersection within EPS of either segment's
endpoint is ignored. Counting those would report most links as blocked and the number would be
about the geometry convention rather than about the walls.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPS = 1e-6


def seg_cross(ax, az, bx, bz, cx, cz, dx, dz):
    """True when segments AB and CD properly cross, endpoints excluded."""
    def d(px, pz, qx, qz, rx, rz):
        return (qx - px) * (rz - pz) - (qz - pz) * (rx - px)

    d1 = d(cx, cz, dx, dz, ax, az)
    d2 = d(cx, cz, dx, dz, bx, bz)
    d3 = d(ax, az, bx, bz, cx, cz)
    d4 = d(ax, az, bx, bz, dx, dz)
    # Strict signs on both sides: a touch (a zero) is a shared endpoint or a graze, and neither is
    # a crossing. Using <= here is what turns "the node sits against a wall" into a false positive.
    return ((d1 > EPS and d2 < -EPS) or (d1 < -EPS and d2 > EPS)) and \
           ((d3 > EPS and d4 < -EPS) or (d3 < -EPS and d4 > EPS))


def wall_segments(w):
    """Normalise whatever shape gen_level_walls.py emits into (x1,z1,x2,z2) tuples.

    Read defensively rather than assuming a key name: this file is someone else's output and an
    audit that silently finds zero segments because a key was renamed would report a perfect score.
    """
    segs = []
    src = w.get("walls") or w.get("segments") or []
    for s in src:
        if isinstance(s, dict):
            yv = s.get("y")
            yv = float(yv) if yv is not None else None
            for keys in (("x1", "z1", "x2", "z2"), ("ax", "az", "bx", "bz")):
                if all(k in s for k in keys):
                    segs.append(tuple(float(s[k]) for k in keys) + (yv,))
                    break
            else:
                p = s.get("a") or s.get("from")
                q = s.get("b") or s.get("to")
                if isinstance(p, (list, tuple)) and isinstance(q, (list, tuple)):
                    segs.append((float(p[0]), float(p[-1]), float(q[0]), float(q[-1]), yv))
        elif isinstance(s, (list, tuple)) and len(s) >= 4:
            # the fifth element IS the floor height, and dropping IT makes this audit meaningless.
            #
            # gen_level_walls.py emits [x1, z1, x2, z2, y]. The first version of this file sliced
            # s[:4] and tested in plan view, which lets a wall on one deck block a link on another.
            # It reported 76% of all engine links blocked -- and the ORDER gave it away: Dam 97%,
            # Statue 93%, Surface 90%, all multi-storey, against flat Train at 20%. A wall
            # derivation is not four times worse on Dam than on Train; a 2D test is.
            #
            # y is carried through and compared below. Absent (a 4-element segment) becomes None,
            # meaning "applies at any height", which is the conservative reading.
            y = float(s[4]) if len(s) >= 5 else None
            segs.append(tuple(float(v) for v in s[:4]) + (y,))
    return segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    ap.add_argument("--level", default=None)
    ap.add_argument("--yband", type=float, default=150.0,
                    help="how far a wall's floor height may sit from the link's height band and "
                         "still count as obstructing it (asset units). Separates DECKS, not bodies.")
    a = ap.parse_args()

    rows = []
    for f in sorted(os.listdir(a.levels)):
        if not f.endswith(".walls.json"):
            continue
        lv = f[: -len(".walls.json")]
        if a.level and lv != a.level:
            continue
        npath = os.path.join(a.levels, "%s.nodes.json" % lv)
        if not os.path.isfile(npath):
            continue

        walls = wall_segments(json.load(open(os.path.join(a.levels, f), encoding="utf-8")))
        nodes = json.load(open(npath, encoding="utf-8"))
        pos = {n["uid"]: n["pos"] for n in nodes["nodes"]}
        wp = [n for n in nodes["nodes"] if n["ns"] == "waypoint"]

        # Each undirected engine link once. The graph lists both directions and counting a->b and
        # b->a separately would double every figure without adding information.
        links = set()
        for n in wp:
            for m in n.get("links", []):
                if m in pos:
                    links.add((min(n["uid"], m), max(n["uid"], m)))

        blocked, worst = 0, []
        for u, v in links:
            ax, az = pos[u][0], pos[u][2]
            bx, bz = pos[v][0], pos[v][2]
            ay, by = pos[u][1], pos[v][1]
            lo_y, hi_y = (ay, by) if ay <= by else (by, ay)
            lo_x, hi_x = (ax, bx) if ax <= bx else (bx, ax)
            lo_z, hi_z = (az, bz) if az <= bz else (bz, az)
            for (cx, cz, dx, dz, wy) in walls:
                # HEIGHT FIRST. A wall belongs to the deck whose floor produced it, and a link
                # running above or below it is not obstructed by it. Without this the audit reports
                # a multi-storey level as almost entirely walled -- see the note in wall_segments.
                #
                # The band is generous: a waypoint's y is a PAD height and a wall's y is
                # a FLOOR height, and those differ by the body-to-floor offset, which this project
                # has already measured at 12-202 units depending on the level. A tight band would
                # reject real crossings; this only has to separate DECKS, which are much further
                # apart than that.
                if wy is not None and (wy < lo_y - a.yband or wy > hi_y + a.yband):
                    continue
                # Cheap bbox reject first: with 7,500 walls and 500 links the exact test alone is
                # millions of operations, and the overwhelming majority cannot possibly touch.
                if max(cx, dx) < lo_x or min(cx, dx) > hi_x: continue
                if max(cz, dz) < lo_z or min(cz, dz) > hi_z: continue
                if seg_cross(ax, az, bx, bz, cx, cz, dx, dz):
                    blocked += 1
                    worst.append((u - 1000000, v - 1000000))
                    break

        pct = 100.0 * blocked / len(links) if links else 0.0
        rows.append((pct, lv, blocked, len(links), len(walls), worst))

    rows.sort(reverse=True)
    print("%-12s %8s %10s %8s   %s" % ("level", "walls", "eng.links", "blocked", "rate"))
    tb = tl = 0
    for pct, lv, blocked, nl, nw, worst in rows:
        tb += blocked; tl += nl
        flag = "  <-- look" if pct >= 5.0 else ""
        print("%-12s %8d %10d %8d   %5.1f%%%s" % (lv, nw, nl, blocked, pct, flag))
    print("\nOVERALL: %d of %d engine links crossed by a wall (%.2f%%)"
          % (tb, tl, 100.0 * tb / tl if tl else 0.0))

    for pct, lv, blocked, nl, nw, worst in rows[:3]:
        if worst:
            print("\n%s worst offenders (engine waypoint pairs): %s%s"
                  % (lv, worst[:10], " ..." if len(worst) > 10 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
