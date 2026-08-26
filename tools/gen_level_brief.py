#!/usr/bin/env python3
"""Distil the walkthrough reconstructions into one short brief per level.

The ingested documents run to hundreds of thousands of characters and carry eighty-odd embedded
JSON blocks between them. Almost none of that belongs at runtime -- but a handful of facts do, and
they are facts no amount of asset reading recovers:

    topology        linear, hub, open       -- decides whether "which way" is even a question
    traversal_axis  +X, -Z                  -- on a corridor this IS the route
    player_start    rear_of_car_1           -- where the designer put you
    player_goal     front_command_area      -- what you are meant to reach
    branching       0                       -- whether route choice exists at all

THIRD-PARTY PROSE, AND THE BRIEF SAYS SO. These are human reconstructions, not measurements.
Where a brief and our own extraction disagree, the extraction wins and the disagreement is worth
investigating -- that is how Train's 39.5:1 corridor got confirmed rather than assumed.

The brief is a HINT LAYER, never a source of coordinates. Nothing here is in game units and
nothing should be treated as one: "car_length_m: 29" is a designer's metre, not a world unit, and
the two differ by a scale nobody has pinned down.
"""
import argparse
import glob
import json
import os

# The keys worth keeping. Everything else in those blocks is prose restated as JSON -- interesting
# to read, not useful to a program, and carrying it forward would make the brief a second copy of
# the document rather than a summary of it.
KEEP = ("topology", "traversal_axis", "player_start", "player_goal", "branching",
        "principal_cars", "car_length_m", "car_width_m", "lateral_escape",
        "primary_traversal", "level",
        # The navigation-character keys. These turned up in the reconstructions as a set --
        # Archives carries {branching, backtracking, multiple_routes, landmark_based} together --
        # and keeping only "branching" reduced a four-fact description of how a level PLAYS to a
        # single bare true, which read like a parser artefact and nearly got the whole thing
        # dismissed as one.
        "backtracking", "multiple_routes", "landmark_based", "verticality",
        "combat_density", "key_required", "objectives_ordered")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join("build", "levels"))
    args = ap.parse_args()

    written = 0
    for path in sorted(glob.glob(os.path.join(args.levels, "*.walkthrough.json"))):
        level = os.path.basename(path)[:-len(".walkthrough.json")]
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)

        brief = {}
        for d in doc.get("documents", []):
            for block in d.get("structured", []):
                if not isinstance(block, dict):
                    continue
                hit = {k: v for k, v in block.items() if k in KEEP}
                # A block naming this level is authoritative over a generic one; otherwise first
                # writer wins, so a later block cannot quietly overwrite a better earlier one.
                if hit.get("level") == level:
                    brief.update(hit)
                else:
                    for k, v in hit.items():
                        brief.setdefault(k, v)

        brief.pop("level", None)
        if not brief:
            continue

        brief["_source"] = "human reconstruction, not measured -- our extraction wins on conflict"
        with open(os.path.join(args.levels, level + ".brief.json"), "w", encoding="utf-8") as fh:
            json.dump(brief, fh, indent=1)
        written += 1
        facts = ", ".join("%s=%s" % (k, v) for k, v in sorted(brief.items())
                          if not k.startswith("_"))
        print("  %-10s %s" % (level, facts[:96]))

    print("\n%d brief(s) written" % written)


if __name__ == "__main__":
    main()
