/* The setup wizard.
 *
 * What this is. A small, standalone window that runs before anything else exists: it has no
 * dependency on the decomp, the ROM, or the built game, because its whole job is to get from
 * "download one setup file" to "goldeneye.exe exists" for someone who does not want to install
 * developer tools or open a terminal. It downloads private, portable Git and Python copies,
 * picks the user's ROM, recognizes z64/v64/n64 byte order, verifies and imports a normalized
 * local copy, then runs tools/setup-windows.sh and shows its output live.
 *
 * ---------------------------------------------------------------- why this is a second binary
 *
 * ge_launcher.cpp cannot do this job itself, however tempting it looks from the outside -- it
 * is compiled into goldeneye.exe, and goldeneye.exe does not exist until after a ROM-dependent
 * build has already happened once (docs/LICENSING.md section 5: assets are compiled in, not
 * loaded at runtime). Something has to exist before that first build to drive it. This is that
 * something, and once it succeeds it hands off to the real launcher and gets out of the way.
 *
 * Contains zero ROM-derived or decomp-derived code, on purpose. That is the package's technical
 * boundary; docs/LICENSING.md records the separate licensing questions that still need review
 * before a public release. It statically links its own SDL2 + Dear ImGui, built once by a
 * developer from this repo's own already-fetched toolchain, the same way ge_launcher.cpp does --
 * see that file for the init sequence this one mirrors.
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
#include <shellapi.h>
#include <shlobj.h>

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

#ifndef GETV_WIZARD_REPO_URL
# define GETV_WIZARD_REPO_URL "https://github.com/seb-patron/goldeneye-native.git"
#endif
#ifndef GETV_WIZARD_REPO_REF
# define GETV_WIZARD_REPO_REF "main"
#endif

namespace {

const char *kWantSha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83";
const long  kWantSize = 12582912;

/* These versions and digests are deliberately pinned. The setup executable may download tools,
 * but it never executes a mutable "latest" URL without first matching a digest published by the
 * upstream release. Updating either package is a reviewed source change, not a server-side event
 * that silently changes what an old setup executable runs. */
const char *kPortableGitDir = "git-2.55.0.3";
const char *kPortablePythonDir = "python-3.14.7";
char gBootstrapRoot[MAX_PATH] = "";

enum RomByteOrder {
    ROM_ORDER_UNKNOWN,
    ROM_ORDER_Z64,
    ROM_ORDER_V64,
    ROM_ORDER_N64,
};

const char *rom_order_name(RomByteOrder order)
{
    switch (order) {
    case ROM_ORDER_Z64: return "z64 (big-endian)";
    case ROM_ORDER_V64: return "v64 (byte-swapped)";
    case ROM_ORDER_N64: return "n64 (word-swapped)";
    default:            return "unknown";
    }
}

RomByteOrder detect_rom_order(const unsigned char header[4])
{
    if (header[0] == 0x80 && header[1] == 0x37 &&
        header[2] == 0x12 && header[3] == 0x40) return ROM_ORDER_Z64;
    if (header[0] == 0x37 && header[1] == 0x80 &&
        header[2] == 0x40 && header[3] == 0x12) return ROM_ORDER_V64;
    if (header[0] == 0x40 && header[1] == 0x12 &&
        header[2] == 0x37 && header[3] == 0x80) return ROM_ORDER_N64;
    return ROM_ORDER_UNKNOWN;
}

/* The extractor consumes big-endian z64 bytes. v64 swaps every adjacent pair and n64 reverses
 * each four-byte word; both operations are their own inverse. Buffers passed here are always a
 * multiple of four bytes except possibly the final buffer of a malformed file, which verify_rom
 * rejects on size before importing it. */
void normalize_rom_bytes(unsigned char *bytes, size_t n, RomByteOrder order)
{
    if (order == ROM_ORDER_V64) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            unsigned char t = bytes[i];
            bytes[i] = bytes[i + 1];
            bytes[i + 1] = t;
        }
    } else if (order == ROM_ORDER_N64) {
        for (size_t i = 0; i + 3 < n; i += 4) {
            unsigned char t = bytes[i];
            bytes[i] = bytes[i + 3];
            bytes[i + 3] = t;
            t = bytes[i + 1];
            bytes[i + 1] = bytes[i + 2];
            bytes[i + 2] = t;
        }
    }
}

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

bool get_bootstrap_root(char *out, size_t n)
{
    char localAppData[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL,
                         SHGFP_TYPE_CURRENT, localAppData) != S_OK) {
        const char *fallback = getenv("TEMP");
        if (fallback == NULL || *fallback == '\0') return false;
        snprintf(localAppData, sizeof localAppData, "%s", fallback);
    }
    int len = snprintf(out, n, "%s\\GoldenEyeNative\\bootstrap", localAppData);
    return len >= 0 && (size_t) len < n;
}

bool portable_tool_paths(char *git, size_t gitN, char *bash, size_t bashN,
                         char *python, size_t pythonN)
{
    if (gBootstrapRoot[0] == '\0') return false;
    int a = snprintf(git, gitN, "%s\\%s\\cmd\\git.exe", gBootstrapRoot, kPortableGitDir);
    int b = snprintf(bash, bashN, "%s\\%s\\bin\\bash.exe", gBootstrapRoot, kPortableGitDir);
    int c = snprintf(python, pythonN, "%s\\%s\\python.exe",
                     gBootstrapRoot, kPortablePythonDir);
    return a >= 0 && (size_t) a < gitN && b >= 0 && (size_t) b < bashN &&
           c >= 0 && (size_t) c < pythonN;
}

bool portable_tools_ready()
{
    char git[MAX_PATH], bash[MAX_PATH], python[MAX_PATH];
    return portable_tool_paths(git, sizeof git, bash, sizeof bash, python, sizeof python) &&
           file_exists(git) && file_exists(bash) && file_exists(python);
}

bool activate_portable_tools(std::string *err)
{
    char git[MAX_PATH], bash[MAX_PATH], python[MAX_PATH];
    if (!portable_tool_paths(git, sizeof git, bash, sizeof bash, python, sizeof python) ||
        !file_exists(git) || !file_exists(bash) || !file_exists(python)) {
        *err = "The private Git/Python download finished, but the expected programs are missing.";
        return false;
    }

    char gitCmd[MAX_PATH], gitBin[MAX_PATH], pythonDir[MAX_PATH], mingw[MAX_PATH];
    snprintf(gitCmd, sizeof gitCmd, "%s\\%s\\cmd", gBootstrapRoot, kPortableGitDir);
    snprintf(gitBin, sizeof gitBin, "%s\\%s\\bin", gBootstrapRoot, kPortableGitDir);
    snprintf(pythonDir, sizeof pythonDir, "%s\\%s", gBootstrapRoot, kPortablePythonDir);
    snprintf(mingw, sizeof mingw, "%s\\build-tools\\mingw64", gBootstrapRoot);

    DWORD oldN = GetEnvironmentVariableA("PATH", NULL, 0);
    std::vector<char> oldPath(oldN > 0 ? oldN : 1, '\0');
    if (oldN > 0) GetEnvironmentVariableA("PATH", oldPath.data(), oldN);
    std::string path = std::string(pythonDir) + ";" + gitCmd + ";" + gitBin;
    if (oldN > 1) path += std::string(";") + oldPath.data();

    if (!SetEnvironmentVariableA("PATH", path.c_str()) ||
        !SetEnvironmentVariableA("GETV_PORTABLE_PYTHON", python) ||
        !SetEnvironmentVariableA("GETV_MINGW", mingw)) {
        *err = "Windows would not activate the private build tools for this setup run.";
        return false;
    }
    return true;
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

/* git.exe has no same-named WSL stub to collide with (unlike bash.exe -- see below), so a
 * plain PATH search is safe for it specifically. Shared by find_bash_exe (derives bash as
 * git's sibling) and the repo-clone step (runs git.exe directly, no shell needed at all). */
bool find_git_exe(char *out, size_t n)
{
    char portableGit[MAX_PATH], portableBash[MAX_PATH], portablePython[MAX_PATH];
    if (portable_tool_paths(portableGit, sizeof portableGit, portableBash, sizeof portableBash,
                            portablePython, sizeof portablePython) && file_exists(portableGit)) {
        snprintf(out, n, "%s", portableGit);
        return true;
    }
    static const char *candidates[] = {
        "C:\\Program Files\\Git\\cmd\\git.exe",
        "C:\\Program Files (x86)\\Git\\cmd\\git.exe",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (file_exists(candidates[i])) { snprintf(out, n, "%s", candidates[i]); return true; }
    }
    char buf[MAX_PATH];
    DWORD r = SearchPathA(NULL, "git.exe", NULL, (DWORD) sizeof buf, buf, NULL);
    if (r > 0 && r < sizeof buf) { snprintf(out, n, "%s", buf); return true; }
    return false;
}

/* Deliberately does NOT SearchPathA for "bash.exe" -- confirmed by hand that on any machine
 * with WSL present (which is most Windows 10/11 machines, whether or not a distro is actually
 * installed), C:\Windows\System32\bash.exe is a real file with that exact name, and System32
 * is searched ahead of Git's own directory. It runs, prints "Windows Subsystem for Linux has
 * no installed distributions" as UTF-16 to stderr, and exits 1 -- which this wizard's line
 * reader, expecting UTF-8/ASCII, turns into a single garbled character with no indication of
 * what actually happened. Known Git for Windows layouts first, then git.exe -- via
 * find_git_exe, which is safe to search for by name -- and bash.exe derived as its sibling. */
bool find_bash_exe(char *out, size_t n)
{
    char portableGit[MAX_PATH], portableBash[MAX_PATH], portablePython[MAX_PATH];
    if (portable_tool_paths(portableGit, sizeof portableGit, portableBash, sizeof portableBash,
                            portablePython, sizeof portablePython) && file_exists(portableBash)) {
        snprintf(out, n, "%s", portableBash);
        return true;
    }
    static const char *candidates[] = {
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (file_exists(candidates[i])) { snprintf(out, n, "%s", candidates[i]); return true; }
    }

    char gitPath[MAX_PATH];
    if (find_git_exe(gitPath, sizeof gitPath)) {
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

/* ---------------------------------------------------------------- ROM verification + import
 *
 * Mirrors docs/SETUP.md 3.3/3.4 while accepting all three common cartridge-dump byte orders.
 * The expected digest is always computed over normalized big-endian z64 bytes, so a correct v64
 * or n64 dump gets the same verdict without asking the user to find a separate conversion tool. */
struct RomCheck {
    bool ok;
    RomByteOrder order;
    char message[512];
};

RomCheck verify_rom(const char *path)
{
    RomCheck r;
    r.ok = false;
    r.order = ROM_ORDER_UNKNOWN;
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
    r.order = detect_rom_order(header);

    if (size != kWantSize) {
        if (r.order != ROM_ORDER_UNKNOWN) {
            snprintf(r.message, sizeof r.message,
                "Recognized %s byte order, but the file is %ld bytes; expected %ld. It may be "
                "trimmed, padded, or carry a dumper header.", rom_order_name(r.order), size,
                kWantSize);
        } else {
            snprintf(r.message, sizeof r.message,
                "Wrong size: %ld bytes, expected %ld. This may be trimmed, padded, or carry a "
                "dumper header rather than being a plain cartridge dump.", size, kWantSize);
        }
        fclose(f);
        return r;
    }

    if (r.order == ROM_ORDER_UNKNOWN) {
        snprintf(r.message, sizeof r.message,
            "Right size, but the header is not a recognized z64, v64, or n64 cartridge dump.");
        fclose(f);
        return r;
    }

    sha1_ctx ctx;
    sha1_init(&ctx);
    fseek(f, 0, SEEK_SET);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        normalize_rom_bytes(buf, n, r.order);
        sha1_update(&ctx, buf, n);
    }
    if (ferror(f)) {
        fclose(f);
        snprintf(r.message, sizeof r.message, "The ROM could not be read completely.");
        return r;
    }
    fclose(f);

    unsigned char digest[20];
    char hex[41];
    sha1_final(&ctx, digest);
    sha1_hex(digest, hex);

    if (strcmp(hex, kWantSha1) != 0) {
        snprintf(r.message, sizeof r.message,
            "Recognized %s byte order, but the normalized checksum doesn't match (got %s). "
            "This may be a different region or a modified ROM -- only the US release is "
            "supported.", rom_order_name(r.order), hex);
        return r;
    }

    r.ok = true;
    if (r.order == ROM_ORDER_Z64) {
        snprintf(r.message, sizeof r.message,
                 "Verified: US GoldenEye 007 ROM in z64 byte order. It will be copied locally.");
    } else {
        snprintf(r.message, sizeof r.message,
                 "Verified: US GoldenEye 007 ROM in %s byte order. It will be converted to z64 "
                 "locally while it is imported.", rom_order_name(r.order));
    }
    return r;
}

/* Write through a sibling temporary file and replace the destination only after all bytes have
 * been normalized successfully. A failed or interrupted import therefore never leaves a partial
 * file at the exact path setup-windows.sh trusts. */
bool import_rom(const char *src, const char *dst, RomByteOrder order, std::string *err)
{
    if (order == ROM_ORDER_UNKNOWN) {
        *err = "The selected ROM's byte order was not verified; select and verify it again.";
        return false;
    }
    char tmp[MAX_PATH];
    int tmpLen = snprintf(tmp, sizeof tmp, "%s.importing", dst);
    if (tmpLen < 0 || (size_t) tmpLen >= sizeof tmp) {
        *err = "The installation path is too long for a safe ROM import. Choose a shorter folder.";
        return false;
    }
    DeleteFileA(tmp);

    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        *err = "Could not reopen the selected ROM for importing.";
        return false;
    }
    FILE *out = fopen(tmp, "wb");
    if (out == NULL) {
        fclose(in);
        *err = "Could not create the local ROM file in the installation folder.";
        return false;
    }

    bool ok = true;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        normalize_rom_bytes(buf, n, order);
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    if (fflush(out) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(in);

    if (!ok) {
        DeleteFileA(tmp);
        *err = "The ROM could not be imported completely. Check free disk space and try again.";
        return false;
    }

    RomCheck written = verify_rom(tmp);
    if (!written.ok || written.order != ROM_ORDER_Z64) {
        DeleteFileA(tmp);
        *err = "The locally imported copy did not pass verification; the original was not changed.";
        return false;
    }
    if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        *err = "The verified ROM could not be moved into the installation folder.";
        return false;
    }
    return true;
}

int rom_import_self_test()
{
    const unsigned char z64[] = {
        0x80, 0x37, 0x12, 0x40, 0xde, 0xad, 0xbe, 0xef,
        0x01, 0x23, 0x45, 0x67,
    };
    unsigned char v64[sizeof z64];
    unsigned char n64[sizeof z64];
    memcpy(v64, z64, sizeof z64);
    memcpy(n64, z64, sizeof z64);
    normalize_rom_bytes(v64, sizeof v64, ROM_ORDER_V64);
    normalize_rom_bytes(n64, sizeof n64, ROM_ORDER_N64);

    if (detect_rom_order(z64) != ROM_ORDER_Z64 ||
        detect_rom_order(v64) != ROM_ORDER_V64 ||
        detect_rom_order(n64) != ROM_ORDER_N64) {
        fprintf(stderr, "ROM import self-test failed: byte-order detection\n");
        return 1;
    }
    normalize_rom_bytes(v64, sizeof v64, ROM_ORDER_V64);
    normalize_rom_bytes(n64, sizeof n64, ROM_ORDER_N64);
    if (memcmp(v64, z64, sizeof z64) != 0 || memcmp(n64, z64, sizeof z64) != 0) {
        fprintf(stderr, "ROM import self-test failed: normalization\n");
        return 1;
    }
    printf("ROM import self-test passed: z64, v64, and n64\n");
    return 0;
}

bool open_rom_dialog(char *out, size_t n)
{
    char buf[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "N64 ROM (*.z64;*.v64;*.n64)\0*.z64;*.v64;*.n64\0All files\0*.*\0\0";
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

/* SHBrowseForFolderA rather than IFileOpenDialog: no COM initialization needed, matching
 * GetOpenFileNameA's simplicity above for a dialog that only has to pick a directory. */
bool open_folder_dialog(char *out, size_t n)
{
    char display[MAX_PATH] = "";
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof bi);
    bi.lpszTitle = "Choose where to install GoldenEye 007";
    bi.pszDisplayName = display;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl == NULL) return false;

    char path[MAX_PATH];
    bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;

    snprintf(out, n, "%s", path);
    return true;
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

/* Shared by start_pipeline and start_clone: both need "run this command line, stream combined
 * stdout+stderr into p->lines off the UI thread, notice when it exits" and differ only in what
 * they run and where. cmd is a fully-built, already-quoted command line; cwd may be NULL to
 * inherit this process's own working directory (used for the clone step, which runs before any
 * repoRoot exists to cd into). p->lock is initialized once in main(), before either caller ever
 * runs -- see the comment there. */
bool start_process(Pipeline *p, const char *cmd, const char *cwd, std::string *err)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hWrite = NULL;
    if (!CreatePipe(&p->hReadPipe, &hWrite, &sa, 0)) {
        *err = "Could not create a pipe for the command's output.";
        return false;
    }
    SetHandleInformation(p->hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* NUL, not NULL. STARTF_USESTDHANDLES makes all three handles significant, so a NULL stdin
     * hands the child a handle that is invalid rather than merely empty, and anything that asks
     * the OS for it fails outright:
     *
     *   python  OSError: [WinError 6] The handle is invalid, from GetStdHandle(STD_INPUT_HANDLE)
     *           inside subprocess.run -- so gen_asset_fileview.py, gen_propdef_layout.py and
     *           uniquify_asset_symbols.py all die the moment they try to run a compiler
     *   msys    sha1sum: failed to set file descriptor text/binary mode: Bad file descriptor,
     *           printing nothing, which made the ROM check compare an empty hash
     *
     * Reported three separate ways -- issues #6, #7 and #8 -- all of them this one line. Nothing
     * the wizard runs wants to read stdin, so an empty device is the right answer rather than a
     * pipe nobody writes to. */
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
    if (hNul == INVALID_HANDLE_VALUE) {
        CloseHandle(p->hReadPipe);
        CloseHandle(hWrite);
        *err = "Could not open the NUL device for the command's input.";
        return false;
    }

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = hNul;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    /* CreateProcessA needs a writable buffer for the command line, not a const char*. */
    std::vector<char> cmdBuf(cmd, cmd + strlen(cmd) + 1);

    BOOL ok = CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
                              NULL, cwd, &si, &pi);
    CloseHandle(hWrite);
    CloseHandle(hNul);
    if (!ok) {
        CloseHandle(p->hReadPipe);
        *err = "Could not start the command.";
        return false;
    }
    p->hProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    p->started = true;
    HANDLE hReader = CreateThread(NULL, 0, reader_thread, p, 0, NULL);
    if (hReader != NULL) CloseHandle(hReader);
    return true;
}

void reset_pipeline(Pipeline *p)
{
    if (p->hProcess != NULL) CloseHandle(p->hProcess);
    if (p->hReadPipe != NULL) CloseHandle(p->hReadPipe);
    p->hProcess = NULL;
    p->hReadPipe = NULL;
    p->lines.clear();
    p->partial.clear();
    p->currentStep.clear();
    p->started = false;
    p->finished = false;
    p->exitCode = 1;
}

/* Windows 10/11 includes Windows PowerShell, so the bootstrapper can remain one executable
 * without shipping another script beside it. The script written here downloads only versioned
 * upstream archives, verifies their published SHA-256 values before execution/extraction, and
 * installs them beneath the current user's LocalAppData. It never changes the registry, PATH,
 * or an all-users installation; activate_portable_tools changes PATH only for this process and
 * the children it creates. */
bool write_portable_tools_script(const char *path, std::string *err)
{
    static const char script[] = R"PS(param(
  [Parameter(Mandatory=$true)][string]$Root
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$gitUrl = 'https://github.com/git-for-windows/git/releases/download/v2.55.0.windows.3/PortableGit-2.55.0.3-64-bit.7z.exe'
$gitSha256 = 'ab00566336b5472120f9a52d34f2e79c5406535792acb0548001ffd0bd090e5d'
$pythonUrl = 'https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip'
$pythonSha256 = 'd297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15'
$gitDir = Join-Path $Root 'git-2.55.0.3'
$pythonDir = Join-Path $Root 'python-3.14.7'
$downloads = Join-Path $Root 'downloads'

New-Item -ItemType Directory -Force -Path $Root, $downloads | Out-Null

function Get-VerifiedFile([string]$Url, [string]$Sha256, [string]$Path) {
  if (Test-Path -LiteralPath $Path) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -eq $Sha256) {
      Write-Output "verified cached $([IO.Path]::GetFileName($Path))"
      return
    }
    Remove-Item -LiteralPath $Path -Force
  }
  $partial = "$Path.downloading"
  Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
  Write-Output "downloading $Url"
  Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
  $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $Sha256) {
    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    throw "SHA-256 mismatch for $Url (got $actual)"
  }
  Move-Item -LiteralPath $partial -Destination $Path
  Write-Output "verified SHA-256: $actual"
}

if (-not (Test-Path -LiteralPath (Join-Path $gitDir 'cmd\git.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $gitDir 'bin\bash.exe'))) {
  Write-Output '== private portable Git for Windows =='
  $archive = Join-Path $downloads 'PortableGit-2.55.0.3-64-bit.7z.exe'
  Get-VerifiedFile $gitUrl $gitSha256 $archive
  $staging = "$gitDir.installing"
  Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $staging | Out-Null
  # Invoke the official self-extracting archive itself (not 7-Zip against its contents), so its
  # configured post-install step runs exactly once after extraction.
  & $archive -y "-o$staging"
  if ($LASTEXITCODE -ne 0) { throw "PortableGit extraction failed (exit $LASTEXITCODE)" }
  if (-not (Test-Path -LiteralPath (Join-Path $staging 'cmd\git.exe')) -or
      -not (Test-Path -LiteralPath (Join-Path $staging 'bin\bash.exe'))) {
    throw 'PortableGit archive did not contain git.exe and bash.exe'
  }
  Remove-Item -LiteralPath $gitDir -Recurse -Force -ErrorAction SilentlyContinue
  Move-Item -LiteralPath $staging -Destination $gitDir
} else {
  Write-Output 'private portable Git for Windows: already ready'
}

if (-not (Test-Path -LiteralPath (Join-Path $pythonDir 'python.exe'))) {
  Write-Output '== private embeddable Python =='
  $archive = Join-Path $downloads 'python-3.14.7-embed-amd64.zip'
  Get-VerifiedFile $pythonUrl $pythonSha256 $archive
  $staging = "$pythonDir.installing"
  Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $staging | Out-Null
  Expand-Archive -LiteralPath $archive -DestinationPath $staging -Force
  if (-not (Test-Path -LiteralPath (Join-Path $staging 'python.exe'))) {
    throw 'Python archive did not contain python.exe'
  }
  Remove-Item -LiteralPath $pythonDir -Recurse -Force -ErrorAction SilentlyContinue
  Move-Item -LiteralPath $staging -Destination $pythonDir
} else {
  Write-Output 'private embeddable Python: already ready'
}

Write-Output 'portable build prerequisites are ready'
)PS";

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        *err = "Could not create the private tool download script.";
        return false;
    }
    const size_t want = sizeof script - 1;
    bool ok = fwrite(script, 1, want, f) == want;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        *err = "Could not write the private tool download script completely.";
        return false;
    }
    return true;
}

bool start_tool_bootstrap(Pipeline *p, std::string *err)
{
    int mkdirResult = SHCreateDirectoryExA(NULL, gBootstrapRoot, NULL);
    if (mkdirResult != ERROR_SUCCESS && mkdirResult != ERROR_ALREADY_EXISTS &&
        mkdirResult != ERROR_FILE_EXISTS) {
        *err = "Could not create the private build-tools folder in LocalAppData.";
        return false;
    }

    char scriptPath[MAX_PATH];
    int scriptLen = snprintf(scriptPath, sizeof scriptPath, "%s\\install-portable-tools.ps1",
                             gBootstrapRoot);
    if (scriptLen < 0 || (size_t) scriptLen >= sizeof scriptPath ||
        !write_portable_tools_script(scriptPath, err)) return false;

    char windowsDir[MAX_PATH], powershell[MAX_PATH];
    UINT windowsLen = GetWindowsDirectoryA(windowsDir, (UINT) sizeof windowsDir);
    int powershellLen = (windowsLen > 0 && windowsLen < sizeof windowsDir)
        ? snprintf(powershell, sizeof powershell,
                   "%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", windowsDir)
        : -1;
    if (powershellLen < 0 || (size_t) powershellLen >= sizeof powershell ||
        !file_exists(powershell)) {
        *err = "Could not locate the Windows PowerShell included with Windows 10/11.";
        return false;
    }

    char cmd[MAX_PATH * 4];
    int cmdLen = snprintf(cmd, sizeof cmd,
        "\"%s\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\" -Root \"%s\"",
        powershell, scriptPath, gBootstrapRoot);
    if (cmdLen < 0 || (size_t) cmdLen >= sizeof cmd) {
        *err = "The private build-tools path is too long.";
        return false;
    }
    return start_process(p, cmd, NULL, err);
}

bool start_pipeline(Pipeline *p, const char *repoRoot, std::string *err)
{
    char bash[MAX_PATH];
    if (!find_bash_exe(bash, sizeof bash)) {
        *err = "The setup app's private bash.exe is missing. Run the setup app again so it can "
               "repair its tool download.";
        return false;
    }

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
    return start_process(p, cmd, repoRoot, err);
}

/* The package build embeds the source repository + branch/tag it belongs to. That matters for a
 * test artifact: cloning main from an exe built on a feature branch silently tests a different
 * setup pipeline. Environment overrides remain for local harnesses. dest is created if it does
 * not already exist; git itself refuses to clone into a non-empty one. */
bool start_clone(Pipeline *p, const char *dest, std::string *err)
{
    char git[MAX_PATH];
    if (!find_git_exe(git, sizeof git)) {
        *err = "The setup app's private git.exe is missing. Run the setup app again so it can "
               "repair its tool download.";
        return false;
    }
    const char *url = getenv("GETV_WIZARD_REPO_URL");
    if (url == NULL || *url == '\0') url = GETV_WIZARD_REPO_URL;
    const char *ref = getenv("GETV_WIZARD_REPO_REF");
    if (ref == NULL || *ref == '\0') ref = GETV_WIZARD_REPO_REF;

    char cmd[MAX_PATH * 3 + 128];
    int cmdLen = snprintf(cmd, sizeof cmd,
                          "\"%s\" clone --branch \"%s\" --single-branch \"%s\" \"%s\"",
                          git, ref, url, dest);
    if (cmdLen < 0 || (size_t) cmdLen >= sizeof cmd) {
        *err = "The download command is too long. Choose a shorter installation folder.";
        return false;
    }
    return start_process(p, cmd, NULL, err);
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

/* The eight banners tools/setup-windows.sh prints, in the order it prints them, so that a live
 * "== build ==" can be turned into "step 8 of 8" and a filled bar.
 *
 * This exists because the RUNNING page is on screen for 10 to 40 minutes and, without it, shows
 * only a scrolling log. Someone who does not read build output cannot tell a slow step from a
 * hung one, and the reasonable thing for them to do at that point is close the window -- part
 * way through asset generation, which is the one moment it costs them the whole run.
 *
 * The order is the contract. A step added to setup-windows.sh belongs here too; one that is not
 * in this list returns -1 and the caller keeps the position it already had, so an unrecognised
 * banner stalls the bar rather than making it jump backwards. */
const char *const kSetupSteps[] = {
    "checking for python3",
    "third-party port sources",
    "mingw toolchain, SDL2, GLEW, Lua, Dear ImGui, Tracy",
    "decompiled game source",
    "ROM",
    "asset generation (docs/SETUP.md 3.5)",
    "symbol namespacing (docs/SETUP.md 3.6)",
    "build",
};
const int kSetupStepCount = (int) (sizeof kSetupSteps / sizeof kSetupSteps[0]);

int setup_step_index(const std::string &step)
{
    for (int i = 0; i < kSetupStepCount; i++) {
        if (step == kSetupSteps[i]) return i;
    }
    return -1;
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

enum WizState {
    WELCOME,
    BOOTSTRAPPING,
    PICK_DEST,
    CLONING,
    PICK_ROM,
    CONFIRM,
    RUNNING,
    DONE,
    FAILED
};

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return rom_import_self_test();
    if (argc == 3 && strcmp(argv[1], "--write-bootstrap-script") == 0) {
        std::string err;
        if (!write_portable_tools_script(argv[2], &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        printf("wrote portable-tool bootstrap script: %s\n", argv[2]);
        return 0;
    }

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

    /* Not fatal if this fails: a wizard handed to someone with nothing set up yet is exactly
     * the case that has no repository to find. PICK_DEST (below) clones one; everything past
     * that point uses repoRoot the same way whether it was found here or just cloned. */
    char repoRoot[MAX_PATH] = "";
    bool haveRepo = find_repo_root(repoRoot, sizeof repoRoot);
    if (!get_bootstrap_root(gBootstrapRoot, sizeof gBootstrapRoot)) {
        MessageBoxA(NULL, "Could not locate a private LocalAppData folder for build tools.",
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
    romCheck.order = ROM_ORDER_UNKNOWN;
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
    std::string cloneErr;
    std::string failureLabel = "Setup";

    /* Computed unconditionally, not just when !haveRepo: GETV_WIZARD_STATE can force PICK_DEST
     * or CLONING for testing regardless of what find_repo_root actually found, and those pages
     * need a real default to show either way. GETV_WIZARD_DEST_PATH overrides it, the same
     * reason GETV_WIZARD_REPO_URL exists -- picking a fresh scratch destination per test run
     * without editing source. */
    char destPath[MAX_PATH];
    {
        const char *override_ = getenv("GETV_WIZARD_DEST_PATH");
        const char *docs = getenv("USERPROFILE");
        if (override_ != NULL && *override_) {
            snprintf(destPath, sizeof destPath, "%s", override_);
        } else {
            snprintf(destPath, sizeof destPath, "%s\\Documents\\goldeneye-native",
                     (docs != NULL && *docs) ? docs : "C:\\Users\\Public");
        }
    }

    /* GETV_WIZARD_STATE=<0..8>: jump straight to a page, matching ge_launcher.cpp's
     * GETV_LAUNCHER_PAGE. Each page the probe cannot reach on its own -- CLONING/CONFIRM/
     * RUNNING/DONE/FAILED all sit behind a real download, a real ROM, or a real multi-minute
     * pipeline run -- gets a placeholder so it renders something meaningful instead of blank
     * fields. Every page from PICK_ROM on assumes a repo exists, same as reaching it normally
     * would (either found at startup or just cloned), so haveRepo is forced on for those; only
     * PICK_DEST/CLONING represent the no-repo-yet state and leave it as found at startup. */
    {
        const char *dbg = getenv("GETV_WIZARD_STATE");
        int want = (dbg != NULL && *dbg) ? atoi(dbg) : -1;
        if (want >= WELCOME && want <= FAILED) {
            state = (WizState) want;
            if (state >= PICK_ROM) haveRepo = true;
            if (state == PICK_ROM || state == CONFIRM) {
                snprintf(romPath, sizeof romPath, "C:\\roms\\ge007.u.z64");
            }
            if (state == CONFIRM) {
                romCheck.ok = true;
                romCheck.order = ROM_ORDER_Z64;
                snprintf(romCheck.message, sizeof romCheck.message,
                          "Verified: US GoldenEye 007 ROM in z64 byte order.");
            }
            if (state == BOOTSTRAPPING && env_true("GETV_WIZARD_AUTOSTART")) {
                failureLabel = "Build-tool download";
                if (!start_tool_bootstrap(&pipeline, &startErr)) {
                    pipeline.lines.push_back(startErr);
                    pipeline.finished = true;
                    pipeline.exitCode = 1;
                }
            } else if (state == BOOTSTRAPPING) {
                pipeline.lines.push_back("downloading private portable Git for Windows...");
                pipeline.lines.push_back("verifying the published SHA-256 before extraction...");
            }
            if (state == CLONING && env_true("GETV_WIZARD_AUTOSTART")) {
                haveRepo = false;
                failureLabel = "Source download";
                start_clone(&pipeline, destPath, &cloneErr);
            } else if (state == CLONING) {
                haveRepo = false;
                pipeline.lines.push_back("Cloning into 'goldeneye-native'...");
                pipeline.lines.push_back("remote: Enumerating objects: 4213, done.");
                pipeline.lines.push_back("Receiving objects: 61% (2570/4213)");
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
                failureLabel = "Setup";
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
                "This setup app is not the game. It downloads private portable build tools and "
                "public source, then builds the playable program locally from your ROM. You do "
                "not need to install Git, Python, or work with source code yourself.");
            ImGui::Spacing();
            ImGui::TextUnformatted(
                "You must provide your own lawfully obtained GoldenEye 007 (U) cartridge dump. "
                "No ROM, game assets, or playable binary is included. .z64, .v64, and .n64 "
                "files are supported; your selected file never leaves this computer.");
            ImGui::Spacing();
            ImGui::TextUnformatted(
                "The first run downloads about 60-80 MB of private bootstrap tools, followed by "
                "the source and build dependencies. The tools stay under your Windows user "
                "profile and do not need administrator access.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (!startErr.empty()) {
                ui_error(startErr.c_str());
                ImGui::Spacing();
            }
            if (ImGui::Button("Continue", ImVec2(120, 32))) {
                startErr.clear();
                if (portable_tools_ready()) {
                    if (activate_portable_tools(&startErr)) state = haveRepo ? PICK_ROM : PICK_DEST;
                } else {
                    failureLabel = "Build-tool download";
                    if (start_tool_bootstrap(&pipeline, &startErr)) state = BOOTSTRAPPING;
                }
            }
            break;
        }
        case BOOTSTRAPPING: {
            EnterCriticalSection(&pipeline.lock);
            bool finished = pipeline.finished;
            DWORD code = pipeline.exitCode;
            ui_step_label("Preparing private build tools...");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "The setup app is downloading portable Git and embeddable Python, then checking "
                "their published SHA-256 values. They are used only by this setup process.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::BeginChild("bootstraplog", ImVec2(0, -8.0f), true);
            ImGuiListClipper bootstrapClipper;
            bootstrapClipper.Begin((int) pipeline.lines.size());
            while (bootstrapClipper.Step()) {
                for (int i = bootstrapClipper.DisplayStart; i < bootstrapClipper.DisplayEnd; i++) {
                    ImGui::TextUnformatted(pipeline.lines[(size_t) i].c_str());
                }
            }
            bootstrapClipper.End();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            LeaveCriticalSection(&pipeline.lock);

            if (finished) {
                if (code == 0) {
                    std::string activateErr;
                    if (activate_portable_tools(&activateErr)) {
                        reset_pipeline(&pipeline);
                        state = haveRepo ? PICK_ROM : PICK_DEST;
                    } else {
                        EnterCriticalSection(&pipeline.lock);
                        pipeline.lines.push_back(activateErr);
                        pipeline.exitCode = 1;
                        LeaveCriticalSection(&pipeline.lock);
                        failureLabel = "Build-tool activation";
                        state = FAILED;
                    }
                } else {
                    failureLabel = "Build-tool download";
                    state = FAILED;
                }
            }
            break;
        }
        case PICK_DEST: {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "This copy of the wizard is not inside a GoldenEye 007 installation yet. Choose "
                "where to put one -- this downloads the project's source, not the game itself.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::PushItemWidth(-90.0f);
            ImGui::InputText("##destpath", destPath, sizeof destPath);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(80, 0))) {
                char picked[MAX_PATH];
                if (open_folder_dialog(picked, sizeof picked)) {
                    snprintf(destPath, sizeof destPath, "%s\\goldeneye-native", picked);
                }
            }
            ImGui::Spacing();
            if (!cloneErr.empty()) {
                ui_error(cloneErr.c_str());
                ImGui::Spacing();
            }
            bool canClone = destPath[0] != '\0';
            if (!canClone) ImGui::BeginDisabled();
            if (ImGui::Button("Download and install here", ImVec2(220, 32))) {
                cloneErr.clear();
                failureLabel = "Source download";
                if (start_clone(&pipeline, destPath, &cloneErr)) state = CLONING;
            }
            if (!canClone) ImGui::EndDisabled();
            break;
        }
        case CLONING: {
            EnterCriticalSection(&pipeline.lock);
            bool finished = pipeline.finished;
            DWORD code = pipeline.exitCode;
            ui_step_label("Downloading GoldenEye 007...");
            ImGui::Spacing();
            ImGui::BeginChild("clonelog", ImVec2(0, -8.0f), true);
            ImGuiListClipper cloneClipper;
            cloneClipper.Begin((int) pipeline.lines.size());
            while (cloneClipper.Step()) {
                for (int i = cloneClipper.DisplayStart; i < cloneClipper.DisplayEnd; i++) {
                    ImGui::TextUnformatted(pipeline.lines[(size_t) i].c_str());
                }
            }
            cloneClipper.End();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            LeaveCriticalSection(&pipeline.lock);

            if (finished) {
                if (code == 0) {
                    snprintf(repoRoot, sizeof repoRoot, "%s", destPath);
                    haveRepo = true;
                    /* Reused for the real setup run next -- reset everything the reader thread
                     * touched, but not the lock, which stays initialized for the process
                     * lifetime (see the comment where it's created). */
                    reset_pipeline(&pipeline);
                    state = PICK_ROM;
                } else {
                    failureLabel = "Source download";
                    state = FAILED;
                }
            }
            break;
        }
        case PICK_ROM: {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "Select your own legally obtained GoldenEye 007 (U) ROM file. The wizard accepts "
                ".z64, .v64, or .n64 byte order, verifies it locally, and never uploads it.");
            ImGui::PopTextWrapPos();
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
                if (!CreateDirectoryA(romsDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    copyErr = "Could not create the local roms/ folder in the installation.";
                    break;
                }
                snprintf(dest, sizeof dest, "%s\\roms\\ge007.u.z64", repoRoot);
                copyErr.clear();
                if (!import_rom(romPath, dest, romCheck.order, &copyErr)) {
                    /* import_rom supplied a user-facing error and left the original untouched. */
                } else if (start_pipeline(&pipeline, repoRoot, &startErr)) {
                    failureLabel = "Setup";
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

            /* Held across frames on purpose: between two banners currentStep is whatever the
             * last one was, and during the clone it is not one of ours at all. Keeping the last
             * recognised index means the bar waits rather than resetting to zero. */
            static int lastStepIdx = -1;
            {
                const int idx = setup_step_index(step);
                if (idx >= 0) lastStepIdx = idx;
            }
            {
                /* Fraction is steps COMPLETED, so the bar is never full while work is still
                 * running -- a bar that sits at 100% for ten minutes reads as a hang. */
                const float frac = (lastStepIdx < 0) ? 0.0f
                                 : (float) lastStepIdx / (float) kSetupStepCount;
                char label[64];
                if (lastStepIdx < 0) {
                    snprintf(label, sizeof label, "starting");
                } else {
                    snprintf(label, sizeof label, "step %d of %d", lastStepIdx + 1, kSetupStepCount);
                }
                ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), label);
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(
                "This takes 10 to 40 minutes and only has to happen once. It is safe to leave "
                "it running and do something else. Do not close this window.");
            ImGui::PopTextWrapPos();

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
            snprintf(hdr, sizeof hdr, "%s failed (exit code %lu).", failureLabel.c_str(),
                     (unsigned long) pipeline.exitCode);
            ui_error(hdr);
            ImGui::PushTextWrapPos(0.0f);
            /* Deliberately not "run setup-windows.sh from a git-bash prompt", which is what
             * this said first. Someone who reached this screen by double-clicking an .exe does
             * not have a git-bash prompt and does not want one; telling them to get one is how
             * a fixable error becomes an abandoned install. What they CAN do is hand the log to
             * somebody who reads logs, so the button below exists to make that one click. */
            ImGui::TextUnformatted(
                "Nothing on this computer has been damaged and nothing needs undoing. The log "
                "below says what went wrong.\n\n"
                "Press 'Copy the log', then paste it into a new issue at\n"
                "github.com/seb-patron/goldeneye-native/issues -- that is enough for someone "
                "to tell you what to do next.");
            /* The two halves genuinely differ and saying so costs one line. Setup resumes: every
             * step checks for its own output first, so running it again continues. A half-done
             * DOWNLOAD does not, because git refuses to clone into a folder that is not empty --
             * telling someone to just try again there sends them back to the same error. */
            ImGui::TextUnformatted(
                failureLabel == "Source download"
                    ? "Before trying again, delete the folder it was downloading into, or choose "
                      "an empty one. A part-finished source download cannot be resumed in place."
                    : "Running this setup app again is safe: verified tool downloads are reused, "
                      "and the build picks up completed steps rather than starting over.");
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
            if (ImGui::Button("Copy the log", ImVec2(160, 32))) {
                /* Rebuilt from the line vector rather than kept as a running string: the log is
                 * a few hundred lines at worst, and this only runs on a click. */
                std::string all;
                EnterCriticalSection(&pipeline.lock);
                for (size_t i = 0; i < pipeline.lines.size(); i++) {
                    all += pipeline.lines[i];
                    all += "\r\n";   /* CRLF: this gets pasted into Windows programs. */
                }
                LeaveCriticalSection(&pipeline.lock);
                ImGui::SetClipboardText(all.c_str());
            }
            ImGui::SameLine();
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
