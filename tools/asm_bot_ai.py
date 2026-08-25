#!/usr/bin/env python3
"""Assemble bot archetypes into GoldenEye AI-list bytecode.

WHAT THIS DOES

data/bots/*.json describes what a bot should be -- accuracy, speed, what it wants, how it
reacts. This turns each archetype into an actual AI list: the bytecode the game already
interprets for every guard in the campaign.

THE INSTRUCTION SET IS READ, NOT TRANSCRIBED

Both halves of the encoding come from src/bondaicommands.h at run time:

    #define guard_start_patrol_ID 0x20
    #define guard_start_patrol(path_num) \\
            guard_start_patrol_ID, \\
            path_num,

The first gives the opcode byte, the second the operand layout -- a bare parameter is one byte,
CharArrayFrom16Rev(x) is two, little-endian low byte first. Deriving both means an opcode whose
number or arity changes upstream produces an assembly error here rather than a bot that runs
the wrong instruction. Hand-copying either would repeat the mistake that cost five levels their
objective text.

LABELS ARE IDS, NOT OFFSETS

The game's own lists declare targets with a label(n) pseudo-op and jump to n, rather than
encoding byte offsets, so no address fixups are needed. The assembler still checks that every
referenced label is declared and that none is declared twice, because a jump to a label that
does not exist is a bot that silently falls through.

Usage:
    python3 tools/asm_bot_ai.py --out build/bots/ai
"""

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOTS_DIR = os.path.join(ROOT, "data", "bots")
AI_HEADER = os.path.join(ROOT, "vendor", "ge-decomp", "src", "bondaicommands.h")

# Label ids used by the generated programs. Small and fixed; the game's own lists use the same
# low numbers.
L_MAIN, L_ENGAGE, L_EVADE, L_SEEK = 0x00, 0x07, 0x0a, 0x0d


def parse_instruction_set(path=AI_HEADER):
    """{name: {"id": int, "params": [(width, param_name), ...]}} read from the header."""
    if not os.path.exists(path):
        raise SystemExit("cannot find %s (vendor/ is gitignored -- generate it first)" % path)
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    ids = {m.group(1): int(m.group(2), 0)
           for m in re.finditer(r"^#define\s+([a-z_0-9]+)_ID\s+(0x[0-9a-fA-F]+|\d+)",
                                text, re.M)}

    # Macro bodies: "#define name(a, b) \" then one continuation line per operand, or a bare
    # "#define name \" for the operand-less ones.
    ops = {}
    for m in re.finditer(r"^#define\s+([a-z_0-9]+)(\([^)]*\))?\s*\\\n((?:[^\n]*\\\n)*[^\n]*)",
                         text, re.M):
        name, body = m.group(1), m.group(3)
        if name.endswith("_ID") or name not in ids:
            continue
        params = []
        for line in body.split("\n"):
            line = line.strip().rstrip("\\").strip().rstrip(",").strip()
            if not line or line == "%s_ID" % name:
                continue
            wm = re.match(r"CharArrayFrom(\d+)Rev\(\s*(\w+)\s*\)", line)
            if wm:
                params.append((int(wm.group(1)) // 8, wm.group(2)))
            elif re.fullmatch(r"\w+", line):
                params.append((1, line))
        ops[name] = {"id": ids[name], "params": params}

    # The operand-less commands (ai_list_end, ai_sleep, ...) have no macro body to match above.
    for name, val in ids.items():
        ops.setdefault(name, {"id": val, "params": []})
    return ops


class Assembler:
    """Emits one AI list, checking every instruction against the parsed instruction set."""

    def __init__(self, ops, where):
        self.ops = ops
        self.where = where
        self.prog = []          # (name, [args])
        self.errors = []
        self.declared = set()
        self.referenced = []

    def emit(self, name, *args):
        op = self.ops.get(name)
        if op is None:
            self.errors.append("%s: unknown opcode %r" % (self.where, name))
            return self
        want = len(op["params"])
        if len(args) != want:
            self.errors.append("%s: %s takes %d operand(s), got %d"
                               % (self.where, name, want, len(args)))
            return self
        for (width, pname), val in zip(op["params"], args):
            lo, hi = 0, (1 << (8 * width)) - 1
            if not isinstance(val, int) or not (lo <= val <= hi):
                self.errors.append("%s: %s operand %s=%r does not fit in %d byte(s)"
                                   % (self.where, name, pname, val, width))
        if name == "label":
            if args[0] in self.declared:
                self.errors.append("%s: label 0x%02x declared twice" % (self.where, args[0]))
            self.declared.add(args[0])
        else:
            for (width, pname), val in zip(op["params"], args):
                if pname == "label":
                    self.referenced.append(val)
        self.prog.append((name, list(args)))
        return self

    def finish(self):
        for lbl in self.referenced:
            if lbl not in self.declared:
                self.errors.append("%s: jumps to label 0x%02x, which is never declared"
                                   % (self.where, lbl))
        return self

    def to_bytes(self):
        out = bytearray()
        for name, args in self.prog:
            op = self.ops[name]
            out.append(op["id"] & 0xFF)
            for (width, _), val in zip(op["params"], args):
                for b in range(width):           # little-endian, low byte first
                    out.append((val >> (8 * b)) & 0xFF)
        return bytes(out)

    def to_c(self, symbol):
        lines = ["u8 %s[] = {" % symbol]
        for name, args in self.prog:
            if not args:
                lines.append("    %s" % name)
                continue
            # Width-aware, so a 16-bit operand reads as 0x0000 rather than 0x00 and the C
            # matches what the bytes actually are.
            widths = [w for w, _ in self.ops[name]["params"]]
            shown = ", ".join("0x%0*x" % (w * 2, a) for w, a in zip(widths, args))
            lines.append("    %s(%s)" % (name, shown))
        lines.append("};")
        return "\n".join(lines)


def disassemble(code, ops):
    """Bytes back to (name, args), walking with the same instruction set used to emit them.

    This exists to check the assembler against itself. Emitting bytes and trusting them is how
    you ship a list whose operands are a byte out -- the interpreter would read the next
    opcode from the middle of an operand and do something arbitrary. Round-tripping every list
    makes that a build failure instead.
    """
    by_id = {}
    for name, op in ops.items():
        by_id.setdefault(op["id"], (name, op["params"]))
    out, i = [], 0
    while i < len(code):
        entry = by_id.get(code[i])
        if entry is None:
            raise ValueError("unknown opcode byte 0x%02x at %d" % (code[i], i))
        name, params = entry
        i += 1
        args = []
        for width, _ in params:
            if i + width > len(code):
                raise ValueError("operand runs past end of list at %d" % i)
            args.append(int.from_bytes(code[i:i + width], "little"))
            i += width
        out.append((name, args))
    return out


def fixed88(value):
    """Health and armour are 8.8 fixed point in the game's own lists (0x2800 is 40.0)."""
    return max(0, min(0xFFFF, int(round(value * 256))))


def build_program(arch, kind, ops):
    """One archetype -> one AI list.

    The shape is the same loop the game's own guard lists use: a sleep-and-poll main label, a
    perception check that jumps to an engage label, and a return to main. What differs per
    archetype is which dials get set, which perception checks are wired, and what the engage
    branch actually does -- which is exactly the axis Perfect Dark's simulants vary along.
    """
    a = Assembler(ops, "%s/%s" % (kind, arch["name"]))
    dials = arch.get("dials") or {}

    # --- setup: the dials, written once at list start -------------------------------------
    if "accuracy" in dials:
        a.emit("guard_set_accuracy_rating", int(dials["accuracy"]))
    if "speed" in dials:
        a.emit("guard_set_speed_rating", int(dials["speed"]))
    if "hearing" in dials:
        a.emit("guard_set_hearing_scale", int(dials["hearing"]))
    if "health" in dials:
        a.emit("guard_set_health_total", fixed88(dials["health"] / 10.0))
    if "armour" in dials:
        a.emit("guard_set_armour", fixed88(dials["armour"] / 10.0))

    ops_named = set(arch.get("opcodes") or [])
    aggression = int(dials.get("aggression", 60))

    # --- main loop ------------------------------------------------------------------------
    a.emit("label", L_MAIN)
    a.emit("ai_sleep")
    a.emit("if_guard_sees_bond", L_ENGAGE)
    if "if_guard_heard_bond" in ops_named:
        a.emit("if_guard_heard_bond", L_ENGAGE)
    if "if_guard_see_another_guard_die" in ops_named:
        a.emit("if_guard_see_another_guard_die", L_ENGAGE)
    if "if_guard_see_another_guard_shot" in ops_named:
        a.emit("if_guard_see_another_guard_shot", L_ENGAGE)
    # --- idle behaviour: what the bot does when it has no target --------------------------
    # Only emit the idle block if the archetype actually has idle behaviour. Without this an
    # archetype that only fights gets a jump to a label that immediately jumps back, which is
    # correct but dead weight in a list the interpreter walks every tick.
    if "guard_start_patrol" in ops_named:
        a.emit("goto_first", L_SEEK)
        a.emit("label", L_SEEK)
        a.emit("guard_start_patrol", 0x00)
        a.emit("goto_first", L_MAIN)
    else:
        a.emit("goto_first", L_MAIN)

    # --- engage ---------------------------------------------------------------------------
    a.emit("label", L_ENGAGE)
    a.emit("random_generate_seed")
    # Aggression as a dice roll: the less aggressive the archetype, the more often it breaks
    # off to the evade branch instead of committing to the fight.
    a.emit("if_random_seed_greater_than", max(0, min(255, aggression * 255 // 100)), L_EVADE)

    # Every guard_try_* command takes exactly one operand: the label to jump to when the action
    # cannot be performed. They are attempts, not orders, and the label is the fallback.
    if "guard_try_throwing_grenade" in ops_named:
        a.emit("guard_try_throwing_grenade", L_EVADE)
    if "guard_try_sprinting_to_bond_position" in ops_named:
        a.emit("guard_try_sprinting_to_bond_position", L_EVADE)
    elif "guard_try_running_to_bond_position" in ops_named:
        a.emit("guard_try_running_to_bond_position", L_EVADE)

    for firing in ("guard_try_firing_run", "guard_try_firing_walk"):
        if firing in ops_named:
            a.emit(firing, L_EVADE)
            break
    a.emit("guard_try_fire_or_aim_at_target", 0x0100, 0x0000, L_EVADE)
    a.emit("goto_first", L_MAIN)

    # --- evade ----------------------------------------------------------------------------
    a.emit("label", L_EVADE)
    for ev in ("guard_try_firing_roll", "guard_try_hopping_sideways",
               "guard_try_running_to_side", "guard_try_sidestepping"):
        if ev in ops_named:
            a.emit(ev, L_MAIN)
            break
    a.emit("goto_first", L_MAIN)
    a.emit("ai_list_end")
    return a.finish()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("build", "bots", "ai"))
    args = ap.parse_args()
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)

    ops = parse_instruction_set()
    with_ops = sum(1 for v in ops.values() if v["params"])
    print("instruction set: %d opcodes (%d take operands)" % (len(ops), with_ops))

    manifest, errors = [], 0
    c_parts = ["/* Generated by tools/asm_bot_ai.py -- do not edit. */",
               '#include "bondaicommands.h"', ""]

    for path in sorted(glob.glob(os.path.join(BOTS_DIR, "*.json"))):
        kind = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        for arch in doc.get("archetypes", []):
            a = build_program(arch, kind, ops)
            for e in a.errors:
                print("  ERROR %s" % e)
            errors += len(a.errors)
            if a.errors:
                continue
            code = a.to_bytes()

            # Round-trip: the bytes must disassemble back to exactly what was assembled.
            try:
                back = disassemble(code, ops)
            except ValueError as exc:
                print("  ERROR %s/%s: bytes do not disassemble: %s" % (kind, arch["name"], exc))
                errors += 1
                continue
            if back != [(n, list(v)) for n, v in a.prog]:
                print("  ERROR %s/%s: round-trip mismatch" % (kind, arch["name"]))
                errors += 1
                continue

            symbol = "bot_ai_%s_%s" % (kind.rstrip("s"), arch["name"])
            c_parts.append(a.to_c(symbol))
            c_parts.append("")
            with open(os.path.join(out_dir, symbol + ".bin"), "wb") as fh:
                fh.write(code)
            manifest.append({"archetype": arch["name"], "kind": kind, "symbol": symbol,
                             "bytes": len(code), "instructions": len(a.prog),
                             "labels": sorted(a.declared)})
            print("  %-28s %-3d instructions  %-4d bytes" % (arch["name"], len(a.prog), len(code)))

    with open(os.path.join(out_dir, "bot_ai_lists.c"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(c_parts))

    # A port-consumable header carrying the assembled BYTES rather than the macros. The port
    # ships exactly the bytes that were round-trip verified here, and does not have to include
    # the game's macro header or re-derive anything at build time.
    hdr = ["/* Generated by tools/asm_bot_ai.py -- do not edit.",
           " *",
           " * Assembled AI-list bytecode for the bot archetypes, as bytes. Each list was",
           " * disassembled back and compared against what was assembled before being written",
           " * here, so these are verified rather than merely emitted.",
           " */",
           "#ifndef GE_BOT_AI_LISTS_H",
           "#define GE_BOT_AI_LISTS_H", ""]
    for m in manifest:
        with open(os.path.join(out_dir, m["symbol"] + ".bin"), "rb") as fh:
            code = fh.read()
        rows = ", ".join("0x%02x" % b for b in code)
        hdr.append("static const unsigned char %s[] = { %s };" % (m["symbol"], rows))
    hdr.append("")
    hdr.append("typedef struct { const char *name; const char *kind;")
    hdr.append("                 const unsigned char *code; unsigned int len; } GeBotAiList;")
    hdr.append("")
    hdr.append("static const GeBotAiList ge_bot_ai_lists[] = {")
    for m in manifest:
        hdr.append('    { "%s", "%s", %s, %d },' % (m["archetype"], m["kind"], m["symbol"],
                                                    m["bytes"]))
    hdr.append("};")
    hdr.append("#define GE_BOT_AI_LIST_COUNT %d" % len(manifest))
    hdr.append("")
    hdr.append("#endif /* GE_BOT_AI_LISTS_H */")
    port_hdr = os.path.join(ROOT, "getv", "port", "src", "ge_bot_ai_lists.h")
    if os.path.isdir(os.path.dirname(port_hdr)):
        with open(port_hdr, "w", encoding="utf-8") as fh:
            fh.write("\n".join(hdr) + "\n")
        print("  wrote %s" % os.path.relpath(port_hdr, ROOT))
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as fh:
        json.dump({"counts": {"archetypes": len(manifest),
                              "total_bytes": sum(m["bytes"] for m in manifest)},
                   "lists": manifest}, fh, indent=1)

    print("\nassembled %d lists, %d bytes total -> %s"
          % (len(manifest), sum(m["bytes"] for m in manifest), out_dir))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
