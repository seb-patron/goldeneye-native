#!/usr/bin/env python3
"""
Generate 32-bit "file view" structs for GoldenEye's model assets.

GoldenEye's model files store 32-bit VMA-relative OFFSETS, which the game promotes
to real pointers in place at load (model.c:5677 PROMOTE, and
modelPromoteNodeOffsetsToPointers). That works on the N64 because a pointer is 4
bytes and fits the file's slot. On arm64 the struct field is 8 bytes, so the loaded
buffer can no longer be reinterpreted as the struct at all.

The port therefore keeps the asset format byte-identical and converts at load. This
emits the missing half of that: a `<Name>_file` mirror of every asset struct with each
pointer member replaced by a u32 offset, so the file bytes can be read correctly at
any pointer width.

Verification is the point: sizeof(<Name>_file) on arm64 MUST equal sizeof(<Name>)
under a 32-bit ABI. The generated header asserts exactly that, and the companion
assertions are checked against armv7 so the two views are proven to agree.

Run from the decomp root (vendor/ge-decomp):
    python3 ../../tools/gen_asset_fileview.py
"""
import io
import re
import os
import sys
import shutil
import subprocess
import tempfile

# Every subprocess call below names stdin=subprocess.DEVNULL. Python otherwise inherits the
# parent's, which means asking Windows for STD_INPUT_HANDLE before it even looks for the
# executable -- and under the setup wizard that has come back invalid, killing the tool with
# WinError 6 before any compiler runs. Issues #7 and #8. Nothing here reads stdin.


HDR = 'src/bondtypes.h'
OUT = 'src/ge_asset_fileview.h'
CHECK = 'src/ge_asset_fileview_check.c'

def _sysroot_flags():
    """-isysroot on Apple, nothing anywhere else.

    xcrun exists only on macOS. Calling it on Linux leaves SDK empty, and passing
    `-isysroot ''` makes every compile fail for a reason that looks nothing like the cause.
    Off Apple the system headers are already on the default search path, so the right answer
    is to pass no sysroot at all rather than an empty one.

    macOS first, then the tvOS simulator: the Command Line Tools alone provide the macOS SDK,
    and this only ever compiles throwaway objects, so the SDK does not have to match the real
    build target.
    """
    if sys.platform != 'darwin':
        return []
    for sdk in ('macosx', 'appletvsimulator', 'appletvos'):
        r = subprocess.run(['xcrun', '-sdk', sdk, '--show-sdk-path'],
                           capture_output=True, text=True, stdin=subprocess.DEVNULL)
        path = r.stdout.strip()
        if r.returncode == 0 and path and os.path.isdir(path):
            return ['-isysroot', path]
    sys.exit("no usable SDK on macOS: install the Xcode Command Line Tools "
             "(xcode-select --install).")


BASE = _sysroot_flags() + ['-fsyntax-only', '-fms-extensions',
        '-include', 'src/ge_port_decls.h',
        '-I', '.', '-I', 'include', '-I', 'include/PR', '-I', 'src',
        '-I', 'src/game', '-I', 'src/inflate',
        '-DVERSION_US', '-DLANG_US', '-DREFRESH_NTSC', '-DLEFTOVERDEBUG',
        '-DLEFTOVERSPECTRUM', '-DBUGFIX_R0', '-DTARGET_N64', '-DGE_PORT_NATIVE',
        '-DNON_MATCHING=1', '-DAVOID_UB=1', '-D_LANGUAGE_C=1',
        '-w', '-ferror-limit=0']

PTR_MEMBER = re.compile(r'^(\s*)(?:struct\s+|union\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*'
                        r'([A-Za-z_][A-Za-z0-9_]*)\s*;')

# An ARRAY of pointers, e.g. `void *unk0c[16];`. Every element is a 4-byte offset in
# the file, so the COUNT must survive into the file view -- collapsing it to a single
# u32 silently changes the record size, and the N64-layout assertion then fails.
PTR_ARRAY_MEMBER = re.compile(r'^\s*(?:struct\s+|union\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*'
                              r'([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*([0-9]+)\s*\]\s*;')

def struct_names():
    """ModelNode, ModelAnimation, plus every member type of union ModelRoData.

    ModelAnimation is here because the ANIMATION TABLE has the same problem the model
    files do: the asset stores 32-bit records, but the native struct has two pointers
    (bitDescriptors, bitStream) AND sub-word fields (u16 unk04, u8 unk06/unk07), so the
    asset bytes cannot be reinterpreted as ModelAnimation at any offset. The table has
    to be converted at load, and that needs a correct 32-bit view of the record.
    """
    text = io.open(HDR, encoding='utf-8', errors='surrogateescape').read()
    m = re.search(r'union ModelRoData\s*\{(.*?)\n\s*\}', text, re.S)
    names = sorted(set(re.findall(r'struct (ModelRoData_[A-Za-z0-9_]+)', m.group(1))))
    return ['ModelNode', 'ModelAnimation'] + names


def body(name):
    """Return the brace body of `typedef struct <name>` / `struct <name>`."""
    lines = io.open(HDR, encoding='utf-8', errors='surrogateescape').read().split('\n')
    for i, l in enumerate(lines):
        if re.match(r'^\s*(?:typedef\s+)?struct\s+%s\s*$' % re.escape(name), l) or \
           re.match(r'^\s*(?:typedef\s+)?struct\s+%s\s*\{' % re.escape(name), l):
            j = i
            while '{' not in lines[j]:
                j += 1
            depth, out = 0, []
            while True:
                depth += lines[j].count('{') - lines[j].count('}')
                out.append(lines[j])
                if depth <= 0 and len(out) > 1:
                    break
                j += 1
            return out
    return None


def to_fileview(name, src):
    """Rewrite pointer members as u32, collapsing anonymous pointer-only unions."""
    out, i = [], 0
    inner = src[1:-1]           # drop the opening/closing brace lines
    while i < len(inner):
        line = inner[i]
        stripped = line.strip()
        if stripped.startswith('union') and '{' in line:
            # collapse an anonymous union of pointers to one u32 offset
            j, depth, members = i, 0, []
            while True:
                depth += inner[j].count('{') - inner[j].count('}')
                members.append(inner[j])
                if depth <= 0 and j > i:
                    break
                j += 1
            ptrs = [PTR_MEMBER.match(x) for x in members]
            ptrs = [p for p in ptrs if p]
            if ptrs and len(ptrs) == len([x for x in members
                                          if x.strip() and not x.strip().startswith(('union', '}', '/*', '*', '//'))]):
                out.append('    u32 %s; /* was a union of pointers */' % ptrs[0].group(2))
                i = j + 1
                continue
        # An ARRAY of pointers, e.g. `void *unk0c[16];`. Each element is a 4-byte
        # offset in the file, so the count must be preserved -- collapsing it to a
        # single u32 silently changes the record size and the N64-layout assertion
        # then fails (which is how this case was found).
        ma = PTR_ARRAY_MEMBER.match(line)
        if ma:
            out.append('    u32 %s[%s];' % (ma.group(1), ma.group(2)))
            i += 1
            continue
        m = PTR_MEMBER.match(line)
        if m:
            out.append('    u32 %s;' % m.group(2))
        else:
            out.append(line)
        i += 1
    return 'typedef struct %s_file\n{\n%s\n} %s_file;' % (name, '\n'.join(out), name)


def main():
    names = struct_names()
    parts, checks, skipped = [], [], []
    for n in names:
        b = body(n)
        if not b:
            skipped.append(n)
            continue
        parts.append(to_fileview(n, b))
        checks.append((n, '_Static_assert(sizeof(struct %s_file) == GE_N64_SIZEOF_%s,\n'
                          '  "%s_file does not match the N64 layout of %s");' % (n, n, n, n)))

    io.open(OUT, 'w', encoding='utf-8', errors='surrogateescape').write(
        '\n\n'.join([
            '/* GENERATED by tools/gen_asset_fileview.py - do not edit by hand.',
            ' *',
            ' * 32-bit mirrors of the model-asset structs. GoldenEye stores VMA-relative',
            ' * OFFSETS in its model files and promotes them to pointers at load time',
            ' * (model.c PROMOTE). At 64-bit a pointer no longer fits the file\'s 4-byte',
            ' * slot, so the file bytes must be read through these views and converted.',
            ' *',
            ' * Each u32 below is a file offset, NOT a pointer. */',
            '#ifndef GE_ASSET_FILEVIEW_H',
            '#define GE_ASSET_FILEVIEW_H',
            '#include "bondtypes.h"',
        ] + parts + ['#endif', '']))
    print('generated %d file-view structs' % len(parts))
    if skipped:
        print('skipped (body not found): %s' % ', '.join(skipped))

    # Everything from here down verifies what was written above rather than producing it. OUT is
    # the output and the only thing the build reads; CHECK is excluded from the source list on
    # every platform. The probe needs clang and an Apple target triple to get a 32-bit ABI, so on
    # a host with no clang there is nothing to run -- and it says so instead of returning quietly,
    # because a verification that silently does nothing is worse than one that is openly absent.
    clang = shutil.which('clang')
    if not clang:
        print('\nskipped the layout verification: no clang on this host, so the 32-bit size '
              'probe cannot run.\n%s was generated and is what the build uses.' % OUT)
        return

    # Was a hardcoded /tmp/sz.c, which is not a path that exists on Windows -- the generator
    # wrote its header, reported success, and then died in its own self-check.
    probe_dir = tempfile.mkdtemp(prefix='ge_fileview_')
    probe_c = os.path.join(probe_dir, 'sz.c')

    # Ground truth: the real struct sizes under a 32-bit ABI == the N64 sizes.
    sizes = {}
    for n, _ in checks:
        lo, hi = 1, 512
        # binary search the size via assertions (no execution available)
        while lo < hi:
            mid = (lo + hi) // 2
            src = '_Static_assert(sizeof(struct %s) <= %d, "x");' % (n, mid)
            io.open(probe_c, 'w').write(src + '\n')
            r = subprocess.run([clang, '-target', 'armv7-apple-ios10.0'] + BASE + [probe_c],
                               capture_output=True, text=True, stdin=subprocess.DEVNULL)
            if 'error:' in (r.stdout + r.stderr):
                lo = mid + 1
            else:
                hi = mid
        sizes[n] = lo
    io.open(CHECK, 'w', encoding='utf-8', errors='surrogateescape').write(
        '/* GENERATED - proves each file view matches the N64 layout. */\n'
        '#include "ge_asset_fileview.h"\n\n' +
        '\n'.join('#define GE_N64_SIZEOF_%s %d' % (n, s) for n, s in sizes.items()) +
        '\n\n' + '\n'.join(c for _, c in checks) + '\n')

    shutil.rmtree(probe_dir, ignore_errors=True)

    r = subprocess.run([clang, '-target', 'arm64-apple-tvos17.0'] + BASE + [CHECK],
                       capture_output=True, text=True, stdin=subprocess.DEVNULL)
    errs = [l for l in (r.stdout + r.stderr).split('\n') if 'error:' in l]
    bad = [l for l in errs if 'does not match' in l]
    print('\n%d of %d file views match the N64 layout exactly'
          % (len(checks) - len(bad), len(checks)))
    for l in bad:
        m = re.search(r'"([A-Za-z0-9_]+)_file does not match', l)
        if m:
            print('  MISMATCH: %s (N64 size %d)' % (m.group(1), sizes.get(m.group(1), -1)))
    other = [l for l in errs if 'does not match' not in l]
    if other:
        print('\n%d other errors:' % len(other))
        for l in other[:5]:
            print('  ' + l[:150])


if __name__ == '__main__':
    main()
