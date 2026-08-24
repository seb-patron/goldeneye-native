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
import glob, os, re, subprocess, sys, tempfile

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
          '-D_LANGUAGE_C=1','-w','-ferror-limit=0','-fno-strict-aliasing','-O1']

def globals_of(rel):
    with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as t: o = t.name
    r = subprocess.run(['clang']+CFLAGS+['-c',rel,'-o',o],cwd=ROOT,capture_output=True,text=True)
    if r.returncode != 0:
        if os.path.exists(o): os.unlink(o)
        return None
    out = subprocess.run(['nm','-g',o],capture_output=True,text=True).stdout
    if os.path.exists(o): os.unlink(o)
    syms=[]
    for line in out.splitlines():
        p=line.split()
        if len(p)==3 and p[1] in ('D','B','S','T','C') and p[2].startswith('_'):
            syms.append(p[2][1:])
    return syms


# Getools opens each stan file with a bare `StandTile <name>_tile_0;` under a "forward
# declarations" comment. In C that is a tentative definition, not a declaration, so the
# compiler allocates tile_0 at that point in the file and the 24-byte StandFileHeader lands
# between tile_0 and tile_1.
#
# The engine walks the tiles as one contiguous run, adding each tile's byte size and
# computing pointers as base + (link << 3), so anything wedged between them breaks the walk
# at the first step. On Windows it produced a one-tile level: the player spawned in room 0,
# which has no geometry, and the world did not draw at all.
#
# Adding extern makes it a declaration and the storage is allocated where the definition
# actually appears. Applied here rather than to the generated files directly, because those
# are regenerated from the ROM and any edit to them is lost on the next run.
_FWD_TILE = re.compile(r'^(StandTile\s+[A-Za-z_][A-Za-z0-9_]*_tile_0\s*;)\s*$', re.M)


def _extern_forward_decls(text):
    return _FWD_TILE.sub(lambda m: 'extern ' + m.group(1), text)


def main():
    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
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
        syms = globals_of(rel)
        if syms is None:
            skipped.append(rel)
            print('SKIP (does not compile):', rel); continue
        # The file's own asset symbol is looked up by name by the engine -- leave it.
        keep = {stem}
        todo = sorted([s for s in syms if s not in keep and not s.startswith(prefix+'_')],
                      key=len, reverse=True)
        if not todo:
            print('ok (already namespaced):', rel); continue
        s = open(f).read()
        for sym in todo:
            s = re.sub(r'\b'+re.escape(sym)+r'\b', prefix+'_'+sym, s)
        s = _extern_forward_decls(s)
        if not dry:
            open(f,'w').write(s)
        renamed.append((rel, len(todo)))
        print(f'{rel}: {len(todo)} symbols namespaced')

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
