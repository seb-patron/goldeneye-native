#!/usr/bin/env python3
"""Extract per-level knowledge from the setup assets into machine-readable JSON.

Why this and not A walkthrough
------------------------------
The obvious way to give a bot level knowledge is to scrape FAQs. That is the wrong source
twice over.

Legally: walkthroughs on GameFAQs and similar are user-authored copyrighted works, and those
sites' terms prohibit automated access. Downloading them wholesale into this repository would be
redistributing someone else's text. Facts extracted from them -- "Dam has five objectives" --
are not copyrightable, but the prose is, and the line is not one to walk casually.

Technically: a walkthrough is a lossy human description of data we already have exactly. The
setup files carry the real thing:

    padlist            named positions with world coordinates, e.g. "p256d2"
    pad3dlist          bounded pads (volumes rather than points)
    pathwaypoints      waypoint -> pad, i.e. where each navigation node actually is
    Path_table_N       per-waypoint neighbours: the navigation graph
    pathsets           a much coarser REGION graph over waygroups -- NOT navigation
    patrolpaths        authored guard routes
    propDefs           every object placed in the level, doors included
    intro              spawn and camera records
    ailists            the behaviour scripts

`path_table_N[] = { 5, 39, 41, 42, -1 }` is an adjacency list. No FAQ contains that, and it is
precisely what a bot needs in order to walk somewhere.

Do not confuse it with `path_neighbors_N`: Dam has 205 path_tables for its 206 waypoints but
only 23 path_neighbors, and Statue has 198 waypoints against 4. The coarse one parses just as
cleanly and yields a bot that can reach four places on a two-hundred-node map.

This is also the data `init_path_table_links` consumes -- the function that is fully stubbed in
this port, which is why live pathfinding does not work today. Extracting it here makes the graph
usable from outside the engine even while that stub stands.

OUTPUT
------
One JSON document per level, plus an index. Written to a directory of your choosing; nothing is
written into vendor/, which is gitignored and regenerated.

    python3 tools/gen_level_knowledge.py --out build/levels
    python3 tools/gen_level_knowledge.py --out build/levels --level cradle

The generated JSON is not committed. This script is, because anything that must survive belongs
in a generator -- vendor/ does not travel between machines.
"""

import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# Setups live in two places: the region overrides in setup/{u,e,j} take precedence over the
# shared ones in setup/. Search u/ first, then the root.
SETUP_DIRS = [
    os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg", "setup", "u"),
    os.path.join(ROOT, "vendor", "ge-decomp", "assets", "obseg", "setup"),
]

# Transcribed from the game's OWN table, `setup_text_pointers[]` at
# vendor/ge-decomp/src/game/chraidata.c:851, whose comment reads "Also happens to be the same
# indices as levelID". Guessing these from the level's English name gets them wrong: Facility is
# Usetupark and Archives is Usetuparch, which is exactly the pair a reasonable guess transposes.
#
# name -> (setup stem, stage id, campaign mission number, 0 = multiplayer-only)
LEVELS = {
    "dam":       ("Usetupdam",        33, 1),
    "facility":  ("Usetupark",        34, 2),
    "runway":    ("Usetuprun",        35, 3),
    "surface":   ("Usetupsevx",       36, 4),
    "bunker1":   ("Usetupsevbunker",   9, 5),
    "silo":      ("Usetupsilo",       20, 6),
    "frigate":   ("Usetupdest",       26, 7),
    "surface2":  ("Usetupsevxb",      43, 8),
    "bunker2":   ("Usetupsevb",       27, 9),
    "statue":    ("Usetupstatue",     22, 10),
    "archives":  ("Usetuparch",       24, 11),
    "streets":   ("Usetuppete",       29, 12),
    "depot":     ("Usetupdepo",       30, 13),
    "train":     ("Usetuptra",        25, 14),
    "jungle":    ("Usetupjun",        37, 15),
    "control":   ("Usetupcontrol",    23, 16),
    "caverns":   ("Usetupcave",       39, 17),
    "cradle":    ("Usetupcrad",       41, 18),
    "aztec":     ("Usetupazt",        28, 19),
    "egypt":     ("Usetupcryp",       32, 20),
    # Multiplayer-only arenas. The launcher lists these separately and they have no campaign
    # slot, hence mission 0. Note the Ump_setup prefix: prop.c:1507 synthesises the MP name by
    # inserting "mp_" after the leading U when getPlayerCount() >= 2, so MP-ness is a function
    # of player count rather than a separate stage id.
    "complex":   ("Ump_setupref",     31, 0),
    "temple":    ("Ump_setupdish",    38, 0),
    "basement":  ("Ump_setupimp",     45, 0),
    "stack":     ("Ump_setupash",     46, 0),
    "library":   ("Ump_setupame",     48, 0),
    "caves":     ("Ump_setupoat",     50, 0),
}

FLOAT = r"-?\d+(?:\.\d+)?(?:e-?\d+)?f?"


def _f(tok):
    return float(tok.rstrip("f"))


def parse_pads(text, stem):
    """padlist entries: { {x,y,z}, {up}, {look}, "name", flags }"""
    m = re.search(r"PadRecord\s+%s\w*_padlist\[\]\s*=\s*\{(.*?)\n\};" % re.escape(stem),
                  text, re.S)
    if not m:
        return []
    body = m.group(1)
    pads = []
    row = re.compile(
        r"\{\s*\{\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*\}\s*,"      # pos
        r"\s*\{[^}]*\}\s*,\s*\{[^}]*\}\s*,"                    # up, look
        # The trailing field is a pointer, and the two setup exports disagree on how they spell
        # a null one: campaign files write 0, multiplayer files write NULL. Requiring an integer
        # meant every multiplayer pad failed to match and the arenas came out with zero pads --
        # no error, just empty data, which is the same silent shape as a short lookup table.
        r'\s*"([^"]*)"\s*,\s*(-?\d+|NULL)' % (FLOAT, FLOAT, FLOAT))
    for i, mm in enumerate(row.finditer(body)):
        flags = mm.group(5)
        pads.append({
            "index": i,
            "name": mm.group(4),
            "pos": [_f(mm.group(1)), _f(mm.group(2)), _f(mm.group(3))],
            "flags": None if flags == "NULL" else int(flags),
        })
    return pads


def parse_pad3d(text, stem):
    """pad3dlist entries: BoundPadRecord, { {pos}, {up}, {look}, "name", stan, {bbox} }.

    The second pad table, and the one the objective targets actually live in. A prop's pad field
    of 10000+ is not a bad pad index -- it selects THIS list, at index pad - 10000. Dam's four
    alarms carry 10070..10074, and dam3dlist has 95 entries, so they resolve here and nowhere
    else. Same leading layout as PadRecord, so the row pattern is shared; only the trailing
    field differs (a stan pointer instead of flags), which the caller does not use.
    """
    m = re.search(r"BoundPadRecord\s+%s\w*_pad3dlist\[\]\s*=\s*\{(.*?)\n\};" % re.escape(stem),
                  text, re.S)
    if not m:
        return []
    out = []
    row = re.compile(
        r"\{\s*\{\s*(%s)\s*,\s*(%s)\s*,\s*(%s)\s*\}\s*,"
        r"\s*\{[^}]*\}\s*,\s*\{[^}]*\}\s*,"
        r'\s*"([^"]*)"\s*,\s*(-?\d+|NULL)' % (FLOAT, FLOAT, FLOAT))
    for i, mm in enumerate(row.finditer(m.group(1))):
        out.append({
            "index": i,
            "name": mm.group(4),
            "pos": [_f(mm.group(1)), _f(mm.group(2)), _f(mm.group(3))],
        })
    return out


def parse_waypoints(text, stem):
    """pathwaypoints entries: { padnum, &path_table_N, 0, 0 }

    The pad number is what turns an abstract graph node into a world position, so a waypoint
    without a resolvable pad is useless to a bot and is reported rather than dropped."""
    m = re.search(r"waypoint\s+%s\w*_pathwaypoints\[\]\s*=\s*\{(.*?)\n\};" % re.escape(stem),
                  text, re.S)
    if not m:
        return []
    body = m.group(1)
    out = []
    for i, mm in enumerate(re.finditer(r"\{\s*(0x[0-9a-fA-F]+|-?\d+)\s*,", body)):
        tok = mm.group(1)
        pad = int(tok, 16) if tok.lower().startswith("0x") else int(tok)
        # Stop AT the -1 terminator. bondtypes.h documents pathwaypoints as a -1 terminated
        # array, and the terminator was being emitted as a real waypoint on EVERY level: Train
        # reported 105 nodes where the engine has 104, Dam 206 against 205, and so on. It survived
        # because it looks like a plain data hole -- one waypoint whose pad will not resolve --
        # rather than like an off-by-one, and every level having exactly one was read as a quirk
        # of the assets instead of the signature it is.
        #
        # Caught by the Mac build measuring 104 LIVE on Train against my 105 offline. Two independent
        # counts of the same thing disagreeing by exactly one, on every level, is the whole tell.
        #
        # Compared unsigned too: the field is read as u32 here, so -1 arrives as 4294967295.
        # Testing only for -1 would have left this in place on every level.
        if pad == -1 or pad == 0xFFFFFFFF:
            break
        out.append({"index": i, "pad": pad})
    return out


def _adjacency(text, stem, kind):
    """<stem>Z_<kind>_(\\d+)[] = { 3, 5, -1 } -- adjacency, -1 terminated."""
    edges = {}
    pat = re.compile(r"s32\s+%s\w*_%s_(\d+)\[\]\s*=\s*\{([^}]*)\}"
                     % (re.escape(stem), re.escape(kind)))
    for mm in pat.finditer(text):
        node = int(mm.group(1))
        vals = [int(v) for v in re.findall(r"-?\d+", mm.group(2))]
        edges[node] = [v for v in vals if v >= 0]     # drop the -1 terminator
    return edges


def parse_nav(text, stem):
    """THE navigation graph: one path_table_N per waypoint, listing that waypoint's neighbours.

    Not to be confused with path_neighbors_N, which is a much coarser REGION graph over
    waygroups -- Dam has 205 path_tables for its 206 waypoints but only 23 path_neighbors, and
    Statue has 198 waypoints against 4. Shipping the latter as "the navigation graph" produces a
    bot that can reach four places on a map with two hundred nodes, and the JSON looks fine."""
    return _adjacency(text, stem, "path_table")


def parse_waygroups(text, stem):
    """group id -> the waypoint ids belonging to it.

    This is the link between the two graphs, and without it they cannot be used together. The
    setup declares waygroups as

        waygroup <stem>Z_pathsets[] = { { &path_neighbors_N, &path_indeces_N, 0 }, ... };

    where bondtypes.h's `waygroup` says the first pointer is the neighbouring group ids and the
    second is the waypoint ids in this group. path_neighbors was already extracted as the region
    graph; path_indeces is the membership that says which waypoints those regions contain.
    """
    out = {}
    pat = re.compile(r"s32\s+%s\w*_path_indeces_(\d+)\[\]\s*=\s*\{([^}]*)\}" % re.escape(stem))
    for mm in pat.finditer(text):
        ids = [int(t) for t in re.findall(r"-?\d+", mm.group(2))]
        out[int(mm.group(1))] = [i for i in ids if i >= 0]     # the list is -1 terminated
    return out


def parse_regions(text, stem):
    """The coarse waygroup adjacency. Useful for high-level routing, useless for walking."""
    return _adjacency(text, stem, "path_neighbors")


def parse_props(text):
    """Every propDef that carries a pad, with the pad resolved.

    The layout is settled by the struct definitions, not by inference. bondtypes.h:2681
    ObjectRecord:

        inherits PropDefHeaderRecord;
        // obj (0x4) and pad (0x6) share one file word -- see GE_SUBWORD2.
        GE_SUBWORD2(s16 obj;, s16 pad;)

    and bondtypes.h GuardRecord does the same with chrnum/PadID. So the word after the header
    is _mkword(obj_or_chrnum, pad): **the pad is the LOW half**, on every record type that has
    one.

    Worth stating because guessing gets it backwards, and backwards is self-consistent enough
    to look right: Dam's 18 doors have 5 distinct high values and 18 distinct low values. Read
    the wrong way that is "18 door types at 5 positions"; read correctly it is 5 door types at
    18 positions, which is what a level actually contains.
    """
    out = []
    # The header word is now CAPTURED rather than skipped over. It was always being matched -- the
    # `[^\n]*?\)\)` is exactly `_mkword(extrascale, _mkshort(state, type))` -- and the value inside
    # is the one thing that made prop extents unusable.
    pat = re.compile(
        r"/\*\s*Type\s*=\s*(\w+)\s*;\s*index\s*=\s*(\d+)\s*\*/[^\n]*\n"
        r"(\s*[^\n]*?\)\))\s*,\s*_mkword\(\s*(\d+)\s*,\s*(\d+)\s*\)")
    # Extrascale IS the prop'S scale, and IT IS IN the setup file all along.
    #
    # propobj.c:78 records that sub_GAME_7F051F30 computes `scale = extrascale * (1/256)`, so 256
    # means 1.0. The model bounding boxes in assets/obseg/prop are MODEL space and are useless as
    # world lengths without it -- Train's median model box is hx=221 against a level only 111 units
    # wide, about ten times too large. This is the missing multiplier, and it is per PLACEMENT
    # rather than per model, which is why it has to come from here and not from the model file.
    #
    # Captured as None when the header is not in the _mkword(n, _mkshort(...)) form rather than
    # defaulting to 256. A record whose scale could not be read is NOT a record scaled by 1.0, and
    # writing 256 there would silently hand every unreadable prop a plausible wrong size -- the
    # same absent-is-not-zero rule the rest of this pipeline follows.
    hdr_pat = re.compile(r"\s*_mkword\(\s*(\d+)\s*,\s*_mkshort\(")
    for mm in pat.finditer(text):
        hm = hdr_pat.match(mm.group(3))
        out.append({
            "type": mm.group(1),
            "propdef": int(mm.group(2)),
            "obj": int(mm.group(4)),     # object preset id, or chrnum for a Guard
            "pad": int(mm.group(5)),
            "extrascale": int(hm.group(1)) if hm else None,
        })
    return out


def parse_tags(text):
    """tag id -> propdef index of the record the tag names.

    Objectives never name a prop directly; they name a TAG, and a Tag record binds that tag to
    a nearby prop by a SIGNED OFFSET from the Tag record's own propdef index:

        _mkword(tag_id, s16 offset)   ->  tagged propdef = tag_propdef + offset

    Proven on Dam rather than assumed. Tags 0,1,2,3 sit at propdefs 309,311,313,315, each with
    offset +1, resolving to 310,312,314,316 -- which are precisely Dam's four Alarm records, and
    Dam's first objective is four ObjectiveDestroyObject records naming tags 0,1,2,3
    ("Neutralize all alarms"). Negative offsets check out too: tag 15 at 277 with 0xfff8 lands
    on 269, a MultiMonitor.

    This is the missing link. With it an objective resolves to a prop, a prop to a pad, a pad to
    a position, and a position to a node on the navigation graph -- so a route can be solved to
    an objective instead of a bot wandering and hoping.
    """
    tags = {}
    pat = re.compile(
        r"/\*\s*Type\s*=\s*Tag\s*;\s*index\s*=\s*(\d+)\s*\*/\s*\n"
        r"\s*[^\n]*?\)\)\s*,\s*_mkword\(\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\)")
    for mm in pat.finditer(text):
        here = int(mm.group(1))
        tag  = int(mm.group(2))
        off  = int(mm.group(3), 0)
        if off >= 0x8000:
            off -= 0x10000
        tags[tag] = here + off
    return tags


def nearest_waypoint(pos, waypoints):
    """Closest waypoint by squared distance. The entry point to the graph for anything that
    knows where it is but not which node it is on -- an objective, a door, the player."""
    best, bestd = None, None
    for w in waypoints:
        p = w.get("pos")
        if p is None:
            continue
        d = (p[0]-pos[0])**2 + (p[1]-pos[1])**2 + (p[2]-pos[2])**2
        if bestd is None or d < bestd:
            best, bestd = w["index"], d
    return best, (bestd ** 0.5 if bestd is not None else None)


_SCHEMA_CACHE = None


def _array_body(text, stem, kind):
    """The body of one setup array, or None. propDefs and intro are separate arrays."""
    m = re.search(r"s32\s+%s\w*_%s\[\]\s*=\s*\{(.*?)\n\};" % (re.escape(stem), kind), text, re.S)
    return m.group(1) if m else None


def record_schemas():
    """{kind: {type_number: (name, size_in_words)}}, learned from the annotated setup files.

    Some multiplayer setups export their records as bare s32 hex words with no type comments --
    Dam, Runway and Frigate all do. They are the same records without the labels, so the layout
    is learned from the files that DO carry labels and then applied to the ones that do not.

    Derived rather than hand-written, and checked: across 34 annotated setups and several
    thousand records, every type has exactly one observed size. A hand-copied size table would
    be the same hazard as the hand-copied bank list that cost five levels their objective text.

    propDefs and intro are derived SEPARATELY because their type numbering is independent:
    intro type 0 is Spawn while propDefs type 0 is unused, and intro type 1 is StartWeapon
    against propDefs type 1 Door. Merging them would silently corrupt both.
    """
    global _SCHEMA_CACHE
    if _SCHEMA_CACHE is not None:
        return _SCHEMA_CACHE

    rec = re.compile(r"/\*\s*Type\s*=\s*(\w+)\s*;\s*index\s*=\s*(\d+)\s*\*/")
    hdr = re.compile(r"_mkword\(\s*[^,]+,\s*_mkshort\(\s*[^,]+,\s*(\d+)\s*\)\s*\)")
    out = {}
    paths = []
    for d in SETUP_DIRS:
        if os.path.isdir(d):
            paths += sorted(glob.glob(os.path.join(d, "*setup*.c")))

    for kind in ("propDefs", "intro"):
        table = {}
        for path in paths:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            m = re.search(r"s32\s+\w+_%s\[\]\s*=\s*\{(.*?)\n\};" % kind, text, re.S)
            if not m:
                continue
            body = m.group(1)
            marks = list(rec.finditer(body))
            for i, mm in enumerate(marks):
                start = mm.end()
                end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
                chunk = body[start:end]
                hm = hdr.search(chunk)
                if not hm:
                    continue
                depth = words = 0
                for ch in chunk:
                    if ch == "(":
                        depth += 1
                    elif ch == ")":
                        depth -= 1
                    elif ch == "," and depth == 0:
                        words += 1
                if i + 1 == len(marks):
                    words += 1        # the last record carries no trailing comma
                if words:
                    table.setdefault(int(hm.group(1)), (mm.group(1), words))
        out[kind] = table

    _SCHEMA_CACHE = out
    return out


# Three record types are laid out differently in the raw exports than in the annotated ones.
# These are measured, not tuned. Runway settles it on its own: its five Vehicle records sit at
# 1883, 1942, 2001, 2060 and 2119 -- a constant stride of 59 against the annotated files' 44 --
# and the fifth is followed at exactly +59 by the array terminator. Five records at a fixed
# stride landing on the terminator is not a coincidence a wrong number produces.
#
# Door and Collectable were found the same way, by asking where the next valid header actually
# begins rather than by trying sizes until something fit: Door 63 against a declared 64 over 28
# observations, Collectable 35 against 34 over 18, both unanimous.
#
# The confirmation is that nothing here was fitted to the pickups. With these three sizes, Dam,
# Frigate and Depot each independently produce sixteen AmmoBox, nine Collectable and two Armour
# -- the exact normalised loadout already verified from the annotated conversions. And the word
# arithmetic closes exactly in all four files, e.g. Dam 70*32 + 1*63 + 16*45 + 9*35 + 2*34 + 1
# terminator = 3407, plus one sentinel word = 3408, the array's true length.
RAW_SIZE_OVERRIDE = {"Door": 63, "Collectable": 35, "Vehicle": 59}


def raw_schema():
    """The propDefs schema as the raw exports lay it out."""
    return {t: (nm, RAW_SIZE_OVERRIDE.get(nm, sz))
            for t, (nm, sz) in record_schemas()["propDefs"].items()}


def parse_props_raw(text, stem):
    """Walk a raw hex propDefs array using the derived schema.

    Returns the same shape as parse_props so the caller cannot tell which export it got. The
    record header is _mkword(extrascale, _mkshort(state, type)), so the type is the low byte of
    word 0, and for the record types that carry one, word 1 is _mkword(obj, pad).
    """
    body = _array_body(text, stem, "propDefs")
    if body is None:
        return [], {}
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]{8})", body)]
    if not words:
        return [], {}

    schema = raw_schema()
    props, tags, i, idx, clean = [], {}, 0, 0, False
    while i < len(words):
        t = words[i] & 0xFF
        entry = schema.get(t)
        if entry is None:
            break                      # drifted: stop rather than walk off into garbage
        name, size = entry
        if name == "EndProps":
            # Landing on the terminator is not enough on its own -- a drifted walk can hit one
            # early. It has to land on the terminator AT THE END, with nothing after it but the
            # array's sentinel word. That is what makes a clean walk mean something.
            clean = len(words) - i <= 2
            break
        if size >= 2 and i + 1 < len(words):
            w1 = words[i + 1]
            hi, lo = (w1 >> 16) & 0xFFFF, w1 & 0xFFFF
            if name == "Tag":
                off = lo - 0x10000 if lo >= 0x8000 else lo
                tags[hi] = idx + off
            else:
                props.append({"type": name, "propdef": idx, "obj": hi, "pad": lo})
        i += size
        idx += 1

    # Only a walk that lands exactly on EndProps is trustworthy. Anything else drifted, and a
    # drifted walk does not fail loudly -- it reports a confident census of records that were
    # never there. Dam's arena decodes as seventy StandardProps and no pickups at all, which is
    # not a stage anyone could play; the true next header sits one word before where the walk
    # expected it, so a record size is off by one somewhere in this export.
    if not clean:
        return [], {}, False
    return props, tags, True


def parse_intro_census_raw(text, stem):
    """Census of a raw hex intro array, walked with the derived intro schema.

    This is where multiplayer spawn points live, so without it the raw-export arenas report no
    spawns at all -- which reads as "this arena has none" rather than "this parser cannot see
    them".
    """
    body = _array_body(text, stem, "intro")
    if body is None:
        return {}
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]{8})", body)]
    if not words:
        return {}
    schema = record_schemas()["intro"]
    census, i = {}, 0
    while i < len(words):
        entry = schema.get(words[i] & 0xFF)
        if entry is None:
            break
        name, size = entry
        census[name] = census.get(name, 0) + 1
        if name == "EndIntro":
            break
        i += size
    return census


def parse_intro_census(text, stem):
    """Counts from the INTRO array, which is where multiplayer spawns and start weapons live.

    Kept separate from the prop census because they are separate arrays with independent type
    numbering; counting them together reports Spawn and Door as if they came from one table.
    """
    body = _array_body(text, stem, "intro")
    if body is None:
        return {}
    census = {}
    for mm in re.finditer(r"/\*\s*Type\s*=\s*(\w+)\s*;", body):
        census[mm.group(1)] = census.get(mm.group(1), 0) + 1
    return census


def parse_prop_census(text, stem):
    """propDefs are macro-encoded, but each record is preceded by a comment naming its type:

        /* Type = WatchMenuObjectiveText; index = 0 */

    Counting by type is honest and useful -- it says what the level contains -- without
    pretending to decode a bit layout this script has not verified.

    Scoped to the propDefs array specifically. It used to scan the whole file, which swept in
    the INTRO array too -- so Spawn, StartWeapon, Cuff and EndIntro were reported as props. They
    are intro records, a separate array with its own independent type numbering (intro type 1 is
    StartWeapon where propDefs type 1 is Door), and merging the two describes neither."""
    body = _array_body(text, stem, "propDefs")
    if body is None:
        return {}
    census = {}
    for mm in re.finditer(r"/\*\s*Type\s*=\s*(\w+)\s*;", body):
        census[mm.group(1)] = census.get(mm.group(1), 0) + 1
    return dict(sorted(census.items(), key=lambda kv: (-kv[1], kv[0])))


def _apply_prop_extents(props, level):
    """Attach asset-space hx/hz/radius to every prop that has a model box and a readable scale.

    Silent no-op when build/levels/_prop_extents.json is absent: that file is produced by
    tools/gen_prop_extents.py, and a fresh tree runs this extractor first. Failing here would make
    the ordering a hard dependency for a field that is an enrichment, so the extents simply do not
    appear and every consumer already has to handle their absence.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    table = os.path.join(here, "..", "build", "levels", "_prop_extents.json")
    if not os.path.isfile(table):
        return 0
    try:
        by_obj = json.load(open(table, encoding="utf-8"))["by_obj"]
    except Exception:
        return 0

    try:
        from pack_world import load_level_scales
        ls = load_level_scales(os.path.join(here, "..")).get(level)
    except Exception:
        return 0
    if not ls:
        # No scale means no honest conversion to asset space. Emitting the model-space numbers
        # anyway would put values ~6.7x too large in front of a bot, so emit nothing.
        return 0

    n = 0
    for pr in props:
        e = by_obj.get(str(pr.get("obj")))
        es = pr.get("extrascale")
        if e is None or es is None or es <= 0:
            continue
        s = (es / 256.0) * ls
        pr["hx"] = round(e["hx"] * s, 2)
        pr["hz"] = round(e["hz"] * s, 2)
        pr["radius"] = round(e["radius"] * s, 2)
        n += 1
    return n


def build(name, stem, stage_id, mission, path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    pads = parse_pads(text, stem)
    waypoints = parse_waypoints(text, stem)
    nav     = parse_nav(text, stem)
    regions = parse_regions(text, stem)
    waygroups = parse_waygroups(text, stem)
    census = parse_prop_census(text, stem)
    props  = parse_props(text)
    tags   = parse_tags(text)

    # Dam, Runway and Frigate export their arena records as bare hex with no type comments, so
    # the comment-driven parsers find nothing. Fall back to walking the words with the derived
    # schema. Which export a file used is not otherwise observable, and a silently empty arena
    # is exactly the failure this whole file keeps running into.
    raw_used = False
    if not props:
        rprops, rtags, rclean = parse_props_raw(text, stem)
        if rclean and (rprops or rtags):
            props, tags, raw_used = rprops, rtags or tags, True

    intro_census = parse_intro_census(text, stem)
    if raw_used:
        # Both censuses come from comments, which a raw export does not have. Rebuild the prop
        # census from the records actually walked, and the intro census from its own walker.
        census = {}
        for pr in props:
            census[pr["type"]] = census.get(pr["type"], 0) + 1
        census = dict(sorted(census.items(), key=lambda kv: (-kv[1], kv[0])))
        intro_census = parse_intro_census_raw(text, stem)
    pad3d  = parse_pad3d(text, stem)

    # Resolve each waypoint to a world position through its pad, which is what makes the graph
    # navigable. Unresolvable ones are counted, not silently dropped: a graph that is quietly
    # missing nodes produces a bot that quietly cannot reach places.
    unresolved = 0
    for wp in waypoints:
        p = wp["pad"]
        if 0 <= p < len(pads):
            wp["pos"] = pads[p]["pos"]
            wp["pad_name"] = pads[p]["name"]
        else:
            unresolved += 1

    # Give every prop a position and a graph node. The node is what a route can actually be
    # solved to -- a bare XYZ is not reachable, a waypoint is. Props whose pad is out of range
    # keep their record and lose their position, so they stay visible as data rather than
    # disappearing into a smaller count.
    # The pad field is s16 and overloaded, per its own comment in bondtypes.h: "0000+ or 2710+
    # (10,000+) to use standard presets. -1 to -256 to set this object inside the previous
    # object." So a prop with no pad position is usually not a parse failure -- it is either
    # preset-placed or mounted inside the prop before it. Both are worth knowing: "inside the
    # previous object" is the crate case, where the way to the item is to destroy its container,
    # and a bot that treats it as unreachable will stand in front of a crate forever.
    props_located = 0
    prev_positioned = None
    for pr in props:
        raw = pr["pad"]
        p = raw - 0x10000 if raw >= 0x8000 else raw   # the field is signed
        pr["pad"] = p
        if p < 0:
            pr["placement"] = "inside_previous"
            pr["contained_by"] = prev_positioned
        elif p >= 10000:
            pr["placement"] = "preset"
        else:
            pr["placement"] = "pad"

        # Preset placement is a real position, just in the other table.
        if pr["placement"] == "preset":
            k = p - 10000
            if 0 <= k < len(pad3d):
                pr["pos"] = pad3d[k]["pos"]
                pr["pad_name"] = pad3d[k]["name"]
                pr["pad3d"] = k
                node, dist = nearest_waypoint(pr["pos"], waypoints)
                if node is not None:
                    pr["nav_node"] = node
                    pr["nav_dist"] = round(dist, 1)
                props_located += 1
                prev_positioned = pr["propdef"]
            continue

        if 0 <= p < len(pads):
            pr["pos"] = pads[p]["pos"]
            pr["pad_name"] = pads[p]["name"]
            node, dist = nearest_waypoint(pr["pos"], waypoints)
            if node is not None:
                pr["nav_node"] = node
                pr["nav_dist"] = round(dist, 1)
            props_located += 1
            prev_positioned = pr["propdef"]

    # ---- prop extents (S1) --------------------------------------------------------------
    #
    # A position is a POINT and the world is made of solids: "crate 278 away" is 278 to its CENTRE,
    # and a bot that still sees room has already walked into the corner. These three numbers are
    # what let a reader turn a distance into a surface.
    #
    # Three multiplications, and all three are needed:
    #
    #   model box     assets/obseg/prop/<name>/Model.c, a ModelRoData_BoundingBoxRecord. MODEL
    #                 space -- Train's median is hx=221 against a carriage 111 units wide.
    #   extrascale    the per-PLACEMENT scale from this level's own setup data. propobj.c:78 --
    #                 `scale = extrascale * (1/256)`. The same hatchbolt model is 600 units as a
    #                 Collectable and 28 as a StandardProp; without this they would be identical.
    #   levelscale    model boxes are runtime-proportioned and this file is ASSET space.
    #
    # Emitted IN asset space, like every other length here. pack_world.py applies
    # runtime = asset / levelscale to every position it packs, and these ride the same conversion.
    # Emitting runtime lengths beside asset positions is the "half in one space" failure its own
    # comment warns about.
    #
    # hx/hz ARE UNROTATED half-extents in the model's own frame; `radius` is the XZ circumradius
    # and is the only one safe to use without knowing the prop's orientation. Both are emitted and
    # named rather than silently picking one, because a long crate at 45 degrees occupies more
    # width than its half-extent suggests.
    #
    # A prop with no model box or no readable extrascale gets NO extent fields at all rather than
    # zeros: absent and zero-sized lead to opposite behaviour, and only one of them is true.
    _apply_prop_extents(props, name)

    by_propdef = {pr["propdef"]: pr for pr in props}
    tagged = 0
    for tag, pd in tags.items():
        pr = by_propdef.get(pd)
        if pr is not None:
            pr["tag"] = tag
            tagged += 1

    nav_edges = sum(len(v) for v in nav.values())
    reg_edges = sum(len(v) for v in regions.values())
    return {
        "level": name,
        "stage_id": stage_id,
        "mission": mission,
        "setup": os.path.basename(path),
        "counts": {
            "pads": len(pads),
            "waypoints": len(waypoints),
            "nav_nodes": len(nav),
            "nav_edges": nav_edges,
            "region_nodes": len(regions),
            "region_edges": reg_edges,
            "waypoints_without_pad": unresolved,
            "prop_types": len(census),
            "props": len(props),
            "props_located": props_located,
            "tags": len(tags),
            "tags_resolved": tagged,
        },
        # "unreadable" means the file exports raw records that could not be walked cleanly, so
        # this stage has reliable pads and geometry but NO prop or pickup census. Stated rather
        # than left as a zero, because a zero reads as "this arena has no armour".
        "prop_export": ("raw" if raw_used
                        else "annotated" if props
                        else "unreadable"),
        "intro_census": intro_census,
        "pads": pads,
        "waypoints": waypoints,
        "graph": {str(k): v for k, v in sorted(nav.items())},
        "region_graph": {str(k): v for k, v in sorted(regions.items())},
        "waygroups": {str(k): v for k, v in sorted(waygroups.items())},
        "prop_census": census,
        "props": props,
        "tags": {str(k): v for k, v in sorted(tags.items())},
    }


def find_setup(stem):
    """Region overrides in setup/u win over the shared copy in setup/. Confirmed against what is
    actually on disk rather than trusted from the table, so a missing level is reported as
    missing instead of silently producing an empty graph."""
    for d in SETUP_DIRS:
        exact = os.path.join(d, stem + "Z.c")
        if os.path.exists(exact):
            return exact
    return None


# Depot is the one stage whose arena stem is not derivable: the campaign file is Usetupdepo and
# the arena file is Ump_setupdepot, with a t the campaign name does not have. Without this the
# lookup misses and Depot silently has no arena data at all -- which is how it came to be the
# one arena stage with no nuance written for it.
MP_STEM_OVERRIDE = {
    "Usetupdepo": "Ump_setupdepot",
}


def mp_stem(stem):
    """The multiplayer variant of a setup stem, or None if it is already one.

    prop.c:1507 synthesises the name by inserting "mp_" after the leading U once
    getPlayerCount() is two or more, so Usetupark becomes Ump_setupark. Multiplayer is a
    function of player count rather than a separate stage, which is why the arenas and the
    campaign missions share a naming scheme at all.
    """
    if stem.startswith("Ump_") or not stem.startswith("U"):
        return None
    return MP_STEM_OVERRIDE.get(stem, "Ump_" + stem[1:])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels"),
                    help="directory to write JSON into (default build/levels)")
    ap.add_argument("--level", help="just this one, by name")
    ap.add_argument("--list", action="store_true", help="list known levels and exit")
    args = ap.parse_args()

    if args.list:
        for n, (stem, sid, mis) in sorted(LEVELS.items(), key=lambda kv: kv[1][2]):
            print("%-10s stage=%-3d mission=%-2d setup=%s" % (n, sid, mis, stem))
        return 0

    if not any(os.path.isdir(d) for d in SETUP_DIRS):
        print("error: setup assets not found under %s" % SETUP_DIRS[-1], file=sys.stderr)
        print("       vendor/ is gitignored; generate the assets on this machine first.",
              file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    wanted = {args.level: LEVELS[args.level]} if args.level else LEVELS
    if args.level and args.level not in LEVELS:
        print("error: unknown level %r (try --list)" % args.level, file=sys.stderr)
        return 2

    index, missing = [], []
    for name, (stem, sid, mis) in sorted(wanted.items(), key=lambda kv: kv[1][2]):
        path = find_setup(stem)
        if path is None:
            missing.append(name)
            continue
        doc = build(name, stem, sid, mis, path)
        with open(os.path.join(args.out, name + ".json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)
        c = doc["counts"]
        print("%-10s pads=%-4d waypoints=%-4d nav=%-4d/%-5d proptypes=%-3d %s"
              % (name, c["pads"], c["waypoints"], c["nav_nodes"], c["nav_edges"], c["prop_types"],
                 "" if not c["waypoints_without_pad"]
                 else "(%d waypoints without a pad)" % c["waypoints_without_pad"]))
        index.append({k: doc[k] for k in ("level", "stage_id", "mission", "setup", "counts")})

        # Most campaign missions are ALSO multiplayer arenas, and their arena setup is a
        # different file with different pickups, spawns and props. Emitting it separately
        # matters: multiplayer Facility has its own armour and ammunition layout, and reading
        # those counts off the campaign file would describe a stage nobody plays.
        mstem = mp_stem(stem)
        mpath = find_setup(mstem) if mstem else None
        if mpath is not None:
            mdoc = build(name, mstem, sid, 0, mpath)
            mdoc["arena"] = True
            with open(os.path.join(args.out, name + ".mp.json"), "w", encoding="utf-8") as fh:
                json.dump(mdoc, fh, indent=1)
            mc = mdoc["counts"]
            print("%-10s   [mp] pads=%-4d props=%-4d proptypes=%-3d"
                  % ("", mc["pads"], mc["props"], mc["prop_types"]))
            index.append({k: mdoc[k] for k in
                          ("level", "stage_id", "mission", "setup", "counts")} | {"arena": True})

    with open(os.path.join(args.out, "index.json"), "w", encoding="utf-8") as fh:
        json.dump({"levels": index}, fh, indent=1)

    print("\nwrote %d level(s) to %s" % (len(index), args.out))
    if missing:
        # Named, not silent. A missing level here is a wrong stem in the table above, and it
        # would otherwise show up much later as "the bot cannot navigate that map".
        print("NOT FOUND (setup stem probably wrong in LEVELS): %s" % ", ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
