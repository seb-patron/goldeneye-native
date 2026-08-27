/* The setup wizard.
 *
 * What this is. A small, standalone window that runs before anything else exists: it has no
 * dependency on the decomp, the ROM, or the built game, because its whole job is to get from
 * "freshly cloned repository" to "goldeneye.exe exists" for someone who does not want to open
 * a terminal. It picks the user's ROM, verifies it, then runs tools/setup-windows.sh (the same
 * pipeline a git-bash user would type by hand) and shows its output live.
 *
 * ---------------------------------------------------------------- why this is a second binary
 *
 * ge_launcher.cpp cannot do this job itself, however tempting it looks from the outside -- it
 * is compiled into goldeneye.exe, and goldeneye.exe does not exist until after a ROM-dependent
 * build has already happened once (docs/LICENSING.md section 5: assets are compiled in, not
 * loaded at runtime). Something has to exist before that first build to drive it. This is that
 * something, and once it succeeds it hands off to the real launcher and gets out of the way.
 *
 * Contains zero ROM-derived or decomp-derived code, on purpose: it is legally distributable on
 * its own by the same reasoning docs/LICENSING.md section 5 applies to the game binary in the
 * other direction. It statically links its own SDL2 + Dear ImGui, built once by a developer
 * from this repo's own already-fetched toolchain, the same way ge_launcher.cpp does -- see that
 * file for the init sequence this one mirrors.
 *
 * ---------------------------------------------------------------- why raw Win32 for the process
 *
 * tools/setup-windows.sh can take upward of ten minutes (toolchain fetch, asset extraction, a
 * full compile) and has to keep printing into the log the whole time without freezing the
 * window. That needs the child's stdout read off the UI thread. CreateProcess with an inherited
 * pipe handle and a plain CreateThread reader is the direct, well-understood way to do that on
 * Windows; std::thread would work too but buys nothing here, and every other OS-facing helper
 * in this port already talks to Win32 directly rather than through an abstraction layer (see
 * self_path() below, copied from ge_launcher.cpp's own).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#define GLEW_STATIC
#include <GL/glew.h>

/* SDL_MAIN_HANDLED: this is a standalone int main(), not run through SDL2main/WinMain. The
 * real build drops -lSDL2main for the same reason (build_windows.ps1's $sdlLibs filter) --
 * matched here rather than diverging, so this links against the exact same toolchain output. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#include "sha1.h"

extern "C" void gePortSetWindowIcon(SDL_Window *w);

namespace {

const char *kWantSha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83";
const long  kWantSize = 12582912;

/* ---------------------------------------------------------------- paths
 *
 * Same technique as ge_launcher.cpp's self_path(): argv[0] is not trustworthy, so the OS is
 * asked directly. */
bool self_path(char *out, size_t n)
{
    DWORD r = GetModuleFileNameA(NULL, out, (DWORD) n);
    return r > 0 && r < n;
}

bool file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool env_true(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && *v != '\0' && *v != '0';
}

/* Walks up from this exe looking for tools/setup-windows.sh -- the exact file this wizard is
 * about to run, so "found the marker" and "can actually proceed" are the same check. Not
 * hardcoded as "N levels up" because the build output directory is exactly the kind of thing
 * that moves once and silently breaks a fixed offset. */
bool find_repo_root(char *out, size_t n)
{
    char dir[MAX_PATH];
    if (!self_path(dir, sizeof dir)) return false;
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char marker[MAX_PATH];
        snprintf(marker, sizeof marker, "%s\\tools\\setup-windows.sh", dir);
        if (file_exists(marker)) {
            snprintf(out, n, "%s", dir);
            return true;
        }
        char *s = strrchr(dir, '\\');
        if (!s) break;
        *s = '\0';
    }
    return false;
}

/* Deliberately does NOT SearchPathA for "bash.exe" -- confirmed by hand that on any machine
 * with WSL present (which is most Windows 10/11 machines, whether or not a distro is actually
 * installed), C:\Windows\System32\bash.exe is a real file with that exact name, and System32
 * is searched ahead of Git's own directory. It runs, prints "Windows Subsystem for Linux has
 * no installed distributions" as UTF-16 to stderr, and exits 1 -- which this wizard's line
 * reader, expecting UTF-8/ASCII, turns into a single garbled character with no indication of
 * what actually happened. Known Git for Windows layouts first, then git.exe -- which has no
 * same-named WSL stub to collide with -- and bash.exe derived as its sibling. */
bool find_bash_exe(char *out, size_t n)
{
    static const char *candidates[] = {
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (file_exists(candidates[i])) { snprintf(out, n, "%s", candidates[i]); return true; }
    }

    char gitPath[MAX_PATH];
    DWORD r = SearchPathA(NULL, "git.exe", NULL, (DWORD) sizeof gitPath, gitPath, NULL);
    if (r > 0 && r < sizeof gitPath) {
        /* .../Git/cmd/git.exe -> .../Git/bin/bash.exe */
        char *cmdDir = strrchr(gitPath, '\\');
        if (cmdDir != NULL) {
            *cmdDir = '\0';
            char *gitRoot = strrchr(gitPath, '\\');
            if (gitRoot != NULL) {
                *gitRoot = '\0';
                char candidate[MAX_PATH];
                snprintf(candidate, sizeof candidate, "%s\\bin\\bash.exe", gitPath);
                if (file_exists(candidate)) { snprintf(out, n, "%s", candidate); return true; }
            }
        }
    }
    return false;
}

/* ---------------------------------------------------------------- ROM verification
 *
 * Mirrors docs/SETUP.md 3.3/3.4 exactly, including which specific wrong-file case gets which
 * message -- "byte-swapped" and "wrong region" call for different fixes, and collapsing them
 * into one generic "invalid ROM" would send a correct .v64 dump down the wrong repair path. */
struct RomCheck {
    bool ok;
    char message[512];
};

RomCheck verify_rom(const char *path)
{
    RomCheck r;
    r.ok = false;
    r.message[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(r.message, sizeof r.message, "Could not open that file.");
        return r;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char header[4] = { 0, 0, 0, 0 };
    fread(header, 1, 4, f);
    bool byteswapped = (header[0] == 0x37 && header[1] == 0x80);

    if (size != kWantSize) {
        if (byteswapped) {
            snprintf(r.message, sizeof r.message,
                "This looks like a byte-swapped dump (.v64/.n64), not a plain .z64. It needs "
                "converting to native big-endian z64 before this will work.");
        } else {
            snprintf(r.message, sizeof r.message,
                "Wrong size: %ld bytes, expected %ld. This may be trimmed, padded, or carry a "
                "dumper header rather than being a plain cartridge dump.", size, kWantSize);
        }
        fclose(f);
        return r;
    }

    if (byteswapped) {
        snprintf(r.message, sizeof r.message,
            "This is byte-swapped (a .v64/.n64 dump) even though the size matches. It needs "
            "converting to native big-endian z64 before this will work.");
        fclose(f);
        return r;
    }
    if (!(header[0] == 0x80 && header[1] == 0x37 && header[2] == 0x12 && header[3] == 0x40)) {
        snprintf(r.message, sizeof r.message,
            "Right size, but the header doesn't match a GoldenEye 007 ROM.");
        fclose(f);
        return r;
    }

    sha1_ctx ctx;
    sha1_init(&ctx);
    fseek(f, 0, SEEK_SET);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha1_update(&ctx, buf, n);
    fclose(f);

    unsigned char digest[20];
    char hex[41];
    sha1_final(&ctx, digest);
    sha1_hex(digest, hex);

    if (strcmp(hex, kWantSha1) != 0) {
        snprintf(r.message, sizeof r.message,
            "Right size, but the checksum doesn't match (got %s). This may be a different "
            "region or a modified ROM -- only the US release is supported.", hex);
        return r;
    }

    r.ok = true;
    snprintf(r.message, sizeof r.message, "Verified: matches the official US GoldenEye 007 ROM.");
    return r;
}

bool open_rom_dialog(char *out, size_t n)
{
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "N64 ROM (*.z64)\0*.z64\0All files\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    /* OFN_NOCHANGEDIR: GetOpenFileName's legacy side effect of changing the process's current
     * directory would otherwise break every relative path this wizard resolves afterward. */
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Select your GoldenEye 007 (U) ROM";

    if (GetOpenFileNameA(&ofn)) {
        snprintf(out, n, "%s", buf);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- pipeline
 *
 * One child process (tools/setup-windows.sh via bash), its combined stdout+stderr read on a
 * background thread into a line buffer the UI thread drains every frame under a critical
 * section. "== step name ==" banners -- setup-windows.sh's own step() function -- are picked
 * out of the stream to drive the one-line status above the log, rather than trying to compute
 * real progress out of output that mixes bash, python3 and a PowerShell build's own counters. */
struct Pipeline {
    HANDLE hProcess;
    HANDLE hReadPipe;
    CRITICAL_SECTION lock;
    std::vector<std::string> lines;
    std::string partial;
    std::string currentStep;
    bool started;
    bool finished;
    DWORD exitCode;

    Pipeline() : hProcess(NULL), hReadPipe(NULL), started(false), finished(false), exitCode(1) {}
};

DWORD WINAPI reader_thread(LPVOID param)
{
    Pipeline *p = (Pipeline *) param;
    char buf[4096];
    DWORD n = 0;

    while (ReadFile(p->hReadPipe, buf, sizeof buf, &n, NULL) && n > 0) {
        EnterCriticalSection(&p->lock);
        p->partial.append(buf, n);
        size_t pos;
        while ((pos = p->partial.find('\n')) != std::string::npos) {
            std::string line = p->partial.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 6 && line.compare(0, 3, "== ") == 0 &&
                line.compare(line.size() - 3, 3, " ==") == 0) {
                p->currentStep = line.substr(3, line.size() - 6);
            }
            p->lines.push_back(line);
            p->partial.erase(0, pos + 1);
        }
        LeaveCriticalSection(&p->lock);
    }

    EnterCriticalSection(&p->lock);
    if (!p->partial.empty()) {
        p->lines.push_back(p->partial);
        p->partial.clear();
    }
    LeaveCriticalSection(&p->lock);

    WaitForSingleObject(p->hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p->hProcess, &code);

    EnterCriticalSection(&p->lock);
    p->exitCode = code;
    p->finished = true;
    LeaveCriticalSection(&p->lock);
    return 0;
}

bool start_pipeline(Pipeline *p, const char *repoRoot, std::string *err)
{
    char bash[MAX_PATH];
    if (!find_bash_exe(bash, sizeof bash)) {
        *err = "Could not find bash.exe. This step needs Git for Windows -- install it from "
               "git-scm.com (the default install options are fine) and try again.";
        return false;
    }

    /* p->lock is initialized once in main(), before this is ever called -- see the comment
     * there. */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hWrite = NULL;
    if (!CreatePipe(&p->hReadPipe, &hWrite, &sa, 0)) {
        *err = "Could not create a pipe for the setup script's output.";
        return false;
    }
    SetHandleInformation(p->hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    /* GETV_WIZARD_TEST_CMD overrides what actually runs, quoted exactly as given. The pipe and
     * reader-thread plumbing is the one part of this file with no UI to click through by hand
     * -- this is what lets it be exercised against a real child process without needing a real
     * ROM or a ten-minute build to sit through. */
    char cmd[MAX_PATH + 64];
    const char *testCmd = getenv("GETV_WIZARD_TEST_CMD");
    if (testCmd != NULL && *testCmd) {
        snprintf(cmd, sizeof cmd, "\"%s\" -c \"%s\"", bash, testCmd);
    } else {
        snprintf(cmd, sizeof cmd, "\"%s\" \"tools/setup-windows.sh\"", bash);
    }

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                              NULL, repoRoot, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(p->hReadPipe);
        *err = "Could not start tools/setup-windows.sh.";
        return false;
    }
    p->hProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    p->started = true;
    HANDLE hReader = CreateThread(NULL, 0, reader_thread, p, 0, NULL);
    if (hReader != NULL) CloseHandle(hReader);
    return true;
}

void launch_game_and_exit(const char *repoRoot)
{
    char exeDir[MAX_PATH], exePath[MAX_PATH];
    snprintf(exeDir, sizeof exeDir, "%s\\getv\\build-windows", repoRoot);
    snprintf(exePath, sizeof exePath, "%s\\goldeneye.exe", exeDir);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" --launcher", exePath);

    if (CreateProcessA(exePath, cmd, NULL, NULL, FALSE, 0, NULL, exeDir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ---------------------------------------------------------------- look
 *
 * Deliberately not ge_launcher.cpp's style module -- this window is up for minutes at most, on
 * a machine that has usually never seen this project before, and reads better as plain
 * instructions than as a game menu. Default ImGui dark style, left as-is. */
void ui_step_label(const char *text)
{
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", text);
}

void ui_error(const char *text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", text);
    ImGui::PopTextWrapPos();
}

void ui_ok(const char *text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", text);
    ImGui::PopTextWrapPos();
}

} // namespace

enum WizState { WELCOME, PICK_ROM, CONFIRM, RUNNING, DONE, FAILED };

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* GETV_WIZARD_VERIFY_PATH=<file>: run verify_rom() against a real file and print the
     * result, no window. Exists to test the ROM-verification branches (wrong size, byte-
     * swapped, wrong hash, correct) against small synthetic files without needing a real
     * licensed ROM dump to exercise every path. */
    {
        const char *verifyPath = getenv("GETV_WIZARD_VERIFY_PATH");
        if (verifyPath != NULL && *verifyPath) {
            RomCheck c = verify_rom(verifyPath);
            printf("ok=%d message=%s\n", c.ok ? 1 : 0, c.message);
            return c.ok ? 0 : 1;
        }
    }

    char repoRoot[MAX_PATH];
    if (!find_repo_root(repoRoot, sizeof repoRoot)) {
        MessageBoxA(NULL,
            "Could not locate the goldeneye-native repository from this executable's location.\n\n"
            "This wizard expects to sit somewhere inside the repository it is setting up.",
            "GoldenEye Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        MessageBoxA(NULL, SDL_GetError(), "GoldenEye Setup: SDL_Init failed", MB_OK | MB_ICONERROR);
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int winw = 760, winh = 560;
    {
        SDL_Rect ub;
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0) {
            if (winw > ub.w - 60) winw = ub.w - 60;
            if (winh > ub.h - 60) winh = ub.h - 60;
        }
    }
    SDL_Window *win = SDL_CreateWindow("GoldenEye 007 -- Setup",
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        winw, winh,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (win == NULL) {
        MessageBoxA(NULL, SDL_GetError(), "GoldenEye Setup: SDL_CreateWindow failed", MB_OK | MB_ICONERROR);
        SDL_Quit();
        return 1;
    }
    gePortSetWindowIcon(win);

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx == NULL) {
        MessageBoxA(NULL, SDL_GetError(), "GoldenEye Setup: SDL_GL_CreateContext failed", MB_OK | MB_ICONERROR);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    glewInit();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL2_Init();

    WizState state = WELCOME;
    char romPath[MAX_PATH] = "";
    RomCheck romCheck;
    romCheck.ok = false;
    romCheck.message[0] = '\0';
    Pipeline pipeline;
    /* Initialized unconditionally, here, rather than inside start_pipeline(): every state past
     * WELCOME/PICK_ROM/CONFIRM reads or writes this lock, and the GETV_WIZARD_STATE debug jump
     * below can land on FAILED/DONE without ever calling start_pipeline() at all. An
     * EnterCriticalSection on an uninitialized CRITICAL_SECTION is a silent access violation,
     * not a clean failure -- worth not having two initialization sites to keep in sync. */
    InitializeCriticalSection(&pipeline.lock);
    std::string startErr;
    std::string copyErr;

    /* GETV_WIZARD_STATE=<0..5>: jump straight to a page, matching ge_launcher.cpp's
     * GETV_LAUNCHER_PAGE. Each page the probe cannot reach on its own -- CONFIRM/RUNNING/
     * DONE/FAILED all sit behind a real ROM and a real multi-minute pipeline run -- gets a
     * placeholder so it renders something meaningful instead of blank fields. */
    {
        const char *dbg = getenv("GETV_WIZARD_STATE");
        int want = (dbg != NULL && *dbg) ? atoi(dbg) : -1;
        if (want >= WELCOME && want <= FAILED) {
            state = (WizState) want;
            if (state == PICK_ROM || state == CONFIRM) {
                snprintf(romPath, sizeof romPath, "C:\\roms\\ge007.u.z64");
            }
            if (state == CONFIRM) {
                romCheck.ok = true;
                snprintf(romCheck.message, sizeof romCheck.message,
                          "Verified: matches the official US GoldenEye 007 ROM.");
            }
            if (state == RUNNING && env_true("GETV_WIZARD_AUTOSTART")) {
                /* Real process, real pipe, real reader thread -- against GETV_WIZARD_TEST_CMD
                 * rather than the real pipeline, so this exercises the actual plumbing without
                 * a ROM or a ten-minute build. */
                if (!start_pipeline(&pipeline, repoRoot, &startErr)) {
                    pipeline.lines.push_back(startErr);
                    pipeline.finished = true;
                    pipeline.exitCode = 1;
                }
            } else if (state == RUNNING) {
                pipeline.currentStep = "asset generation (docs/SETUP.md 3.5)";
                pipeline.lines.push_back("== decompiled game source ==");
                pipeline.lines.push_back("already cloned at vendor/ge-decomp");
                pipeline.lines.push_back("== ROM ==");
                pipeline.lines.push_back("ROM ok, copied into vendor/ge-decomp/baserom.u.z64");
                pipeline.lines.push_back("== asset generation (docs/SETUP.md 3.5) ==");
                pipeline.lines.push_back("Extracting compressed obseg/chr/00, 4096 bytes...");
            }
            if (state == DONE) pipeline.exitCode = 0;
            if (state == FAILED) {
                pipeline.exitCode = 1;
                pipeline.lines.push_back("setup-windows: 0002-assets.patch failed to apply");
            }
        }
    }

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win)) running = false;
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("wizard", NULL,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        switch (state) {
        case WELCOME: {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "This sets up GoldenEye 007 on this computer: it downloads the build tools, "
                "compiles the game from the public decompiled source, and links in your own "
                "ROM's assets. Nothing here is redistributed -- everything stays on this machine.");
            ImGui::Spacing();
            ImGui::TextUnformatted(
                "You will need your own GoldenEye 007 (U) cartridge, dumped as a .z64 file.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Continue", ImVec2(120, 32))) state = PICK_ROM;
            break;
        }
        case PICK_ROM: {
            ImGui::TextUnformatted("Select your GoldenEye 007 (U) ROM file.");
            ImGui::Spacing();
            ImGui::PushItemWidth(-90.0f);
            ImGui::InputText("##rompath", romPath, sizeof romPath);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(80, 0))) {
                char picked[MAX_PATH];
                if (open_rom_dialog(picked, sizeof picked)) snprintf(romPath, sizeof romPath, "%s", picked);
            }
            ImGui::Spacing();
            if (romCheck.message[0] != '\0') {
                if (romCheck.ok) ui_ok(romCheck.message); else ui_error(romCheck.message);
                ImGui::Spacing();
            }
            bool canVerify = romPath[0] != '\0';
            if (!canVerify) ImGui::BeginDisabled();
            if (ImGui::Button("Verify", ImVec2(100, 32))) {
                romCheck = verify_rom(romPath);
                if (romCheck.ok) state = CONFIRM;
            }
            if (!canVerify) ImGui::EndDisabled();
            break;
        }
        case CONFIRM: {
            ui_ok(romCheck.message);
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "Next: this downloads roughly 300-400 MB of build tools and libraries (first "
                "run only), then extracts and compiles the game. This can take several minutes "
                "and needs an internet connection.");
            if (copyErr.empty() && !startErr.empty()) {
                ImGui::Spacing();
                ui_error(startErr.c_str());
            }
            if (!copyErr.empty()) {
                ImGui::Spacing();
                ui_error(copyErr.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Back", ImVec2(100, 32))) state = PICK_ROM;
            ImGui::SameLine();
            if (ImGui::Button("Start setup", ImVec2(140, 32))) {
                char romsDir[MAX_PATH], dest[MAX_PATH];
                snprintf(romsDir, sizeof romsDir, "%s\\roms", repoRoot);
                CreateDirectoryA(romsDir, NULL);
                snprintf(dest, sizeof dest, "%s\\roms\\ge007.u.z64", repoRoot);
                copyErr.clear();
                if (!CopyFileA(romPath, dest, FALSE)) {
                    copyErr = "Could not copy the ROM into the repository's roms/ folder.";
                } else if (start_pipeline(&pipeline, repoRoot, &startErr)) {
                    state = RUNNING;
                } /* else startErr is set; stay on this page and show it */
            }
            break;
        }
        case RUNNING: {
            EnterCriticalSection(&pipeline.lock);
            std::string step = pipeline.currentStep;
            bool finished = pipeline.finished;
            DWORD code = pipeline.exitCode;

            ui_step_label(step.empty() ? "Starting..." : step.c_str());
            ImGui::Spacing();
            ImGui::BeginChild("log", ImVec2(0, -8.0f), true);
            ImGuiListClipper clipper;
            clipper.Begin((int) pipeline.lines.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    ImGui::TextUnformatted(pipeline.lines[(size_t) i].c_str());
                }
            }
            clipper.End();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            LeaveCriticalSection(&pipeline.lock);

            if (finished) state = (code == 0) ? DONE : FAILED;
            break;
        }
        case DONE: {
            ui_ok("Setup complete.");
            ImGui::Spacing();
            if (ImGui::Button("Launch GoldenEye", ImVec2(180, 36))) {
                launch_game_and_exit(repoRoot);
                running = false;
            }
            break;
        }
        case FAILED: {
            char hdr[128];
            snprintf(hdr, sizeof hdr, "Setup failed (exit code %lu).", (unsigned long) pipeline.exitCode);
            ui_error(hdr);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "Scroll the log below for details, or run tools\\setup-windows.sh yourself from "
                "a git-bash prompt to see the full output.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            EnterCriticalSection(&pipeline.lock);
            ImGui::BeginChild("log2", ImVec2(0, -46.0f), true);
            ImGuiListClipper clipper;
            clipper.Begin((int) pipeline.lines.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    ImGui::TextUnformatted(pipeline.lines[(size_t) i].c_str());
                }
            }
            clipper.End();
            ImGui::EndChild();
            LeaveCriticalSection(&pipeline.lock);
            if (ImGui::Button("Quit", ImVec2(100, 32))) running = false;
            break;
        }
        }

        ImGui::End();
        ImGui::Render();

        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.031f, 0.035f, 0.043f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        /* GETV_WIZARD_SHOT=<path.bmp> with GETV_WIZARD_PROBE=<frames>: same deterministic
         * glReadPixels capture ge_launcher.cpp uses, for the same reason -- a window-manager
         * screenshot tool cannot photograph a HighDPI window reliably, and this has no such
         * ambiguity because it reads the buffer that was just drawn. */
        {
            static int probe_seen = 0;
            const char *probeEnv = getenv("GETV_WIZARD_PROBE");
            int probeFrames = (probeEnv != NULL && *probeEnv) ? atoi(probeEnv) : 0;
            if (probeFrames > 0) {
                probe_seen++;
                if (probe_seen >= probeFrames) {
                    const char *shot = getenv("GETV_WIZARD_SHOT");
                    if (shot != NULL && *shot != '\0') {
                        unsigned char *px = (unsigned char *) malloc((size_t) dw * (size_t) dh * 4);
                        FILE *f = (px != NULL) ? fopen(shot, "wb") : NULL;
                        if (px != NULL && f != NULL) {
                            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                            const int pad = (4 - (dw * 3) % 4) % 4;
                            const int dat = (dw * 3 + pad) * dh;
                            unsigned char hdr[54];
                            const unsigned char zero[3] = { 0, 0, 0 };
                            memset(hdr, 0, sizeof hdr);
                            hdr[0] = 'B'; hdr[1] = 'M';
                            *(int *) &hdr[2]  = 54 + dat;
                            *(int *) &hdr[10] = 54;
                            *(int *) &hdr[14] = 40;
                            *(int *) &hdr[18] = dw;
                            *(int *) &hdr[22] = dh;
                            *(short *) &hdr[26] = 1;
                            *(short *) &hdr[28] = 24;
                            *(int *) &hdr[34] = dat;
                            fwrite(hdr, 1, 54, f);
                            for (int y = 0; y < dh; y++) {
                                for (int x = 0; x < dw; x++) {
                                    const unsigned char *q = px + ((size_t) y * dw + x) * 4;
                                    const unsigned char bgr[3] = { q[2], q[1], q[0] };
                                    fwrite(bgr, 1, 3, f);
                                }
                                if (pad) fwrite(zero, 1, (size_t) pad, f);
                            }
                            printf("[getv][wizard] shot: %s (%dx%d)\n", shot, dw, dh);
                        }
                        if (f) fclose(f);
                        if (px) free(px);
                    }
                    running = false;
                }
            }
        }

        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
