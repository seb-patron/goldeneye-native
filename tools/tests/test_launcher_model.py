"""Compile the actual launcher model/profile code without SDL, ImGui or game data."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        parser.error(f"C++ compiler not found: {args.cxx}")
    source = (args.source_root / "getv/port/src/ge_launcher.cpp").read_text()
    model = source[source.index("struct Stage {"):source.index("/* Where the game will actually look for mods")]
    profile = re.search(r"void apply_profile\(Model &m\)\s*\{.*?\n\}", source, re.S)
    if not profile:
        raise RuntimeError("production apply_profile function missing")
    harness = r'''
int main() {
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char *name) {
        ++checks;
        printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    Model m{};
    m.profile = 0;
    m.pick_stage = true;
    m.stage_idx = 4;
    m.ruleset = 4;
    m.rs_custom = m.horde = true;
    m.coop_players = 4;
    m.net_mode = 1;
    m.mouse_mode = 0;
    m.framerate = 60;
    m.bind_all[GE_ACT_FIRE] = 3;
    m.msaa = 8;
    m.hd_textures = m.widescreen = true;
    m.gibs = 3;
    m.blood = 2;
    m.fullscreen = true;
    strcpy(m.resolution, "1920x1080");
    for (int i = 0; i < kCheatCount; ++i) m.cheat_on[i] = true;
    apply_profile(m);
    check(m.pick_stage, "Base Game permits direct mission startup");
    check(m.base_game, "Base Game suppresses Brutal effects");
    check(m.ruleset == 0 && !m.rs_custom && !m.horde, "Base Game uses original gameplay");
    check(m.coop_players == 0 && m.net_mode == 0, "Base Game disables added multiplayer modes");
    check(m.mouse_mode == 0 && m.bind_all[GE_ACT_FIRE] == 3 && m.framerate == 60,
          "Base Game preserves mouse, bindings and selected frame rate");
    check(m.msaa == 0 && !m.hd_textures && !m.widescreen && m.filtering == 2,
          "Base Game shows original image settings");
    bool no_cheats = true;
    for (int i = 0; i < kCheatCount; ++i) no_cheats &= !m.cheat_on[i];
    check(no_cheats, "Base Game clears launcher cheat selection");
    check(m.fullscreen && strcmp(m.resolution, "1920x1080") == 0, "Base Game preserves display choices");
    check(m.stage_idx == 4, "Base Game retains the inactive selected mission");
    check(m.gibs == 3 && m.blood == 2, "Base Game retains inactive Brutal preferences");
    m.profile = 1;
    m.mouse_mode = 1;
    apply_profile(m);
    check(m.msaa >= 4 && m.hd_textures && m.base_game && m.mouse_mode == 1, "GoldenEye+ enhances graphics without enabling Brutal effects");
    m.pick_stage = true;
    m.ruleset = 2;
    m.horde = true;
    apply_profile(m);
    check(m.pick_stage && m.ruleset == 2 && m.horde, "GoldenEye+ allows custom mission and gameplay");
    printf("launcher model: %d checks, %d failures\n", checks, failures);
    return failures != 0 || checks != 12;
}
'''
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="launcher-model-") as directory:
        root = Path(directory)
        cpp, exe = root / "model.cpp", root / "model.exe"
        # ge_actions.h is included rather than having its constants restated here.
        # `struct Model` sizes its binding arrays with GE_ACT_MAX and GE_AXIS_MAX, and a
        # local copy of those numbers would compile happily while testing a Model that
        # is a different shape from the real one -- which is exactly the drift the
        # header exists to prevent. It includes nothing itself, so pulling it in costs
        # no SDL, no ImGui and no game data.
        cpp.write_text(
            '#include <cstdio>\n#include <cstring>\n#include "ge_actions.h"\n'
            + model + profile.group() + harness
        )
        include = args.source_root / "getv/port/src"
        subprocess.run(
            [compiler, "-std=c++17", f"-I{include}", str(cpp), "-o", str(exe)],
            env=env, check=True,
        )
        subprocess.run([str(exe)], env=env, check=True)


if __name__ == "__main__":
    main()
