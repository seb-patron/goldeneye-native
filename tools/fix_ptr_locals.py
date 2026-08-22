#!/usr/bin/env python3
"""
tvOS port helper: widen locals that hold pointers but are declared s32/u32.

The N64 build stored pointers in 32-bit ints, which is lossless on MIPS and
lossy on arm64. Rather than cast at each use (which would truncate), this
retypes the *declaration* using the type clang itself reports, so the pointer
stays 64-bit wide end to end.

Only declarations inside the function containing the flagged assignment are
touched, so same-named locals in other functions are unaffected.

Usage: fix_ptr_locals.py <file.c> [...]
"""
import io
import re
import subprocess
import sys

SDK = subprocess.run(['xcrun', '-sdk', 'appletvos', '--show-sdk-path'],
                     capture_output=True, text=True).stdout.strip()

FLAGS = ['-target', 'arm64-apple-tvos17.0', '-isysroot', SDK, '-fsyntax-only',
         '-fms-extensions', '-include', 'src/ge_port_decls.h',
         '-I', '.', '-I', 'include', '-I', 'include/PR', '-I', 'src',
         '-I', 'src/game', '-I', 'src/inflate',
         '-DVERSION_US', '-DLANG_US', '-DREFRESH_NTSC', '-DLEFTOVERDEBUG',
         '-DLEFTOVERSPECTRUM', '-DBUGFIX_R0', '-DTARGET_N64', '-DGE_PORT_NATIVE',
         '-DNON_MATCHING=1', '-DAVOID_UB=1', '-D_LANGUAGE_C=1',
         '-w', '-ferror-limit=0']

# "incompatible pointer to integer conversion assigning to 's32' ... from 'struct font *'"
ERR = re.compile(r"^([^:]+):(\d+):\d+: error: incompatible pointer to integer "
                 r"conversion assigning to '[^']+'[^\n]*? from '([^']+)'")


def compile_errors(path):
    r = subprocess.run(['clang'] + FLAGS + [path], capture_output=True, text=True)
    return (r.stdout + r.stderr).split('\n')


def enclosing_body(lines, idx):
    """Range of the function body containing line index idx (0-based)."""
    start = 0
    for i in range(idx, -1, -1):
        if lines[i].startswith('{'):
            start = i
            break
    return start, idx


def fix(path):
    total = 0
    for _ in range(10):
        lines = io.open(path, encoding='utf-8', errors='surrogateescape').read().split('\n')
        edits = {}
        for line in compile_errors(path):
            m = ERR.match(line)
            if not m or m.group(1) != path:
                continue
            ln, ptype = int(m.group(2)), m.group(3).strip()
            if ln > len(lines):
                continue
            lhs = lines[ln - 1].split('=')[0].strip()
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', lhs):
                continue  # only plain scalar locals, never struct fields or indexing
            start, _ = enclosing_body(lines, ln - 1)
            decl = re.compile(r'^(\s*)(?:s32|u32|int)(\s+)%s\s*;\s*$' % re.escape(lhs))
            for i in range(start, ln):
                dm = decl.match(lines[i])
                if dm and i not in edits:
                    base = ptype.rstrip('*').strip()   # 'struct font *' -> 'struct font'
                    stars = '*' * ptype.count('*')
                    edits[i] = '%s%s %s%s;' % (dm.group(1), base, stars, lhs)
                    break
        if not edits:
            break
        for i, new in edits.items():
            lines[i] = new
        io.open(path, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(lines))
        total += len(edits)
    return total


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print('%s: retyped %d local declarations' % (p, fix(p)))
