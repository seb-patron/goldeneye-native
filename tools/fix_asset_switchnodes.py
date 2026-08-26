#!/usr/bin/env python3
"""Retype getools' `SwitchNodes` model-asset arrays from u32 to real ModelNode pointers.

THE BUG
-------
getools emitted every extracted model's switch-index array as

    u32 SwitchNodes[11] =
    {
        (u32)&ModelNode_0x0f8,  // ModelNode at offset 0x0F8
        0x00000000,             // NULL
        ...
    };

On the N64 that was fine: a pointer was 4 bytes, and the array literally held the
32-bit segment-relative addresses the loader would later fix up. On arm64 it is wrong
twice over:

  1. `(u32)&sym` is not a compile-time constant expression on a 64-bit target, so the
     translation unit does not compile at all. 156 of the 762
     asset TUs died on exactly this -- 68 chr, 61 prop, 27 gun -- which meant 156
     character/weapon/prop models were absent from the binary entirely.
  2. Even if it compiled, the value would be a pointer truncated to 32 bits -- the
     dominant bug family on this port. Our heap lives at
     0x1_05xxxxxx, so a truncated pointer is visually indistinguishable from an N64
     segment-0x05 address, which makes this failure mode especially hard to read.

WHY NATIVE POINTERS AND NOT OFFSETS
-----------------------------------
The other candidate fixes were 32-bit offsets resolved against a segment base, or a
gePropdefsConvertToNative()-style load-time conversion pass. Neither applies here:
these asset .c files are COMPILED, not loaded -- there is no file image and no
segment base for an offset to be relative to. Every other pointer in the very same
struct literal (ModelNode.Data/Parent/Next/Prev/Child, SwitchRecord.Controls) is
already a real native pointer emitted by getools. `SwitchNodes` is the one place it
narrowed, so widening it back to a pointer restores internal consistency.

Verified before changing anything: `SwitchNodes` is never READ. The switch tree is
walked through ModelRoData_SwitchRecord.Controls (model.c:1984, 6787, 6992), which
is already `ModelNode *`. Nothing in src/, getv/ or assets/ references any
*SwitchNodes symbol. So widening the array cannot change any record's size or any
consumer's view of it -- it only has to compile and hold a correct address.

Idempotent: re-running on already-converted files is a no-op.

Usage:  tools/fix_asset_switchnodes.py [--dry-run]
"""
import io, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     '..', 'vendor', 'ge-decomp'))
DECL = re.compile(r'^(\s*)u32(\s+[A-Za-z0-9_]*SwitchNodes\s*\[\s*\d*\s*\]\s*=)', re.M)

def convert(text):
    """Retype each SwitchNodes decl and rewrite the initializers in ITS brace body only."""
    out, pos, n = [], 0, 0
    for m in DECL.finditer(text):
        # find the brace body that follows this declaration
        b = text.index('{', m.end())
        depth, j = 0, b
        while True:
            if text[j] == '{': depth += 1
            elif text[j] == '}': depth -= 1
            if depth == 0: break
            j += 1
        body = text[b:j + 1]
        body = re.sub(r'\(\s*u32\s*\)\s*&', '&', body)
        body = re.sub(r'\b0x0{8}\b', 'NULL', body)
        out.append(text[pos:m.start()])
        out.append('%sModelNode *%s' % (m.group(1), m.group(2).lstrip()))
        out.append(text[m.end():b])
        out.append(body)
        pos = j + 1
        n += 1
    out.append(text[pos:])
    return ''.join(out), n

def main():
    dry = '--dry-run' in sys.argv
    changed = touched = 0
    for dirpath, _, names in os.walk(os.path.join(ROOT, 'assets')):
        for name in sorted(names):
            if not name.endswith('.c'):
                continue
            p = os.path.join(dirpath, name)
            s = io.open(p, encoding='utf-8', errors='surrogateescape').read()
            if 'SwitchNodes' not in s:
                continue
            new, n = convert(s)
            touched += n
            if new != s:
                changed += 1
                if not dry:
                    io.open(p,'w',encoding='utf-8',errors='surrogateescape').write(new)
                print('converted:', os.path.relpath(p, ROOT))
    print('%d SwitchNodes arrays seen, %d files %s'
          % (touched, changed, 'would change' if dry else 'changed'))

main()
