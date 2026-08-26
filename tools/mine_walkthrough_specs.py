#!/usr/bin/env python3
"""Extract the JSON level specifications embedded in the walkthroughs.

The documents contain 168 structured blocks across all 28 distinct files -- the author writing a
level out as machine-readable fields rather than prose, usually introduced by a line about how they
would describe the level to a level-generation API. Archives declares environment type, primary
axis, verticality and space composition; Train declares a linear topology. This is the corpus
stating its own model in the form a program can consume, and neither the structural miner nor the
claims miner touched any of it: the structural miner sees lines that are not headings, drawings or
enumerations, and the claims miner sees sentences that are not sentences.

It is also the most trustworthy material in the corpus. Prose has to be pattern-matched and can be
misread; a field named primary_axis with the value horizontal means one thing.

PARSING IS ATTEMPTED, NOT ASSUMED, AND FAILURES ARE COUNTED. These blocks are handwritten inside
prose, so some carry trailing commas, comments, ellipses standing in for omitted content, or prose
interleaved between fields. A lenient repair pass fixes the mechanical cases -- trailing commas
before a closing brace, // and # comments, smart quotes. Anything still unparseable is REPORTED as
unparseable with its file and line rather than silently dropped, because a spec that fails to parse
is a thing to go and look at, not a thing to pretend does not exist.

THE REPAIR PASS NEVER GUESSES AT CONTENT. It removes syntax the author did not intend as data
(comments, trailing commas) and nothing else. It does not fill in an ellipsis, close an unclosed
brace or invent a missing value, because a spec that parses because the tool completed it is worse
than one that does not parse: the invented part is indistinguishable from the authored part.

Attribution comes from the attribution audit, so a misfiled file's specs land on the level its text
actually describes. Byte-identical copies are read once.

Author material stays out of git. Output goes to build/ only.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from mine_walkthroughs_deep import load_attribution

# Syntax the author did not intend as data. Nothing here alters a value.
TRAILING_COMMA = re.compile(r",(\s*[}\]])")
LINE_COMMENT = re.compile(r"^\s*(?://|#)\s.*$", re.M)
SMART = {"“": '"', "”": '"', "‘": "'", "’": "'"}


def blocks(lines):
    """Every brace-balanced run in the file, with its start line."""
    out, depth, start = [], 0, None
    for i, l in enumerate(lines):
        s = l.strip()
        if depth == 0:
            if s.startswith("{"):
                depth, start = 1, i
                # A one-line block opens and closes on the same line.
                depth += s.count("{") - 1 - s.count("}")
                if depth <= 0:
                    out.append((start, lines[start:i + 1]))
                    depth = 0
            continue
        depth += s.count("{") - s.count("}")
        if depth <= 0:
            out.append((start, lines[start:i + 1]))
            depth, start = 0, None
    return out


def repair(text):
    """Mechanical fixes only. See the module note on why this never guesses at content."""
    for a, b in SMART.items():
        text = text.replace(a, b)
    text = LINE_COMMENT.sub("", text)
    text = TRAILING_COMMA.sub(r"\1", text)
    return text


# A block of bare identifiers with no colons is not broken JSON, it is the author sketching a
# SCHEMA -- the fields a message or record carries, named without values. 37 of the 43 blocks that
# failed to parse are this. Repairing them into JSON would invent values the author never wrote;
# discarding them would lose a field list that says exactly what a structure contains.
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{1,40}$")

# A value written as a bare placeholder: "position": [x,y,z]. The SHAPE is authored -- position is
# a three-vector -- and the value is not. Substituted with null so the structure
# parses, and every substituted path is recorded, so nobody can mistake the null for something the
# author wrote.
PLACEHOLDER_LIST = re.compile(r":[ ]*\[[ ]*[a-z](?:[ ]*,[ ]*[a-z])*[ ]*\]")


def schema_fields(blk):
    """Bare field names in a block, or None when the block is not a field list."""
    names, other = [], 0
    for l in blk[1:-1] if len(blk) > 2 else []:
        s = l.strip().rstrip(",")
        if not s:
            continue
        if IDENT.match(s):
            names.append(s)
        else:
            other += 1
    # Mostly identifiers, and enough of them to be a list rather than a stray word.
    if len(names) >= 3 and other <= max(1, len(names) // 4):
        return names
    return None


def flatten(obj, prefix=""):
    """Spec fields as dotted paths, so they can be compared across levels."""
    out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.update(flatten(v, "%s.%s" % (prefix, k) if prefix else str(k)))
    elif isinstance(obj, list):
        # Lists of scalars are values; lists of objects are indexed so nothing collides.
        if all(not isinstance(x, (dict, list)) for x in obj):
            out[prefix] = obj
        else:
            for n, x in enumerate(obj):
                out.update(flatten(x, "%s[%d]" % (prefix, n)))
    else:
        out[prefix] = obj
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "walkthroughs"))
    ap.add_argument("--attribution", default=os.path.join(ROOT, "build", "levels",
                                                          "_walkthrough.attribution.json"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "levels",
                                                  "_walkthrough.specs.json"))
    ap.add_argument("--show-failures", type=int, default=6)
    a = ap.parse_args()

    usable, _ = load_attribution(a.attribution)
    seen, specs, failures = set(), [], []

    for f in sorted(os.listdir(a.dir)):
        if not f.lower().endswith(".txt"):
            continue
        p = os.path.join(a.dir, f)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        if f.lower().startswith("goldeneye64"):
            level = "_engine"
        elif f in usable:
            level = usable[f]
        else:
            continue

        lines = open(p, "rb").read().decode("utf-8", errors="replace").splitlines()
        for start, blk in blocks(lines):
            raw = "\n".join(blk)
            try:
                obj = json.loads(repair(raw))
            except Exception as exc:
                # A block that will not parse is usually not broken JSON. It is one of two other
                # things the author wrote deliberately, and both are worth keeping as themselves
                # rather than being forced into JSON or thrown away.
                fields = schema_fields(blk)
                if fields:
                    # Bare field names, no values: a schema sketch of what a record carries.
                    specs.append({"level": level, "source": f, "line": start + 1,
                                  "rows": len(blk), "kind": "schema", "fields": len(fields),
                                  "field_names": fields})
                    continue
                if PLACEHOLDER_LIST.search(raw):
                    # Values written as placeholders, e.g. a position given as [x,y,z]. The shape
                    # is authored and the value is not, so the placeholders become
                    # null and every path that was substituted is recorded. Without that record a
                    # reader could not tell a substituted null from one the author wrote.
                    patched = PLACEHOLDER_LIST.sub(": null", raw)
                    try:
                        obj2 = json.loads(repair(patched))
                    except Exception:
                        obj2 = None
                    if isinstance(obj2, dict):
                        flat2 = flatten(obj2)
                        holes = [k for k, v in flat2.items() if v is None]
                        specs.append({"level": level, "source": f, "line": start + 1,
                                      "rows": len(blk), "kind": "spec_with_placeholders",
                                      "fields": len(flat2), "spec": obj2, "flat": flat2,
                                      "placeholder_paths": holes})
                        continue
                failures.append({"level": level, "source": f, "line": start + 1,
                                 "rows": len(blk), "error": str(exc)[:90],
                                 "head": blk[0].strip()[:70]})
                continue
            if not isinstance(obj, dict):
                continue
            flat = flatten(obj)
            specs.append({"level": level, "source": f, "line": start + 1,
                          "rows": len(blk), "fields": len(flat),
                          "spec": obj, "flat": flat})

    total = len(specs) + len(failures)
    print("%d structured blocks: %d parsed, %d did not" % (total, len(specs), len(failures)))
    if total:
        print("   %.0f%% parsed" % (100.0 * len(specs) / total))

    by_level = collections.Counter(x["level"] for x in specs)
    print("\nparsed specs by level:")
    for lv, n in sorted(by_level.items(), key=lambda x: -x[1]):
        fields = sum(s.get("fields", 0) for s in specs if s["level"] == lv)
        print("   %-10s %3d spec(s), %4d fields" % (lv, n, fields))

    # The field vocabulary is the interesting part: fields shared across many levels are the
    # author's model of what a GoldenEye level has, stated the same way every time.
    # Schema sketches carry field_names and no flat map; JSON specs carry flat and no field_names.
    # Both contribute field names to the vocabulary, so both are read, each by its own key.
    keys = collections.Counter()
    for s in specs:
        for k in s.get("flat", {}):
            keys[k.split("[")[0]] += 1
        for k in s.get("field_names", []):
            keys[k] += 1
    print("\nmost common spec fields (the author's level model):")
    for k, n in keys.most_common(18):
        print("   %-42s %3d" % (k, n))

    if failures:
        print("\nUNPARSEABLE, reported rather than dropped:")
        for x in failures[:a.show_failures]:
            print("   %-34s line %-5d %-28s %s"
                  % (x["source"], x["line"], x["head"][:28], x["error"][:44]))
        if len(failures) > a.show_failures:
            print("   ... and %d more" % (len(failures) - a.show_failures))

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump({"parsed": len(specs), "unparsed": len(failures),
                   "by_level": dict(by_level),
                   "field_frequency": dict(keys.most_common()),
                   "specs": specs, "failures": failures}, fh, indent=1)
    print("\n-> %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
