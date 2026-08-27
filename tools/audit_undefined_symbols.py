#!/usr/bin/env python3
"""W15's first slice: which declared symbols have no definition anywhere in the tree.

Motivating incident, not a hypothetical: this exact defect shape has hit three times in one day.
gePortCliFrame was defined and never called (a hook gap, caught at runtime, the hard way).
gePortTileAt and gePortPathClearParts were called and never defined (a link failure, caught only
because a full rebuild happened to be attempted). gePortTargetState hit again in a rebuild later
the same day. Every one of these was an `extern` declaration nobody checked against the
rest of the tree, because nothing DOES that check -- until now, it was "does the build fail",
which only happens when someone rebuilds everything, which this project's own build times
discourage doing constantly.

WHAT THIS IS NOT. A real answer needs a real parser -- clang's AST, which this machine does not
have (`which clang` finds nothing; the Windows build is MinGW GCC only). This is a text scan, and
the first version of it was WRONG in a way worth recording rather than quietly fixing: matching a
parameter list as `[^;{}]*` looked reasonable and was not, because C parameter lists can legally
touch other parens, and semicolon-less macro invocations (ge_ruleset.c's GE_RS_GET, nine calls
in a row with no `;` between them) let that class greedily bridge across unrelated constructs --
it swallowed nine macro calls AND the real function after them into one bogus match, and the real
function's own definition vanished from the results because finditer never looked at its span
again. Caught by validating against a KNOWN true positive AND a known-defined name, not by reading
the regex and believing it. Fixed by manually tracking paren depth per call site instead of
trusting a character class to stay inside one signature.

WHAT THIS STILL WILL MISS. Macro-generated function bodies (the GE_RS_GET case: a real match, but
the definition line this script sees is the macro CALL, not a body it can find) -- these move to
their own bucket by heuristic, not confirmed. Definitions that do not start their return type at
column 0 (every definition read across this whole project this session did, but a style is not a
guarantee). Anything behind an #ifdef this scan's file set does not also satisfy. None of that is
a rounding error; the fuller W15 -- call graph, patch provenance, reachability -- wants clang's
AST and a real design pass, not this script grown sideways to cover every case by hand.

SCOPE: gePort*-prefixed names are reported first and separately, because that prefix IS the
project's own declared boundary -- every real incident above was a gePort* symbol.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")

# extern declarations: the codebase's own established convention for a port-layer boundary
# crossing (`extern int gePortNavCount(void);`), local or header-level. Anchored on the literal
# `extern` keyword -- a strong, low-false-positive signal -- rather than trying to recognise an
# arbitrary return type.
EXTERN_SIG = re.compile(r"\bextern\s+[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\(")

# definitions: TYPE NAME( at the start of a line (column 0, this project's own near-universal
# style for a function definition, checked across every file read this session).
#
# No exclusion for a leading `extern` here -- there was one, and it was wrong. `extern "C" int
# gePortLauncherRun(...) { ... }` is a real definition (the C++ linkage-specification idiom this
# tree uses throughout its .cpp files), and rejecting anything starting with the word `extern`
# rejected that too, which is exactly how gePortLauncherRun -- defined and called constantly --
# turned up on a "missing" list. The next_significant_char check below is what actually tells a
# definition from a declaration (`{` vs `;`), so it does not need help from this pattern also
# trying to guess, and guessing was the bug.
DEFN_SIG = re.compile(r"^[A-Za-z_][\w\s\*\"]*?\b([A-Za-z_]\w*)\s*\(", re.M)


def strip_comments(text):
    text = BLOCK_COMMENT.sub(" ", text)
    text = LINE_COMMENT.sub(" ", text)
    return text


def skip_balanced_parens(text, open_paren_index):
    """Given the index of a '(', return the index just after its matching ')'.

    Manual depth tracking, not a regex character class -- the whole reason this file exists is
    that the character-class version bridges across unrelated calls when nothing but whitespace
    separates them (no ';', no '{'). This cannot make that mistake: it only ever closes on ITS
    OWN opening paren, however much or little separates it from the next thing in the file.
    """
    depth = 0
    i = open_paren_index
    n = len(text)
    while i < n:
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n  # unbalanced -- truncated file or a paren inside a string this scan didn't strip


def next_significant_char(text, i):
    n = len(text)
    while i < n and text[i] in " \t\r\n":
        i += 1
    return text[i] if i < n else ""


def scan(paths):
    declared = {}   # name -> list of (file, line)
    defined = set()
    texts = []
    for p in paths:
        try:
            raw = open(p, "rb").read().decode("utf-8", errors="replace")
        except OSError:
            continue
        text = strip_comments(raw)
        texts.append(text)

        for m in EXTERN_SIG.finditer(text):
            name = m.group(1)
            close = skip_balanced_parens(text, m.end() - 1)
            if next_significant_char(text, close) == ";":
                line = text.count("\n", 0, m.start()) + 1
                declared.setdefault(name, []).append((p, line))

        for m in DEFN_SIG.finditer(text):
            name = m.group(1)
            close = skip_balanced_parens(text, m.end() - 1)
            if next_significant_char(text, close) == "{":
                defined.add(name)

    return declared, defined, texts


def maybe_macro_generated(name, texts):
    """True if NAME appears as a macro argument anywhere -- SOME_MACRO(name, ...).

    Caught in practice: GE_RS_GET(gePortRulesetEnemyHealth, enemy_health) in ge_ruleset.c
    expands to a real function body the preprocessor writes, which this scan cannot see because
    it never runs the preprocessor. A heuristic, not proof -- it only shows the name reaching some
    call-shaped thing as an argument, not that the macro produces a function body from it. That is
    why these sit in their own bucket rather than being silently dropped.
    """
    pat = re.compile(r"[A-Za-z_]\w*\s*\(\s*" + re.escape(name) + r"\s*[,)]")
    return any(pat.search(t) for t in texts)


# Confirmed by hand, not by this scan: real definitions that live under vendor/ge-decomp/assets/,
# which this tool does not scan because most of that tree is multi-megabyte pixel and text data,
# not code -- scanning it for the sake of one file cost minutes for zero other signal, measured
# by actually trying it. Grep the specific file if one of these ever needs re-checking.
KNOWN_GENERATED_ELSEWHERE = {
    "gePortObsegSize": "vendor/ge-decomp/assets/obseg/ge_obseg_sizes.c, generated by "
                        "tools/gen_obseg_blobs.py",
}


def relroot(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dirs", nargs="+", default=[
        os.path.join(ROOT, "getv", "port"),
        os.path.join(ROOT, "vendor", "ge-decomp", "src"),
    ])
    a = ap.parse_args()

    paths = []
    for d in a.dirs:
        if not os.path.isdir(d):
            print("  (skipping %s -- not present in this tree)" % relroot(d))
            continue
        for dirpath, _, files in os.walk(d):
            for f in files:
                if f.endswith((".c", ".h", ".cpp", ".hpp")):
                    paths.append(os.path.join(dirpath, f))

    print("scanned %d file(s)" % len(paths))
    declared, defined, texts = scan(paths)

    missing = sorted(n for n in declared if n not in defined and n not in KNOWN_GENERATED_ELSEWHERE)
    for n, where in KNOWN_GENERATED_ELSEWHERE.items():
        if n in declared:
            print("(%s: confirmed defined at %s, not rescanned -- see KNOWN_GENERATED_ELSEWHERE)"
                  % (n, where))
    likely_macro = [n for n in missing if maybe_macro_generated(n, texts)]
    ge_port = [n for n in missing if n.startswith("gePort") and n not in likely_macro]
    other = [n for n in missing if not n.startswith("gePort") and n not in likely_macro]

    print("\n%d declared symbol(s) with no definition found in the scanned tree" % len(missing))

    print("\ngePort* boundary symbols -- the shape that has actually caused real breakage:")
    if not ge_port:
        print("  none")
    for n in ge_port:
        sites = declared[n]
        print("  %-32s declared at:" % n)
        for f, ln in sites[:4]:
            print("      %s:%d" % (relroot(f), ln))
        if len(sites) > 4:
            print("      ... and %d more" % (len(sites) - 4))

    print("\npossibly macro-generated (name appears as a call-shaped argument elsewhere -- "
          "check before trusting either way):")
    if not likely_macro:
        print("  none")
    for n in likely_macro:
        f, ln = declared[n][0]
        print("  %-32s %s:%d" % (n, relroot(f), ln))

    print("\neverything else (higher false-positive risk -- #ifdef'd bodies, library externs -- "
          "read before trusting):")
    if not other:
        print("  none")
    for n in other[:40]:
        f, ln = declared[n][0]
        print("  %-32s %s:%d%s" % (n, relroot(f), ln,
              "" if len(declared[n]) == 1 else " (+%d more)" % (len(declared[n]) - 1)))
    if len(other) > 40:
        print("  ... and %d more" % (len(other) - 40))

    return 0


if __name__ == "__main__":
    sys.exit(main())
