#!/usr/bin/env python3
"""Deep mine: the ENTITIES, RELATIONS, COUNTS and CONDITIONS stated in the level documents.

The earlier passes took the documents' skeleton -- JSON blocks, then sections, labels, attributes
and bullets. That made 805,474 characters addressable but left the substance in prose. This takes
the substance.

WHAT IT PULLS, and why each is worth having:

  ENTITIES   the things a level contains, by kind: rooms and areas, doors and hatches, objects and
             machinery, weapons, enemies. A bot planning on a level needs a noun list before it
             needs anything else.
  RELATIONS  "X is north of Y", "A connects to B", "the door leads to the bridge". Direction and
             connection are what turn a noun list into a map, and they are the one thing our
             extracted geometry CANNOT provide -- the game data has positions, not intent.
  COUNTS     "three guards", "two brake units". Checkable against the setup files directly, which
             makes them the most valuable claims in the whole corpus.
  CONDITIONS "if the alarm sounds", "once the hatch opens", "until the guard turns" -- the
             triggers a level's logic actually turns on.

EVERY RECORD KEEPS ITS SOURCE LINE, and every record is a CLAIM. The documents' distances are
already known wrong by 1.46x (gen_level_facts.py), so a count or a relation from them is a lead to
verify, not a fact to act on. The counts in particular can be checked against the setup data, and
that is the point of extracting them separately.

AND IT EXTRACTS DATA, NOT PROSE. A record is a noun, a kind, a relation type and a line number.
Sentences stay in the source document; this makes them findable.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Kind vocabulary, drawn from the terms the documents themselves use most (see the label census in
# gen_level_knowledge_deep.py: corridor 67, bunker 22, tunnel 21, warehouse 20, chamber 19...).
KINDS = {
    "area":   r"room|corridor|hallway|chamber|tunnel|warehouse|bunker|vault|silo|hangar|bridge|"
              r"catwalk|stairwell|staircase|balcony|platform|courtyard|cavern|dock|lab|office|"
              r"control room|server room|cargo bay|engine room|car\b|carriage",
    "portal": r"door|doorway|hatch|gate|airlock|elevator|lift|ladder|stairs|grate|vent|window",
    "object": r"crate|barrel|console|terminal|computer|desk|table|cabinet|locker|pipe|tank|"
              r"generator|brake unit|brake|panel|switch|lever|alarm|camera|monitor|safe",
    "weapon": r"pp7|kf7|ak-?47|rc-?p90|zmg|d5k|dd44|klobb|shotgun|sniper|rifle|grenade|mine|"
              r"rocket|launcher|magnum|silenced|throwing knife|taser",
    "enemy":  r"guard|soldier|scientist|technician|sniper|commando|janus|siberian|russian|"
              r"boris|ourumov|xenia|trevelyan|mishkin|natalya",
}
KIND_RE = {k: re.compile(r"\b(%s)\b" % v, re.I) for k, v in KINDS.items()}

DIRECTION = re.compile(
    r"\b(north|south|east|west|left|right|ahead|behind|above|below|upstairs|downstairs|"
    r"forward|backward|opposite|beyond|past|through|beside|adjacent)\b", re.I)
# Broadened after measurement: the original list matched 6 lines in 43,731. Real documents say
# "runs into", "gives access to", "emerges", "drops into" far more often than the formal
# "connects to" a schema author imagines.
CONNECT = re.compile(
    r"\b(connects? to|leads? to|opens? (?:on)?to|joins?|links? to|exits? (?:in)?to|"
    r"takes you to|brings you to|runs? (?:in)?to|gives? access to|emerges? (?:in)?to|"
    r"drops? (?:down )?(?:in)?to|feeds? (?:in)?to|continues? (?:in)?to|returns? to|"
    r"accessible from|reached from|reachable from|via)\b", re.I)

# Filler that would otherwise become a relation operand.
STOP = set("""a an the of to in on at for with and or is are was were be been it its this that
these those you your they them from by as if then than so but not no do does did can will would
should could may might must have has had there here when where which who what about into over
under up down out off just only very more most some any each also both such only very""".split())
COUNT = re.compile(
    r"\b(one|two|three|four|five|six|seven|eight|nine|ten|\d{1,3})\s+"
    r"([a-z][a-z-]{2,20}(?:s|es)?)\b", re.I)
CONDITION = re.compile(
    r"\b(if|once|when|until|unless|after|before|as soon as|whenever)\b\s+([a-z][^.;]{6,70})", re.I)

WORDNUM = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
           "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10}

# Counting "the guards" or "some doors" is noise; only concrete quantities are useful, and only
# when the noun is one we recognise as a thing a level contains.
COUNTABLE = re.compile(r"guard|door|crate|room|car|enemy|enemies|barrel|console|hatch|brake|"
                       r"objective|key|camera|alarm|tank|floor|level|stair", re.I)


def mine_text(text):
    ent = collections.Counter()
    ent_kind = {}
    ent_line = {}
    relations, counts, conditions = [], [], []

    for i, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or len(s) > 400:
            continue
        low = s.lower()

        for kind, rx in KIND_RE.items():
            for m in rx.finditer(s):
                t = m.group(1).lower()
                ent[t] += 1
                ent_kind[t] = kind
                ent_line.setdefault(t, i)

        # relations must not require MY vocabulary. The first version demanded a direction word
        # AND two nouns from the KINDS list, and kept 20 relations out of 43,731 lines. Measured:
        # 724 lines carry a direction word, but only 102 contain even ONE noun I had listed. The
        # documents describe these levels in their own words; a miner that only sees terms it
        # already knew is measuring its own word list.
        #
        # So the anchor is the DIRECTION or CONNECTIVE, and the operands are whatever substantial
        # words sit either side of it. Recognised kinds are still tagged when present, because a
        # typed operand is worth more -- but an untyped one is kept rather than discarded.
        d = DIRECTION.search(s)
        c = CONNECT.search(s)
        if d or c:
            m = c or d
            before = [w for w in re.findall(r"[A-Za-z][A-Za-z0-9'-]{2,}", s[: m.start()])
                      if w.lower() not in STOP][-3:]
            after = [w for w in re.findall(r"[A-Za-z][A-Za-z0-9'-]{2,}", s[m.end():])
                     if w.lower() not in STOP][:3]
            if before or after:
                typed = []
                for kind, rx in KIND_RE.items():
                    for mm in rx.finditer(s):
                        typed.append({"term": mm.group(1).lower(), "kind": kind})
                relations.append({
                    "kind": "connect" if c else "direction",
                    "term": m.group(1).lower(),
                    "from": [w.lower() for w in before],
                    "to": [w.lower() for w in after],
                    "typed": typed[:4],
                    "line": i,
                })

        for m in COUNT.finditer(s):
            raw, noun = m.group(1).lower(), m.group(2).lower()
            if not COUNTABLE.search(noun):
                continue
            n = WORDNUM.get(raw)
            if n is None:
                try:
                    n = int(raw)
                except ValueError:
                    continue
            if 1 <= n <= 200:
                counts.append({"n": n, "of": noun, "line": i})

        m = CONDITION.search(low)
        if m:
            conditions.append({"trigger": m.group(1), "clause": m.group(2).strip()[:70], "line": i})

    entities = [{"name": t, "kind": ent_kind[t], "count": c, "line": ent_line[t]}
                for t, c in ent.most_common()]

    # and what the documents name that I did not think TO list. The kinds vocabulary was written
    # by guessing which nouns a GoldenEye level guide would use, and a miner limited to it reports
    # its author's imagination rather than the source. Same failure the relations had.
    #
    # So: multi-word Capitalised phrases (the way documents name specific places -- "Control
    # Room", "Cargo Bay") that recur, and are not already typed. Kept as kind="untyped" rather
    # than guessed at, because a wrong type is worse than none: a reader can see "untyped" and
    # look, where a confidently mislabelled "area" would be believed.
    phrase = collections.Counter()
    phrase_line = {}
    for i, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or len(s) > 400 or s.isupper():
            continue          # all-caps lines are labels; gen_level_knowledge_deep.py has them
        for m in re.finditer(r"\b([A-Z][a-z]{2,}(?:\s+[A-Z][a-z]{2,}){1,3})\b", s):
            t = m.group(1).lower()
            if t.split()[0] in STOP:
                continue
            phrase[t] += 1
            phrase_line.setdefault(t, i)
    known = {e["name"] for e in entities}
    for t, c in phrase.most_common():
        # Twice or more: a phrase used once is usually a sentence opening, not a named thing.
        if c >= 2 and t not in known:
            entities.append({"name": t, "kind": "untyped", "count": c, "line": phrase_line[t]})

    return entities, relations, counts, conditions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "build", "levels"))
    a = ap.parse_args()

    by_level, tot = {}, collections.Counter()
    for f in sorted(glob.glob(os.path.join(a.levels, "*.walkthrough.json"))):
        lv = os.path.basename(f)[: -len(".walkthrough.json")]
        d = json.load(open(f, encoding="utf-8"))
        E, R, C, K = [], [], [], []
        for doc in d.get("documents", []):
            e, r, c, k = mine_text(doc.get("text", ""))
            src = os.path.basename(doc.get("source", "?"))
            for x in (r, c, k):
                for rec in x:
                    rec["source"] = src
            E += e; R += r; C += c; K += k
        if not (E or R or C or K):
            continue
        by_level[lv] = {"entities": E, "relations": R, "counts": C, "conditions": K}
        tot["entities"] += len(E); tot["relations"] += len(R)
        tot["counts"] += len(C); tot["conditions"] += len(K)

    dest = os.path.join(a.levels, "_entities.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump({"note": "CLAIMS mined from the level documents; verify before acting",
                   "levels": len(by_level), "totals": dict(tot), "by_level": by_level},
                  fh, indent=1, ensure_ascii=False)

    print("%d level(s) -> %s" % (len(by_level), dest))
    for k in ("entities", "relations", "counts", "conditions"):
        print("   %-11s %d" % (k, tot[k]))

    kinds = collections.Counter()
    for lv, r in by_level.items():
        for e in r["entities"]:
            kinds[e["kind"]] += e["count"]
    print("\nentity mentions by kind:")
    for k, n in kinds.most_common():
        print("   %-8s %d" % (k, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
