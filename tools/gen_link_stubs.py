#!/usr/bin/env python3
"""
Generate link stubs for still-undefined symbols, classified as DATA or FUNCTION.

Why classification matters
--------------------------
Emitting every undefined symbol as a zero-returning function links, but any symbol that
is really data then resolves to code bytes: reading it gives garbage and often SIGBUS.
The crash then looks like a port bug when it is an artifact of the diagnostic.

So each symbol is classified from the decomp's own declarations:
  extern <type> name;      / name[]  -> data      -> emit a real zeroed object
  <type> name(args);                 -> function  -> emit a logging stub

Function stubs print their name once and return 0, so the boot continues and yields a
trace of what startup actually touches rather than stopping at the first gap.

Diagnostic only. Delete once the real implementations land.

Usage (from the decomp root):
    python3 ../../tools/gen_link_stubs.py <undefined-symbols-file> <output.c>
"""
import io
import os
import re
import sys

DECL_CACHE = None


def _roots():
    import itertools
    return itertools.chain(os.walk('src'), os.walk('include'))


def load_decls():
    """Map symbol -> ('func'|'data', declared type text) from every header/source."""
    global DECL_CACHE
    if DECL_CACHE is not None:
        return DECL_CACHE
    funcs, data = {}, {}
    # This must include the N64 SDK headers, not just src/. libultra functions like
    # osInvalDCache are declared only in include/PR/*.h; without them they fall to the
    # "unclassified" default and get emitted as data, so calling one jumps into
    # 0xFF-filled bytes and SIGBUSes.
    for root, _, files in _roots():
        for f in files:
            if not f.endswith(('.h', '.c')):
                continue
            p = os.path.join(root, f)
            try:
                t = io.open(p, encoding='utf-8', errors='surrogateescape').read()
            except OSError:
                continue
            t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
            # function declarations / definitions
            for m in re.finditer(r'^\s*(?:extern\s+)?([A-Za-z_][A-Za-z0-9_ \t\*]*?[ \t\*])'
                                 r'([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)', t, re.M):
                funcs.setdefault(m.group(2), (m.group(1).strip(), m.group(3)))
            # data declarations: extern TYPE name;  /  extern TYPE name[...];
            for m in re.finditer(r'^\s*extern\s+([A-Za-z_][A-Za-z0-9_ \t\*]*?[ \t\*])'
                                 r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;', t, re.M):
                data.setdefault(m.group(2), (m.group(1).strip(), m.group(3) or ''))
    DECL_CACHE = (funcs, data)
    return DECL_CACHE


def main():
    src, out_path = sys.argv[1], sys.argv[2]
    # Mach-O prefixes exactly one underscore. lstrip('_') would eat both underscores of a
    # symbol like __GlobalimagetableSegmentEnd, whose real C name is
    # _GlobalimagetableSegmentEnd, emitting the wrong symbol and leaving it undefined.
    syms = []
    for l in io.open(src):
        l = l.strip()
        if not l:
            continue
        syms.append(l[1:] if l.startswith('_') else l)
    # The generator's own API must never be stubbed. The regeneration recipe deletes
    # ge_link_stubs.c before relinking, so these show up in the undefined list like any
    # other symbol and get emitted as data arrays, which then collide with the real
    # definitions appended at the bottom of this same file.
    # A stub that returns 0 without writing its output parameter is worse than a missing
    # symbol: the caller reads uninitialised memory as a matrix and nothing logs it.
    # src/libultra/gu/*.c is compiled for real, so these must never be stubbed.
    GU_REAL = {'guLookAt', 'guLookAtReflect', 'guMtxF2L', 'guOrtho', 'guPerspective',
               'guPerspectiveF', 'guRotate', 'guRotateF', 'guScale', 'guScaleF',
               'guTranslate', 'guNormalize', 'guAlignF', 'guMtxIdentF', 'guMtxIdent'}
    # The port's own renderer must never be stubbed. If gfx_pc.c fails to compile, every
    # Fast3D symbol becomes undefined and this generator would emit
    # gfx_init/gfx_run/gfx_start_frame as data arrays. The app then links cleanly and
    # calls straight into a data array: a SIGBUS at boot with no backtrace, whose real
    # cause (a one-line typo in gfx_pc.c) is nowhere in the build output, because build.sh
    # reports only the game-file count and swallows compiler stderr.
    # Keep this list in step with gfx_pc.h.
    FAST3D_OWN = {'gfx_init', 'gfx_run', 'gfx_start_frame', 'gfx_end_frame',
                  'gfx_shutdown', 'gfx_dump_trace', 'gfx_current_dimensions',
                  'gfx_output_dimensions', 'gfx_supersample', 'gfx_segment_table',
                  'gfx_native_width', 'gfx_native_height'}
    GE_PORT_OWN = {'gePortStubInit', 'gePortStubCheck', 'osSyncPrintf'} | GU_REAL | FAST3D_OWN
    syms = [x for x in syms if x not in GE_PORT_OWN]

    # Never stub a symbol that has a real definition in assets/. A stub here is
    # `unsigned char NAME[GE_STUB_BYTES];` -- a tentative definition, so the linker merges
    # it into the real object with no duplicate-symbol error. gePortStubInit() then memsets
    # 0xFF over the first 4096 bytes of the genuine data and writes a canary 256 KB
    # downstream. That silently destroyed the ARCHIVES stagesetup: its address, its
    # initializer and the resource lookup were all correct, and the struct still read as
    # all-0xFF from boot because our own poison had overwritten it.
    # This check must run every time, because a symbol can gain a real definition later
    # (UsetuparchZ only started compiling once an unrelated macro bug was fixed).
    import re as _re, os as _os
    _assets = _os.path.join(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))),
                            'vendor', 'ge-decomp', 'assets')
    _defined = set()
    for _root, _dirs, _files in _os.walk(_assets):
        for _fn in _files:
            if not _fn.endswith('.c') or _fn.endswith('.inc.c'):
                continue
            try:
                _txt = io.open(_os.path.join(_root, _fn), errors='ignore').read()
            except OSError:
                continue
            # a definition with an initializer, e.g. `stagesetup UsetuparchZ = {`
            for _m in _re.finditer(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*=',
                                   _txt, _re.M):
                _defined.add(_m.group(1))
    _shadowed = [x for x in syms if x in _defined]
    if _shadowed:
        print('  gen_link_stubs: refusing to stub %d symbol(s) that assets/ defines for real: %s'
              % (len(_shadowed), ', '.join(sorted(_shadowed)[:8])))
    syms = [x for x in syms if x not in _defined]
    funcs, data = load_decls()

    out = [
        '/* GENERATED DIAGNOSTIC by tools/gen_link_stubs.py - not part of the port.',
        ' *',
        ' * One stub per still-undefined symbol so the app links and the boot path can be',
        ' * traced. Symbols are classified from the decomp\'s own declarations: DATA gets a',
        ' * real zeroed object, FUNCTIONS get a stub that logs once and returns 0 so the',
        ' * boot continues.',
        ' *',
        ' * Emitting data symbols as functions (the naive approach) makes any read of them',
        ' * return code bytes, which shows up as a SIGBUS that looks like a port bug.',
        ' *',
        ' * Delete once the real implementations land. */',
        '#include <stdio.h>',
        '#include <string.h>',
        '#include <PR/ultratypes.h>',
        '',
        '/* Over-allocated on purpose -- the real sizes are unknown, and a stub that is',
        ' * too SMALL corrupts whatever global follows it (this is exactly how g_Props',
        ' * ate the memory-pool table). Uninitialised, so it lives in BSS and the',
        ' * generous size costs nothing in the binary. */',
        '#define GE_STUB_BYTES  (256 * 1024)',
        '#define GE_STUB_POISON 4096',
        '#define GE_STUB_CANARY 16',
        '#define GE_STUB_MAGIC  "GETVcanary_v1!!"',
        '',
        'static void ge_stub_hit(const char *name)',
        '{',
        '    static const char *seen[1024];',
        '    static int n = 0;',
        '    int i;',
        '    for (i = 0; i < n; i++) {',
        '        if (seen[i] == name) return;',
        '    }',
        '    if (n < 1024) seen[n++] = name;',
        '    printf("[getv] STUB: %s\\n", name);',
        '    fflush(stdout);',
        '}',
        '',
    ]

    n_data = n_func = n_guess = n_passthru = 0
    stub_arrays = []
    for s in syms:
        if s in data and s not in funcs:
            # Emitted as raw bytes rather than with the declared type: these stubs live
            # in a port TU, which deliberately excludes the decomp's include/ (its
            # math.h/string.h/stddef.h shadow the system headers), so game types like
            # StandTilePoint or PropRecord are not visible here. The linker does not care
            # about type; the game's own declaration governs every access.
            # The fill value depends on what the symbol is, and there is no single safe
            # choice:
            #
            #   Tables scanned for a sentinel need 0xFF. image_entries_load() walks
            #   g_Textures until dataoffset == 0xffff; a zero-filled stub never hits the
            #   sentinel and runs off the end of the buffer.
            #
            #   Pointer globals need 0. A 0xFF-filled pointer is 0xFFFF...FF, the most
            #   invalid address possible, and any dereference is an immediate SIGSEGV,
            #   which is how lvlStageLoad died. NULL at least faults predictably, and most
            #   code null-checks.
            typ = data[s][0]
            if '*' in typ:
                out.append('void *%s = 0;   /* pointer global: NULL, not 0xFF */' % s)
            else:
                out.append('unsigned char %s[GE_STUB_BYTES];' % s)
                stub_arrays.append(s)
            n_data += 1
        elif s in funcs:
            # A stub that returns 0 is wrong for the Gfx* display-list chain, which is
            # pervasive in N64 code: `DL = someRenderStep(DL);`. Returning NULL poisons
            # DL and the next gSP/gDP write faults, which is how
            # menu_jump_constructor_handler crashed lvlRender.
            #
            # When a function returns a pointer and takes a pointer first, the correct
            # no-op is to return that argument unchanged. For the display-list family that
            # means "this stage appended nothing", which is exactly right. Taking
            # (void *) and returning it is ABI-safe on AAPCS64 (x0 in, x0 out) even where
            # the real prototype has extra parameters.
            rettype, params = funcs[s]
            first = params.split(',')[0].strip()
            if '*' in rettype and '*' in first and first not in ('void', ''):
                out.append('void *%s(void *passthrough) { ge_stub_hit("%s"); '
                           'return passthrough; }' % (s, s))
                n_passthru += 1
            else:
                out.append('long %s(void) { ge_stub_hit("%s"); return 0; }' % (s, s))
            n_func += 1
        else:
            # No declaration found anywhere. Treat as data: a spurious object is
            # harmless, whereas a spurious function that gets read as data is not.
            out.append('unsigned char %s[GE_STUB_BYTES];   /* unclassified */' % s)
            stub_arrays.append(s)
            n_guess += 1

    # Data stubs are deliberately over-allocated and canaried.
    #
    # A fixed `unsigned char NAME[4096] = {[0...4095] = 0xFF}` is a guess: g_Props is
    # really PropRecord[600] = 24,000 bytes on arm64, so init_load_objpos_table()'s
    # `g_Props[i].prev = &g_Props[i+1]` loop overran it by ~20 KB and wrote a chain of
    # pointers straight through g_mempPools, corrupting the memory-pool bank table. That
    # surfaced only as a silent hang inside mempAllocBytesInBank's `while(1);`, three
    # subsystems away from the cause.
    #
    # Two properties keep that class of bug from being silent:
    #   * uninitialised (BSS), so a generous size costs nothing in the binary, with the
    #     0xFF poison written at runtime instead. The poison still matters, because
    #     tables scanned for a 0xFFFF sentinel run off the end of a zero-filled stub.
    #   * a canary after every stub, checked from gePortBootMark(), so an overflow reports
    #     which symbol and at which boot step instead of corrupting a neighbour.
    out.append('')
    out.append('/* ---- stub storage registry (see comment in gen_link_stubs.py) ---- */')
    out.append('static unsigned char *const ge_stub_ptrs[] = {')
    for a in stub_arrays:
        out.append('    %s,' % a)
    out.append('    0')
    out.append('};')
    out.append('static const char *const ge_stub_names[] = {')
    for a in stub_arrays:
        out.append('    "%s",' % a)
    out.append('    0')
    out.append('};')
    out.append("""
void gePortStubInit(void)
{
    int i;
    for (i = 0; ge_stub_ptrs[i]; i++) {
        /* Poison only the first GE_STUB_POISON bytes: that is where a struct's fields
         * and a table's first entries live, so a sentinel scan still terminates. The
         * tail stays zero and exists purely to absorb overruns. */
        memset(ge_stub_ptrs[i], 0xFF, GE_STUB_POISON);
        memcpy(ge_stub_ptrs[i] + GE_STUB_BYTES - GE_STUB_CANARY, GE_STUB_MAGIC,
               GE_STUB_CANARY);
    }
    printf("[getv] stub storage: %d arrays x %d bytes, poison %d, canaried\\n",
           i, GE_STUB_BYTES, GE_STUB_POISON);
    fflush(stdout);
}

/* Returns the name of the first overflowed stub, or 0. Cheap enough to call from
 * every boot mark, which is what pins an overflow to a single call. */
const char *gePortStubCheck(void)
{
    int i;
    for (i = 0; ge_stub_ptrs[i]; i++) {
        if (memcmp(ge_stub_ptrs[i] + GE_STUB_BYTES - GE_STUB_CANARY, GE_STUB_MAGIC,
                   GE_STUB_CANARY) != 0) {
            return ge_stub_names[i];
        }
    }
    return 0;
}
""")

    io.open(out_path, 'w').write('\n'.join(out) + '\n')
    print('stubs: %d function (%d pointer pass-through), %d data, %d unclassified '
          '(total %d)' % (n_func, n_passthru, n_data, n_guess, len(syms)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
