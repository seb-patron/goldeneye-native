#!/usr/bin/env python3
"""Give every generated asset translation unit its own global-symbol namespace.

Why this exists
---------------
Getools emits each asset as plain C with generic, non-static global names -- `tile_0`,
`padlist`, `intro`, `ai_0`, `path_table_7`, `footer` and so on -- and emits the same
names in every level's file. Linked together, all 29 stan files and all 50 setup files
define the same symbols, so an internal reference such as

    StandFileHeader Tbg_dam_all_p_stanZ = { NULL, &tile_0, ... };
    stagesetup      UsetupdamZ          = { &pathwaypoints, ..., &padlist, ... };

binds to whichever object the linker picked, which is alphabetically the first. Before
this pass existed, the Dam's stan header resolved to Tbg_ame's tile_0 and the Dam's pad
list resolved to a 62-entry list belonging to another level (the Dam has 368). Every
level was running on some other level's data.

What it does
------------
For each .c under the given directory: compile it, read its defined global symbols with
nm, and rewrite every one of them (except the file's own top-level asset symbol, which
the engine looks up by name) to `<filestem>_<name>`. Symbols stay extern so the linker
cannot drop them, and their declaration order -- which the stan tile walk depends on for
contiguity -- is untouched.

A file that does not compile is left colliding. This pass can only read a file's globals
by compiling it, so a failed compile is a silent gap: one "SKIP" line among hundreds of
"ok" lines. That is how `assets/obseg/setup/UsetuparchZ.c` and all three
`setup/{u,j,e}/UsetuplenZ.c` kept a bare `propDefs` (plus 243 and 9 other bare globals)
long after every other setup file had been namespaced, leaving ARCHIVES and CUBA sharing
one propDef stream that ARCHIVES then walked off the end of. The skip list is therefore
printed as a block at the end and the tool exits non-zero.

`--dry-run` also exits non-zero when anything is left to rename, so it works as a
regression gate:  uniquify_asset_symbols.py assets/obseg/setup --dry-run

Language-variant dirs need `--recurse` from the parent, not a direct path. `setup/u`,
`setup/j` and `setup/e` hold the same filenames. Passing `setup/u` directly makes
`parent == basename(dir)`, so the prefix collapses to the bare file stem
(`UsetuplenZ_`), identical in all three dirs, which is no fix at all. Run
`... assets/obseg/setup --recurse` instead; that yields `u_UsetuplenZ_`.

Not fixed by this tool: each language dir also defines the file's own top-level symbol
(three `stagesetup UsetuplenZ`). That one is deliberately kept unprefixed because the
engine looks it up by name, so the linker picks whichever comes first -- alphabetically
`e/`, the PAL setup, in a VERSION_US build. The fix for that is to stop compiling the
non-US language dirs, not to rename here.

Usage:  uniquify_asset_symbols.py <dir> [--recurse] [--dry-run]
"""
import glob, os, re, shutil, subprocess, sys, tempfile

def _host_flags():
    """Target and sysroot for the throwaway probe objects, or nothing off Apple.

    This only ever compiles an object that nm is run over and then deleted, so the target
    does not have to match the real build. On macOS it does need a sysroot, and macOS is
    tried before the tvOS simulator because the Command Line Tools alone provide it.

    Off Apple there is no xcrun. Calling it there leaves the path empty and `-isysroot ''`
    makes every compile fail, which this tool reports as a SKIP for every file -- it then
    renames nothing while appearing to succeed. So on Linux and elsewhere, pass neither a
    target nor a sysroot and let the system compiler use its defaults.
    """
    if sys.platform != 'darwin':
        return []
    for sdk, target in (('macosx', 'arm64-apple-macos13.0'),
                        ('appletvsimulator', 'arm64-apple-tvos17.0-simulator')):
        r = subprocess.run(['xcrun', '-sdk', sdk, '--show-sdk-path'],
                           capture_output=True, text=True)
        path = r.stdout.strip()
        if r.returncode == 0 and path and os.path.isdir(path):
            return ['-target', target, '-isysroot', path]
    sys.exit("no usable SDK on macOS: neither 'xcrun -sdk macosx' nor 'xcrun -sdk "
             "appletvsimulator' resolved. Install the Xcode Command Line Tools "
             "(xcode-select --install) before running this.")


HOST_FLAGS = _host_flags()
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vendor', 'ge-decomp')
ROOT = os.path.normpath(ROOT)
CFLAGS = HOST_FLAGS + [
          '-fms-extensions','-include','src/ge_port_decls.h',
          '-I','.','-I','include','-I','include/PR','-I','src','-I','src/game','-I','src/inflate',
          '-DVERSION_US','-DLANG_US','-DREFRESH_NTSC','-DLEFTOVERDEBUG','-DLEFTOVERSPECTRUM',
          '-DBUGFIX_R0','-DTARGET_N64','-DGE_PORT_NATIVE','-DNON_MATCHING=1','-DAVOID_UB=1',
          '-D_LANGUAGE_C=1','-w','-fno-strict-aliasing','-O1']

# The compiler and nm are resolved rather than assumed, because this used to hardcode 'clang'
# and that is not a safe assumption anywhere the project actually builds. The documented Windows
# toolchain is mingw-w64 gcc and ships no clang at all, so on a machine set up exactly as
# docs/SETUP.md says, subprocess.run raised FileNotFoundError before there was any returncode to
# test -- the `if r.returncode != 0` guard below never saw it, and the whole pass died on a
# traceback at the first file rather than reporting anything useful.
#
# GETV_CC and GETV_NM override, in case a machine has something the search does not find.
#
# nm has to read what the compiler just wrote, so an nm sitting beside the compiler wins over
# anything earlier on PATH. Pairing mingw gcc's COFF objects with some unrelated nm is how the
# pass goes straight back to reporting zero globals for every file -- the same silent no-op,
# reached by a third route. That preference, and the mingw location below, are the Windows
# lane's, who found the original hardcoding.
_TOOL_DIRS = [r'C:\mingw64\bin'] if os.name == 'nt' else []

def _resolve(env_var, names, prefer_dir=None):
    # GE_CC/GE_NM are accepted alongside GETV_CC/GETV_NM: the Windows lane wrote and documented
    # the shorter spelling, and breaking it to enforce this repository's GETV_ convention would
    # cost someone a confusing afternoon for no gain.
    override = os.environ.get(env_var) or os.environ.get(env_var.replace('GETV_', 'GE_', 1))
    if override:
        found = shutil.which(override) or (override if os.path.isfile(override) else None)
        if not found:
            sys.exit("uniquify_asset_symbols: %s is set to %r, which does not resolve to an "
                     "executable" % (env_var, override))
        return found
    for d in ([prefer_dir] if prefer_dir else []) + _TOOL_DIRS:
        for n in names:
            p = os.path.join(d, n + ('.exe' if os.name == 'nt' else ''))
            if os.path.isfile(p):
                return p
    for n in names:
        found = shutil.which(n)
        if found:
            return found
    return None

CC = _resolve('GETV_CC', ['clang', 'cc', 'gcc'])
if CC is None:
    sys.exit("uniquify_asset_symbols: no C compiler found. Looked for clang, cc and gcc on PATH"
             + (" and in C:\\mingw64\\bin" if os.name == 'nt' else "")
             + ".\nThis pass compiles each asset and reads its globals with nm, so it cannot run\n"
             "without one. Install it, or set GETV_CC to its path.")
NM = _resolve('GETV_NM', ['nm', 'llvm-nm', 'gcc-nm'], prefer_dir=os.path.dirname(CC))
if NM is None:
    sys.exit("uniquify_asset_symbols: found a compiler at %s but no nm beside it or on PATH.\n"
             "Set GETV_NM to the nm that belongs to that toolchain." % CC)

# -ferror-limit=0 is a clang spelling. gcc rejects it and wants -fmax-errors=0, the same split
# build_windows.ps1 already documents for -Wno-everything. Ask the compiler what it is rather
# than inferring from its filename, since cc is usually a symlink to one or the other.
try:
    _v = subprocess.run([CC, '--version'], capture_output=True, text=True).stdout.lower()
except OSError as e:
    sys.exit("uniquify_asset_symbols: cannot run %s: %s" % (CC, e))
# -ferror-limit=0 is a clang spelling. gcc rejects it and wants -fmax-errors=0, the same split
# build_windows.ps1 already documents for -Wno-everything. Ask the compiler what it is rather
# than inferring from its filename, since cc is usually a symlink to one or the other.
try:
    _v = subprocess.run([CC, '--version'], capture_output=True, text=True).stdout.lower()
except OSError as e:
    sys.exit("uniquify_asset_symbols: cannot run %s: %s" % (CC, e))
# -std=gnu17 on both, and it is not cosmetic. GCC 16 defaults to C23, where bool is a keyword,
# so bondtypes.h:85's `typedef s32 bool` is a syntax error and EVERY file fails to compile.
# Every file then becomes a SKIP, the pass renames nothing, and it looks like it worked -- the
# same silent no-op the underscore bug caused, arrived at from a different direction. Measured
# on mingw gcc 16.2 by the Windows lane.
#
# -fpermissive is gcc-only and clang rejects the flag outright. GCC 14 promoted
# incompatible-pointer-types from a warning to an error, which throws out the beta-stan file
# Tbg_cat_all_p_stanZ.c (Caves) on its own. One skipped stan file is one level silently bound
# to another level's data, which is exactly what this whole pass exists to prevent.
if 'clang' in _v:
    CFLAGS += ['-ferror-limit=0', '-std=gnu17']
else:
    CFLAGS += ['-fmax-errors=0', '-std=gnu17', '-fpermissive']

# nm writes a leading underscore on every C symbol on Mach-O and nothing at all on ELF. Reading
# for the wrong one is invisible: every file reports zero globals, every file is then declared
# already namespaced, and the whole pass becomes a no-op that only surfaces as duplicate-symbol
# link errors an hour later. So the prefix is measured against a probe whose symbol is known
# rather than assumed, and a probe that comes back empty is a hard error -- a tool that cannot
# read symbols must say so, not return an empty set that looks like success.
def nm_prefix():
    with tempfile.NamedTemporaryFile(suffix='.c', mode='w', delete=False) as t:
        t.write('int getv_nm_probe = 1;\n'); c = t.name
    o = c[:-2] + '.o'
    r = subprocess.run([CC,'-c',c,'-o',o], capture_output=True, text=True)
    if r.returncode != 0:
        os.unlink(c)
        sys.exit('uniquify_asset_symbols: cannot compile a probe file:\n' + r.stderr.strip())
    out = subprocess.run([NM,'-g',o], capture_output=True, text=True).stdout
    os.unlink(c)
    if os.path.exists(o): os.unlink(o)
    for line in out.splitlines():
        w = line.split()
        if w and w[-1].endswith('getv_nm_probe'):
            return w[-1][:-len('getv_nm_probe')]
    sys.exit('uniquify_asset_symbols: nm reported no getv_nm_probe in a probe object -- '
             'symbol reading is broken, refusing to run')

NM_PREFIX = nm_prefix()

def globals_of(rel):
    with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as t: o = t.name
    r = subprocess.run([CC]+CFLAGS+['-c',rel,'-o',o],cwd=ROOT,capture_output=True,text=True)
    if r.returncode != 0:
        if os.path.exists(o): os.unlink(o)
        return None
    out = subprocess.run([NM,'-g',o],capture_output=True,text=True).stdout
    if os.path.exists(o): os.unlink(o)
    syms=[]
    for line in out.splitlines():
        p=line.split()
        if len(p)==3 and p[1] in ('D','B','S','T','C') and p[2].startswith(NM_PREFIX):
            syms.append(p[2][len(NM_PREFIX):])
    return syms

# A file-scope `Type name;` that has a real `Type name = {...}` later in the SAME file is a
# tentative definition, not a forward declaration. C allocates the object at the point of the
# tentative definition, so the object lands where the "declaration" is rather than where its
# initialiser is.
#
# In the stan assets that matters and is wrong. Every generated stan file opens with
#
#     StandTile Tbg_sev_all_p_stanZ_tile_0;      <- under a "// forward declarations" comment
#     StandFileHeader Tbg_sev_all_p_stanZ = { NULL, &..._tile_0, ... };
#     StandTile Tbg_sev_all_p_stanZ_tile_0 = { ... };
#     StandTile Tbg_sev_all_p_stanZ_tile_1 = { ... };
#
# and the stan format requires the tiles to be ONE CONTIGUOUS RUN in declaration order -- the
# engine walks it by adding each tile's byte size and resolves links as base + (link << 3).
# The tentative definition put tile_0 first and left the 24-byte StandFileHeader sitting
# between tile_0 and tile_1, breaking the run at the first step.
#
# This pass is here rather than in the asset output because vendor/ is gitignored: the
# generated assets do not travel between machines, each side regenerates its own, and a fix
# applied to the output is destroyed by the next regeneration. See docs/WINDOWS_STAN_ORDERING.md.
#
# Only rewritten when a real definition of the same name exists later in the file, which is
# what makes the line a tentative definition rather than a genuine extern reference. Lines
# containing '(' are skipped so function declarations are never touched, and the match is
# anchored at column 0 so nothing inside a function body can match.
FWD_RE = re.compile(r'^([A-Za-z_]\w*(?:[ \t]+[A-Za-z_]\w*)*)([ \t]+\*?)([A-Za-z_]\w*)[ \t]*;[ \t]*$')

def externise_forward_decls(s):
    out, n = [], 0
    for line in s.splitlines(True):
        body = line.rstrip('\r\n')
        m = FWD_RE.match(body)
        if m and '(' not in body and '=' not in body:
            typ, name = m.group(1), m.group(3)
            first = typ.split()[0]
            if first not in ('extern', 'static', 'typedef', 'return', 'goto') and \
               re.search(r'^\s*(?:\w+[ \t]+)*\*?' + re.escape(name) + r'[ \t]*(?:\[[^\]]*\])?[ \t]*=',
                         s, re.M):
                line = 'extern ' + line.lstrip()
                n += 1
        out.append(line)
    return ''.join(out), n


KNOWN_FLAGS = ('--dry-run', '--forward-decls-only', '--recurse')

def main():
    # Refuse an argument we do not recognise instead of ignoring it. This pass is destructive
    # when it runs twice -- a second run prefixes the already-prefixed names and breaks exactly
    # the files the first run fixed -- so a mistyped guard flag is not a harmless typo. `--dry`
    # instead of `--dry-run` silently did the real thing to two font assets, which then had to
    # be repaired by hand; every flag here is now checked rather than merely looked for.
    unknown = [a for a in sys.argv[2:] if a.startswith('-') and a not in KNOWN_FLAGS]
    if unknown:
        sys.exit("uniquify_asset_symbols: unrecognised argument(s): %s\n"
                 "Known flags are %s. Refusing to run, because this pass rewrites files in\n"
                 "place and running it twice corrupts what the first run fixed."
                 % (' '.join(unknown), ', '.join(KNOWN_FLAGS)))

    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
    # --forward-decls-only: run just the tentative-definition pass, which is pure text and
    # needs no compiler. The rename pass shells out to clang and nm; those are not present on
    # every machine that has to build these assets, and this pass must be runnable there too.
    fwd_only = '--forward-decls-only' in sys.argv
    # --recurse: the model assets are laid out as <dir>/<name>/Model.c -- 103 different
    # props all defining ModelNode_0x048, each its own translation unit, which is the
    # real collision. A flat glob misses them entirely, and the flat prefix rule would
    # give every one of them the prefix "Model" because the file stems are identical.
    # When recursing, the subdirectory name is what makes a file unique, so use it.
    if '--recurse' in sys.argv:
        files = sorted(glob.glob(os.path.join(ROOT,d,'*','*.c')))
    else:
        files = sorted(glob.glob(os.path.join(ROOT,d,'*.c')))
    # Never touch .inc.c: they are #included into another TU, so their symbols are
    # referenced from outside the file and a rename here breaks those references. Several
    # are macro invocations (MODELFILEHEADER(wppkmag, ...)) where a regex rename would
    # mangle the macro argument. Only standalone translation units can be namespaced
    # safely, and those are the ones that actually collide at link time.
    files = [f for f in files if not f.endswith('.inc.c')]
    skipped = []
    renamed = []
    for f in files:
        rel = os.path.relpath(f, ROOT)
        stem = os.path.basename(f)[:-2]
        # Region variants live in sibling dirs (setup/e, setup/j, setup/u) under the same
        # filename, so the file stem alone is not unique; qualify it with the directory.
        parent = os.path.basename(os.path.dirname(f))
        if '--recurse' in sys.argv:
            prefix = parent + '_' + stem      # uzimag_Model, desk_lamp2_Model, ...
        else:
            prefix = stem if parent == os.path.basename(d.rstrip('/')) else parent + '_' + stem
        todo = []
        if not fwd_only:
            syms = globals_of(rel)
            if syms is None:
                skipped.append(rel)
                print('SKIP (does not compile):', rel); continue
            # The file's own asset symbol is looked up by name by the engine -- leave it.
            keep = {stem}
            todo = sorted([s for s in syms if s not in keep and not s.startswith(prefix+'_')],
                          key=len, reverse=True)

        # Explicit utf-8, and newline='' on the write. Python's default encoding is the
        # locale's, which is cp1252 on Windows, and the generated headers contain non-ASCII --
        # so a plain open() raises UnicodeDecodeError on every asset there while working fine
        # on the Mac. newline='' stops Python translating LF to CRLF on the way out, which
        # would rewrite every line of every asset it touched: nothing in this repository sets
        # core.autocrlf, and git must not be handed rewritten endings.
        s = open(f, encoding='utf-8').read()
        for sym in todo:
            s = re.sub(r'\b'+re.escape(sym)+r'\b', prefix+'_'+sym, s)
        # Always, even when the rename found nothing to do: a re-run over an
        # already-namespaced tree must still fix the tentative definitions, or the pass is
        # a no-op exactly on the trees that have been set up once already.
        s, nfwd = externise_forward_decls(s)

        if not todo and not nfwd:
            print('ok (already namespaced):', rel); continue
        if not dry:
            open(f, 'w', encoding='utf-8', newline='').write(s)
        renamed.append((rel, len(todo) + nfwd))
        bits = []
        if todo: bits.append(f'{len(todo)} symbols namespaced')
        if nfwd: bits.append(f'{nfwd} tentative definition(s) made extern')
        print(f'{rel}: ' + ', '.join(bits))

    # Terminal summary; see the module docstring. A silent SKIP is how UsetuparchZ.c and
    # UsetuplenZ.c stayed collided.
    rc = 0
    if skipped:
        print()
        print('=' * 72)
        print(f'{len(skipped)} FILE(S) DID NOT COMPILE AND ARE STILL COLLIDING:')
        for rel in skipped:
            print('   ', rel)
        print('Their globals are unknown, so they keep the generic Getools names and')
        print('will bind to another level\'s data at link time. Make them compile, then')
        print('re-run this pass. Do not ship a build with this list non-empty.')
        print('=' * 72)
        rc = 1
    if dry and renamed:
        print()
        print(f'--dry-run: {len(renamed)} file(s) still need namespacing.')
        rc = 1
    sys.exit(rc)

main()
