#!/usr/bin/env python3
"""Build the patched native model-slot contract against synthetic storage, without assets."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DECOMP_REV = "c4356466796c697dfd298010b9bed261f9ed8c6a"
DECOMP_URL = "https://github.com/n64decomp/007.git"


def run(args, cwd=None):
    print("+ " + " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"),
                        help="C compiler executable (MinGW gcc.exe on Windows)")
    parser.add_argument("--source-repo", default=DECOMP_URL,
                        help="upstream Git URL or local source repository containing the pinned commit")
    parser.add_argument("--patch-root", type=Path, default=ROOT,
                        help="repository whose source patches are tested")
    args = parser.parse_args()
    source_repo = Path(args.source_repo)
    if source_repo.exists():
        source_repo = source_repo.resolve()
    else:
        source_repo = args.source_repo

    with tempfile.TemporaryDirectory(prefix="ge-model-slots-") as temporary:
        scratch = Path(temporary)
        run(["git", "init", "-q", scratch])
        run(["git", "-c", "core.autocrlf=false", "fetch", "--quiet", "--depth=1",
             source_repo, DECOMP_REV], scratch)
        run(["git", "-c", "core.autocrlf=false", "checkout", "FETCH_HEAD", "--",
             "src", "include", "assets/images.def"], scratch)

        patches = sorted((args.patch_root.resolve() / "getv/patches").glob("0*.patch"))
        if not patches:
            raise SystemExit("No patches found; refusing an unpatched test")
        for patch in patches:
            if patch.name.startswith("0002-"):
                continue
            run(["git", "apply", "--include=src/*", "--include=include/*", patch], scratch)

        flags = ["-std=gnu17", "-fms-extensions", "-fno-strict-aliasing", "-O1",
                 "-DVERSION_US", "-DLANG_US", "-DREFRESH_NTSC", "-DLEFTOVERDEBUG",
                 "-DLEFTOVERSPECTRUM", "-DBUGFIX_R0", "-DTARGET_N64", "-DGE_PORT_NATIVE",
                 "-DNON_MATCHING=1", "-DAVOID_UB=1", "-D_LANGUAGE_C=1",
                 "-Werror=return-type", "-D_FORTIFY_SOURCE=0"]
        for directory in [".", "include", "include/PR", "src", "src/game", "src/inflate"]:
            flags += ["-I", directory]
        if os.name == "nt":
            flags += ["-mno-ms-bitfields"]
            compiler_dir = str(Path(args.cc).resolve().parent)
            os.environ["PATH"] = compiler_dir + os.pathsep + os.environ.get("PATH", "")

        executable = scratch / ("model-slots.exe" if os.name == "nt" else "model-slots")
        run([args.cc, *flags, ROOT / "getv/port/tests/game/test_model_slot_lifecycle.c",
             "-o", executable], scratch)
        run([executable], scratch)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
