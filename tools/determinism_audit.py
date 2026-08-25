#!/usr/bin/env python3
"""Find the things that will break lockstep.

WHY

Lockstep works only if every machine simulates identically from the same inputs. Anything that
reads the wall clock, an unsynchronised random source, real elapsed time, or uninitialised
memory is a divergence waiting to happen. The fingerprint exchange in ge_net.c will CATCH a
divergence, but it cannot locate one -- it reports that two machines disagree, not which line
made them disagree. This finds candidates before they cost a debugging session.

WHAT MATTERS AND WHAT DOES NOT

The distinction this tool exists to draw is SIMULATION versus PRESENTATION. A wall-clock read in
the audio mixer or the renderer is harmless: those do not feed the simulation, and two machines
may render at different rates all day without disagreeing about where anybody is standing. The
same call inside movement or AI is fatal. So every hit is classified by where it lives, and the
report leads with the ones that can actually diverge a session.

Run:  python3 tools/determinism_audit.py
"""

import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where the simulation actually happens. A hit here can change what the game simulates and so
# can diverge two machines; a hit anywhere else generally cannot.
SIM_PATHS = (
    os.path.join("vendor", "ge-decomp", "src", "game"),
)
# Presentation and host plumbing: real time here is expected and correct.
PRESENTATION_HINTS = (
    "port_audio", "port_vi", "port_render", "gfx_", "ge_launcher", "ge_postfx",
    "port_os", "sys_shim", "ge_lua", "ge_config",
)

PATTERNS = [
    # (label, regex, severity, note)
    ("wall clock", r"\b(time|clock|gettimeofday|GetTickCount(64)?|timeGetTime|"
                   r"QueryPerformanceCounter|SDL_GetTicks(64)?|SDL_GetPerformanceCounter)\s*\(",
     "high", "real time differs on every machine and every run"),
    ("host rng", r"\b(rand|srand|random|srandom|arc4random|rand_r)\s*\(",
     "high", "an unsynchronised random source diverges immediately"),
    ("uninitialised", r"\bmalloc\s*\(", "low",
     "malloc without a following memset can be read before it is written"),
    ("float mode", r"-ffast-math|-funsafe-math|__FLT_EVAL_METHOD__|_controlfp",
     "high", "changes float results between machines or builds"),
    ("frame delta", r"\b(delta_?time|deltaTime|dt_?seconds|elapsed_?ms|frame_?time)\b",
     "medium", "frame-rate dependent logic makes simulation depend on how fast a machine runs"),
    ("thread", r"\b(pthread_create|CreateThread|std::thread|SDL_CreateThread)\s*\(",
     "medium", "concurrent mutation of simulation state is order-dependent"),
    ("pointer identity", r"\(\s*(?:unsigned\s+)?(?:long|int|size_t|uintptr_t)\s*\)\s*&",
     "low", "addresses differ per process, so anything derived from one does too"),
]


def classify(path):
    rel = os.path.relpath(path, ROOT)
    norm = rel.replace("\\", "/")
    base = os.path.basename(path)
    if any(norm.startswith(p.replace("\\", "/")) for p in SIM_PATHS):
        return "simulation"
    if any(h in base for h in PRESENTATION_HINTS):
        return "presentation"
    if "/port/" in norm or norm.startswith("getv/"):
        return "port"
    return "other"


def scan():
    hits = defaultdict(list)
    scanned = 0
    roots = [
        os.path.join(ROOT, "vendor", "ge-decomp", "src"),
        os.path.join(ROOT, "getv", "port", "src"),
    ]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            if "thirdparty" in dirpath or "libultra" in dirpath:
                continue
            for fn in files:
                if not fn.endswith((".c", ".h", ".cpp")):
                    continue
                path = os.path.join(dirpath, fn)
                scanned += 1
                try:
                    with open(path, encoding="utf-8", errors="replace") as fh:
                        text = fh.read()
                except OSError:
                    continue
                area = classify(path)
                for label, rx, sev, note in PATTERNS:
                    for m in re.finditer(rx, text):
                        line = text.count("\n", 0, m.start()) + 1
                        # Skip comments cheaply: if the match sits after a // on its own line.
                        ls = text.rfind("\n", 0, m.start()) + 1
                        prefix = text[ls:m.start()]
                        if "//" in prefix or "* " in prefix:
                            continue
                        hits[(area, label, sev, note)].append(
                            (os.path.relpath(path, ROOT).replace("\\", "/"), line))
    return scanned, hits


def main():
    scanned, hits = scan()
    print("determinism audit -- %d source files scanned\n" % scanned)

    order = {"simulation": 0, "port": 1, "presentation": 2, "other": 3}
    sev_order = {"high": 0, "medium": 1, "low": 2}
    keys = sorted(hits, key=lambda k: (order.get(k[0], 9), sev_order.get(k[2], 9), k[1]))

    blocking = 0
    for area, label, sev, note in keys:
        where = hits[(area, label, sev, note)]
        if area == "simulation" and sev in ("high", "medium"):
            blocking += len(where)
        head = "%-12s %-16s %-6s %4d" % (area, label, sev, len(where))
        print("  %s   %s" % (head, note))
        # Show a few examples for the ones that matter.
        if area in ("simulation", "port") and sev != "low":
            seen = set()
            for f, ln in where:
                if f in seen:
                    continue
                seen.add(f)
                print("        %s:%d" % (f, ln))
                if len(seen) >= 4:
                    break

    print("\nsimulation-path hits at medium or high: %d" % blocking)
    print("\nReading this: presentation hits are expected and fine -- the renderer and mixer are")
    print("allowed to know what time it is. Only the simulation rows can diverge two machines,")
    print("and 'port' rows matter exactly insofar as that code feeds the simulation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
