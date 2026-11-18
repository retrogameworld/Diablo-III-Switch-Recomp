// Diablo III first-time setup (Claude 2026-11-17, the tester: "let users put their nsps on data and let the game build
// everything it needs to run / must: base game + update / optional: dlc", with their own prod.keys).
//
// `lasrecomp.exe --d3-setup` (started by Diablo3Recomp.exe, tools/d3_starter.c, which shows the progress) in the data
// folder: the player's NSPs and prod.keys are looked for in the data folder, its subfolders and the folder above it.
//   1. base game (0100...BE000) + update 2.7.7 (0100...BE800): the update's program NCA is unpacked together with the
//      base one into extracted/exefs and extracted/romfs by bin/hactool.exe (ISC; our copy reads an NCA where it
//      sits inside the NSP: --ncaoffset). Title keys come from the NSPs' own tickets, the rest from prod.keys.
//   2. the game code: runtime/gamebuild.c (generator + bundled compiler) -> extracted/lasgame.dll.
//   3. DLC NSPs (0100...BF0xx) are copied into dlc/ (runtime/services/misc.c reads them there).
// It prints one line per event on stdout for the starter: "STEP <text>", "PROG <0..1000>", "FAIL <text>", "READY".
// Nothing is changed when everything is there already (then it only prints READY, after copying any new DLC).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "gamebuild.h"
#include "gamelib.h"
#include "paths.h"
#include "rt.h"

#define D3_BASE_TID 0x01001B300B9BE000ull
#define D3_UPD_TID 0x01001B300B9BE800ull
#define D3_AOC_MASK 0xFFFFFFFFFFFFF000ull
#define D3_AOC_TID 0x01001B300B9BF000ull
static const char D3_BUILD_ID[] = "2607a74f5df7754cc0357b5df7e496931355d8ca";  // update 2.7.7 (v1441792)
static const unsigned long long D3_ROMFS_BYTES = 17144415892ull;  // its merged romfs (37 files)

static void out(const char *kind, const char *fmt, ...) {
    char b[1200];
    int n = snprintf(b, sizeof b, "%s ", kind);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    // (the starter's pipe: its handle value in D3_SETUP_OUT - the log setup, runtime/platform/windows.c, takes over
    // stdout itself)
    static HANDLE pipe;
    if (!pipe) {
        const char *e = getenv("D3_SETUP_OUT");
        pipe = e && *e ? (HANDLE)(uintptr_t)strtoull(e, NULL, 16) : GetStdHandle(STD_OUTPUT_HANDLE);
    }
    DWORD w;
    WriteFile(pipe, b, (DWORD)n, &w, NULL);
    LOG("[d3setup] %.*s", n, b);
}
static int fail(const char *fmt, ...) {
    char b[1000], e[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t o = 0;  // (one line: its line breaks as the two characters \ and n, tools/d3_starter.c)
    for (const char *p = b; *p && o < sizeof e - 3; p++) {
        if (*p == '\n') e[o++] = '\\', e[o++] = 'n';
        else e[o++] = *p;
    }
    e[o] = 0;
    out("FAIL", "%s", e);
    return 1;
}

// ---- the player's files
typedef struct {
    wchar_t path[MAX_PATH * 2];
    uint64_t tid;          // from the ticket (or the file name)
    uint64_t prog_off;     // the program NCA (the biggest NCA) inside the NSP
    uint64_t prog_size;
    char keys[4][80];      // "rightsid = titlekey" lines
    int nkeys;
} Nsp;
static Nsp g_nsp[64];
static int g_nnsp;
static wchar_t g_keys[MAX_PATH * 2];

static bool read_at(FILE *f, uint64_t off, void *buf, size_t n) {
    return _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}
static uint64_t tid_from_name(const wchar_t *p) {
    const wchar_t *s = wcsstr(p, L"[0100");
    return s ? wcstoull(s + 1, NULL, 16) : 0;
}
static void add_nsp(const wchar_t *path) {
    if (g_nnsp >= 64) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    Nsp *n = &g_nsp[g_nnsp];
    memset(n, 0, sizeof *n);
    wcsncpy(n->path, path, MAX_PATH * 2 - 1);
    uint8_t h[16];
    if (!read_at(f, 0, h, 16) || memcmp(h, "PFS0", 4)) { fclose(f); return; }
    uint32_t cnt, strsz;
    memcpy(&cnt, h + 4, 4), memcpy(&strsz, h + 8, 4);
    if (cnt == 0 || cnt > 4096 || strsz > (1u << 20)) { fclose(f); return; }
    uint8_t *ents = malloc((size_t)cnt * 0x18), *strs = malloc(strsz + 1);
    if (!ents || !strs || !read_at(f, 16, ents, (size_t)cnt * 0x18) || !read_at(f, 16 + (uint64_t)cnt * 0x18, strs, strsz)) {
        free(ents), free(strs), fclose(f);
        return;
    }
    strs[strsz] = 0;
    uint64_t data = 16 + (uint64_t)cnt * 0x18 + strsz;
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, ents + i * 0x18, 8), memcpy(&size, ents + i * 0x18 + 8, 8), memcpy(&no, ents + i * 0x18 + 16, 4);
        if (no >= strsz) continue;
        const char *name = (const char *)strs + no;
        size_t len = strlen(name);
        if (len > 4 && !_stricmp(name + len - 4, ".tik") && size >= 0x2b0 && n->nkeys < 4) {
            uint8_t t[0x2b0];
            if (read_at(f, data + off, t, sizeof t)) {
                char *k = n->keys[n->nkeys++];
                int m = 0;
                for (int j = 0; j < 16; j++) m += sprintf(k + m, "%02x", t[0x2a0 + j]);
                m += sprintf(k + m, " = ");
                for (int j = 0; j < 16; j++) m += sprintf(k + m, "%02x", t[0x180 + j]);
                uint64_t tid = 0;
                for (int j = 0; j < 8; j++) tid = tid << 8 | t[0x2a0 + j];
                if (!n->tid) n->tid = tid;
            }
        } else if (len > 4 && !_stricmp(name + len - 4, ".nca") && !(len > 9 && !_stricmp(name + len - 9, ".cnmt.nca")) &&
                   size > n->prog_size) {
            n->prog_off = data + off, n->prog_size = size;
        }
    }
    free(ents), free(strs), fclose(f);
    if (!n->tid) n->tid = tid_from_name(path);
    if (n->tid && n->prog_size) g_nnsp++;
}

static bool exists_w(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// *.nsp and prod.keys in dir (and, depth 1, in its subfolders but extracted/)
static void scan(const wchar_t *dir, int depth) {
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wchar_t p[MAX_PATH * 2];
        _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        size_t len = wcslen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && _wcsicmp(fd.cFileName, L"extracted") && _wcsicmp(fd.cFileName, L"bin")) scan(p, depth - 1);
        } else if (len > 4 && !_wcsicmp(fd.cFileName + len - 4, L".nsp")) {
            add_nsp(p);
        } else if (!_wcsicmp(fd.cFileName, L"prod.keys") && !g_keys[0]) {
            wcsncpy(g_keys, p, MAX_PATH * 2 - 1);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- hactool
static HANDLE g_job;
static int run_wait(wchar_t *cmd, const wchar_t *log, void (*tick)(void)) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE lf = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, 0, NULL);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = lf;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (!ok) return -1;
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    while (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT)
        if (tick) tick();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return (int)code;
}

static bool main_is_277(const char *exefs_main) {
    FILE *f = fopen(exefs_main, "rb");
    if (!f) return false;
    uint8_t id[20];
    bool ok = fseek(f, 0x40, SEEK_SET) == 0 && fread(id, 1, 20, f) == 20;
    fclose(f);
    char hex[41];
    for (int i = 0; ok && i < 20; i++) sprintf(hex + 2 * i, "%02x", id[i]);
    return ok && !strcmp(hex, D3_BUILD_ID);
}
static bool romfs_complete(void) { return rt_tree_size("extracted/romfs") >= D3_ROMFS_BYTES; }

static int g_last_prog = -1;
static void prog(double f) {
    int p = (int)(f * 1000);
    if (p < 0) p = 0;
    if (p > 1000) p = 1000;
    if (p != g_last_prog) out("PROG", "%d", p), g_last_prog = p;
}
static void unpack_tick(void) {  // unpacking: the first 40%
    prog(0.4 * (double)rt_tree_size("extracted/romfs") / (double)D3_ROMFS_BYTES);
}
// The game code's bar (the tester 2026-11-17: "make the % progress more accurate, i dont want people to think its stuck"):
// gamebuild.c counts the bytes of C compiled, but one piece (main+0x1c9b40, one huge function) takes ~10 minutes on its
// own while the other 562 are done in ~3: the bytes said 97% for all that time. Now: the bulk moves the bar to 60% of
// this stage; from there it keeps moving with the time, against an estimate (3x what the bulk took, at least 2 minutes),
// slowing as it nears the end but never standing still; the text says what is going on, with a running clock.
static void build_progress(double f, const char *step) {
    static uint64_t t0, t90, last_text;
    uint64_t now = GetTickCount64();
    if (!t0) t0 = now;
    bool finishing = step && !strcmp(step, "Finishing...");
    double shown;
    if (f >= 1) shown = 1;
    else if (finishing) shown = 0.985;
    else if (f < 0.9) shown = 0.6 * f / 0.9;
    else {
        if (!t90) t90 = now;
        double est = 3.0 * (double)(t90 - t0);
        if (est < 120000) est = 120000;
        shown = 0.6 + 0.38 * (1 - exp(-2.0 * (double)(now - t90) / est));
    }
    prog(0.4 + 0.6 * shown);
    if (now - last_text >= 1000 || finishing) {  // (once a second: the clock shows it is working)
        last_text = now;
        unsigned s = (unsigned)((now - t0) / 1000);
        if (finishing) out("STEP", "Building the game code: finishing (%u:%02u)", s / 60, s % 60);
        else if (t90) out("STEP", "Building the game code: almost done - the last part is the biggest and takes a few more minutes (%u:%02u)", s / 60, s % 60);
        else out("STEP", "Building the game code (one time, 10-30 minutes): %s (%u:%02u)", step ? step : "", s / 60, s % 60);
    }
}

// the DLC packs into dlc/ (only new ones; the player's own files are left where they are)
static int copy_dlc(void) {
    int n = 0;
    CreateDirectoryW(L"dlc", NULL);
    wchar_t dlc_dir[MAX_PATH * 2];
    GetFullPathNameW(L"dlc", MAX_PATH * 2, dlc_dir, NULL);
    for (int i = 0; i < g_nnsp; i++) {
        if ((g_nsp[i].tid & D3_AOC_MASK) != D3_AOC_TID || g_nsp[i].tid == D3_AOC_TID) continue;
        const wchar_t *name = wcsrchr(g_nsp[i].path, L'\\');
        name = name ? name + 1 : g_nsp[i].path;
        if (!_wcsnicmp(g_nsp[i].path, dlc_dir, wcslen(dlc_dir)) && g_nsp[i].path[wcslen(dlc_dir)] == L'\\') continue;
        wchar_t to[MAX_PATH * 2];
        _snwprintf(to, MAX_PATH * 2, L"%ls\\%ls", dlc_dir, name);
        if (!exists_w(to) && CopyFileW(g_nsp[i].path, to, TRUE)) n++;
    }
    return n;
}

// After a good setup (the tester 2026-11-17): the player's NSPs and prod.keys go out of the data folder into a folder next to
// Diablo3Recomp.exe, with a note that they can be deleted - the game doesn't need them any more (the DLC packs it
// reads were copied into dlc\). Moved, never deleted; only files of this game, only from the data folder (dlc\ stays).
static const wchar_t TIDY_DIR[] = L"Your game files (no longer needed)";
static void tidy_inputs(const wchar_t *here, const wchar_t *up) {
    wchar_t dir[MAX_PATH * 2], dlc_dir[MAX_PATH * 2];
    _snwprintf(dir, MAX_PATH * 2, L"%ls\\%ls", up, TIDY_DIR);
    _snwprintf(dlc_dir, MAX_PATH * 2, L"%ls\\dlc\\", here);
    size_t hn = wcslen(here);
    const wchar_t *move[66];
    int n = 0;
    for (int i = 0; i < g_nnsp && n < 64; i++) {
        const wchar_t *p = g_nsp[i].path;
        bool ours = g_nsp[i].tid == D3_BASE_TID || g_nsp[i].tid == D3_UPD_TID || (g_nsp[i].tid & D3_AOC_MASK) == D3_AOC_TID;
        if (ours && !_wcsnicmp(p, here, hn) && p[hn] == L'\\' && _wcsnicmp(p, dlc_dir, wcslen(dlc_dir))) move[n++] = p;
    }
    if (g_keys[0] && !_wcsnicmp(g_keys, here, hn) && g_keys[hn] == L'\\') move[n++] = g_keys;
    if (!n) return;
    CreateDirectoryW(dir, NULL);
    int moved = 0;
    for (int i = 0; i < n; i++) {
        const wchar_t *name = wcsrchr(move[i], L'\\');
        name = name ? name + 1 : move[i];
        wchar_t to[MAX_PATH * 2];
        _snwprintf(to, MAX_PATH * 2, L"%ls\\%ls", dir, name);
        for (int k = 2; GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES && k < 100; k++)
            _snwprintf(to, MAX_PATH * 2, L"%ls\\(%d) %ls", dir, k, name);
        if (MoveFileExW(move[i], to, MOVEFILE_COPY_ALLOWED)) moved++;
        else LOG("[d3setup] could not move %ls (%lu)\n", name, (unsigned long)GetLastError());
    }
    wchar_t note[MAX_PATH * 2];
    _snwprintf(note, MAX_PATH * 2, L"%ls\\READ ME - you can delete this folder.txt", dir);
    FILE *f = _wfopen(note, L"w");
    if (f) {
        fputs("These are the game files you put in the data folder: your Diablo III NSPs and your prod.keys.\n\n"
              "Diablo III Recomp has unpacked what it needs from them and does not use them any more\n"
              "(your DLC is kept in data\\dlc). You can delete this folder to free the space,\n"
              "or keep it as a backup.\n", f);
        fclose(f);
    }
    LOG("[d3setup] %d of your files moved to \"%ls\"\n", moved, TIDY_DIR);
}

// The menu's Diablo fonts from a PC copy of Diablo III, if the player has one in a usual place (runtime/d3_pcfonts.c;
// PC Settings has a button for any other place). Once: extracted/.setup/pcfonts_tried marks it.
bool d3_pcfonts_present(void);
bool d3_pcfonts_find_install(char *out, size_t n);
bool d3_pcfonts_extract(const char *dir, char *why, size_t why_n);
// Add-on content that holds real data - the voice packs (a player: "the game says the voice pack can't be found and
// offers to download it from the eShop"). The game's DLC item packs are ownership tokens with one tiny placeholder
// file, served as they are by services/fs.c; a voice pack's data NCA holds the voice files. Its RomFS is saved once as
// one image, dlc\<add-on id>\romfs.bin, which fs.c serves when the game opens that add-on's data. Made from the copy
// in dlc\ (the player's originals are moved aside after setup), so a pack put in the data folder later is done at the
// next start. prod.keys: from the data folder, else from where setup moved it.
#define AOC_DATA_MIN (8ull << 20)  // (the token packs are a few hundred KB)
static void unpack_aoc_data(const wchar_t *up) {
    for (int i = 0; i < g_nnsp; i++) {
        Nsp *n = &g_nsp[i];
        uint64_t min = getenv("D3_AOC_MIN_KB") ? strtoull(getenv("D3_AOC_MIN_KB"), NULL, 10) << 10 : AOC_DATA_MIN;  // (test)
        if ((n->tid & D3_AOC_MASK) != D3_AOC_TID || n->tid == D3_AOC_TID || n->prog_size < min) continue;
        wchar_t dir[MAX_PATH], img[MAX_PATH], tmp[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"dlc\\%016llx", (unsigned long long)n->tid);
        _snwprintf(img, MAX_PATH, L"%ls\\romfs.bin", dir);
        _snwprintf(tmp, MAX_PATH, L"%ls\\romfs.bin.part", dir);
        if (exists_w(img)) continue;
        wchar_t keys[MAX_PATH * 2];
        wcscpy(keys, g_keys);
        if (!keys[0]) _snwprintf(keys, MAX_PATH * 2, L"%ls\\%ls\\prod.keys", up, TIDY_DIR);
        if (!exists_w(keys)) {
            LOG("[d3setup] add-on %016llx holds data, but prod.keys is not here: not unpacked\n", (unsigned long long)n->tid);
            continue;
        }
        wchar_t hac[MAX_PATH * 2];
        char h8[1100];
        wchar_t *e = _wgetenv(L"LARECOMP_HACTOOL");
        if (e && *e) wcsncpy(hac, e, MAX_PATH * 2 - 1);
        else if (rt_resource("hactool.exe", NULL, h8, sizeof h8)) MultiByteToWideChar(CP_ACP, 0, h8, -1, hac, MAX_PATH * 2);
        else return;
        out("STEP", "Unpacking an add-on (a voice pack, one time)...");
        CreateDirectoryW(L"extracted", NULL);
        CreateDirectoryW(L"extracted\\.setup", NULL);
        CreateDirectoryW(L"extracted\\.setup\\home", NULL);
        CreateDirectoryW(L"extracted\\.setup\\home\\.switch", NULL);
        CreateDirectoryW(dir, NULL);
        if (!CopyFileW(keys, L"extracted\\.setup\\home\\.switch\\prod.keys", FALSE)) continue;
        FILE *tk = _wfopen(L"extracted\\.setup\\home\\.switch\\title.keys", L"w");
        if (tk) {
            for (int k = 0; k < n->nkeys; k++) fprintf(tk, "%s\n", n->keys[k]);
            fclose(tk);
        }
        wchar_t home[MAX_PATH * 2], log[MAX_PATH * 2], cmd[MAX_PATH * 8];
        GetFullPathNameW(L"extracted\\.setup\\home", MAX_PATH * 2, home, NULL);
        GetFullPathNameW(L"extracted\\.setup\\hactool.log", MAX_PATH * 2, log, NULL);
        SetEnvironmentVariableW(L"HOME", home);
        SetEnvironmentVariableW(L"USERPROFILE", home);
        _snwprintf(cmd, MAX_PATH * 8, L"\"%ls\" --ncaoffset=%llu --romfs=\"%ls\" \"%ls\"", hac,
                   (unsigned long long)n->prog_off, tmp, n->path);
        int rc = run_wait(cmd, log, NULL);
        DeleteFileW(L"extracted\\.setup\\home\\.switch\\prod.keys");  // (our copies for hactool: not kept)
        DeleteFileW(L"extracted\\.setup\\home\\.switch\\title.keys");
        WIN32_FILE_ATTRIBUTE_DATA a;
        bool ok = GetFileAttributesExW(tmp, GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > 0x50);
        if (ok) ok = MoveFileExW(tmp, img, MOVEFILE_REPLACE_EXISTING);
        LOG("[d3setup] add-on %016llx: its data %s (hactool exit %d)\n", (unsigned long long)n->tid,
            ok ? "unpacked into dlc\\" : "could NOT be unpacked", rc);
    }
}

static void try_pc_fonts(void) {
    if (d3_pcfonts_present() || exists_w(L"extracted\\.setup\\pcfonts_tried")) return;
    CreateDirectoryW(L"extracted\\.setup", NULL);
    FILE *m = _wfopen(L"extracted\\.setup\\pcfonts_tried", L"w");
    if (m) fclose(m);
    char dir[1100], why[300];
    if (!d3_pcfonts_find_install(dir, sizeof dir)) return;
    out("STEP", "Copying the Diablo fonts from your PC copy of Diablo III...");
    bool ok = d3_pcfonts_extract(dir, why, sizeof why);
    LOG("[d3setup] fonts from the PC copy in %s: %s\n", dir, ok ? "done" : why);
}

int d3_setup_main(void) {
    g_job = CreateJobObjectW(NULL, NULL);  // (hactool ends with this process: the starter's Cancel)
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    wchar_t here[MAX_PATH * 2], up[MAX_PATH * 2];
    GetCurrentDirectoryW(MAX_PATH * 2, here);  // (the data folder: rt_paths_init)
    wcscpy(up, here);
    wchar_t *s = wcsrchr(up, L'\\');
    if (s) *s = 0;
    scan(here, 1);
    scan(up, 0);
    {
        int n = copy_dlc();
        if (n) LOG("[d3setup] %d DLC pack(s) copied into dlc\\\n", n);
    }
    unpack_aoc_data(up);
    CreateDirectoryW(L"extracted", NULL);
    bool have_game = main_is_277("extracted/exefs/main") && romfs_complete();
    if (have_game && gamelib_present("extracted")) {
        try_pc_fonts();
        tidy_inputs(here, up);
        out("READY", "");
        return 0;
    }
    if (!have_game) {
        Nsp *base = NULL, *upd[16];
        int nupd = 0;
        for (int i = 0; i < g_nnsp; i++) {
            if (g_nsp[i].tid == D3_BASE_TID && !base) base = &g_nsp[i];
            if (g_nsp[i].tid == D3_UPD_TID && nupd < 16) upd[nupd++] = &g_nsp[i];
        }
        if (!g_keys[0]) {
            const wchar_t *const env[] = {L"USERPROFILE", L"APPDATA"};
            const wchar_t *const rel[] = {L"\\.switch\\prod.keys", L"\\Ryujinx\\system\\prod.keys"};
            for (int i = 0; i < 2 && !g_keys[0]; i++) {
                wchar_t e[MAX_PATH], p[MAX_PATH * 2];
                if (GetEnvironmentVariableW(env[i], e, MAX_PATH)) {
                    _snwprintf(p, MAX_PATH * 2, L"%ls%ls", e, rel[i]);
                    if (exists_w(p)) wcscpy(g_keys, p);
                }
            }
        }
        if (!base && !nupd)
            return fail("No Diablo III game files found.\n\nPut these in the data folder (next to Diablo3Recomp.exe), then start again:\n"
                        "  - your Diablo III base game NSP\n  - your Diablo III update 2.7.7 NSP\n  - your prod.keys\n"
                        "  - (optional) your Diablo III DLC NSPs");
        if (!base) return fail("The Diablo III base game NSP is missing.\n\nPut your base game NSP in the data folder (next to the update), then start again.");
        if (!nupd) return fail("The Diablo III update NSP is missing.\n\nThe game needs update 2.7.7 (v1441792). Put your update NSP in the data folder, then start again.");
        if (!g_keys[0]) return fail("prod.keys is missing.\n\nPut the prod.keys from your own Switch (firmware 22.5.0 or newer) in the data folder, then start again.");
        ULARGE_INTEGER freeb;
        if (GetDiskFreeSpaceExW(here, &freeb, NULL, NULL) && freeb.QuadPart < D3_ROMFS_BYTES + (4ull << 30))
            return fail("Not enough free disk space.\n\nThe game needs about 21 GB free on this drive (%.1f GB free now).",
                        (double)freeb.QuadPart / (1 << 30));
        // the keys hactool reads: <data>/extracted/.setup/home/.switch/{prod,title}.keys (HOME points there)
        CreateDirectoryW(L"extracted\\.setup", NULL);
        CreateDirectoryW(L"extracted\\.setup\\home", NULL);
        CreateDirectoryW(L"extracted\\.setup\\home\\.switch", NULL);
        if (!CopyFileW(g_keys, L"extracted\\.setup\\home\\.switch\\prod.keys", FALSE))
            return fail("prod.keys could not be read.");
        FILE *tk = _wfopen(L"extracted\\.setup\\home\\.switch\\title.keys", L"w");
        if (!tk) return fail("The setup could not write in the data folder.");
        for (int i = 0; i < g_nnsp; i++)  // (each rights id once: hactool stops on a repeated one - a DLC pack can be
            for (int k = 0; k < g_nsp[i].nkeys; k++) {  // both in the data folder and in dlc\)
                bool seen = false;
                for (int j = 0; j <= i && !seen; j++)
                    for (int l = 0; l < (j < i ? g_nsp[j].nkeys : k) && !seen; l++)
                        seen = !strncmp(g_nsp[j].keys[l], g_nsp[i].keys[k], 32);
                if (!seen) fprintf(tk, "%s\n", g_nsp[i].keys[k]);
            }
        fclose(tk);
        wchar_t home[MAX_PATH * 2], hac[MAX_PATH * 2], log[MAX_PATH * 2];
        GetFullPathNameW(L"extracted\\.setup\\home", MAX_PATH * 2, home, NULL);
        GetFullPathNameW(L"extracted\\.setup\\hactool.log", MAX_PATH * 2, log, NULL);
        SetEnvironmentVariableW(L"HOME", home);
        SetEnvironmentVariableW(L"USERPROFILE", home);
        {
            char h8[1100];
            wchar_t *e = _wgetenv(L"LARECOMP_HACTOOL");
            if (e && *e) wcsncpy(hac, e, MAX_PATH * 2 - 1);
            else if (rt_resource("hactool.exe", NULL, h8, sizeof h8)) MultiByteToWideChar(CP_ACP, 0, h8, -1, hac, MAX_PATH * 2);
            else return fail("This copy of Diablo III Recomp is incomplete (bin\\hactool.exe is missing). Download it again.");
        }
        // exefs first (seconds): it tells the version; then the romfs (minutes)
        out("STEP", "Checking your game files...");
        prog(0);
        Nsp *u = NULL;
        for (int i = 0; i < nupd && !u; i++) {
            wchar_t cmd[MAX_PATH * 8];
            _snwprintf(cmd, MAX_PATH * 8, L"\"%ls\" --ncaoffset=%llu --basenca=\"%ls\" --basencaoffset=%llu --exefsdir=extracted\\exefs \"%ls\"",
                       hac, (unsigned long long)upd[i]->prog_off, base->path, (unsigned long long)base->prog_off, upd[i]->path);
            int rc = run_wait(cmd, log, NULL);
            LOG("[d3setup] exefs from update %d: exit %d\n", i, rc);
            if (main_is_277("extracted/exefs/main")) u = upd[i];
        }
        if (!u) {
            FILE *f = fopen("extracted/exefs/main", "rb");
            if (f) {
                fclose(f);
                return fail("Your update NSP is not version 2.7.7.\n\nDiablo III Recomp needs update 2.7.7 (v1441792). Put that update NSP in the data folder, then start again.");
            }
            return fail("Your game files could not be unpacked.\n\nYour prod.keys may be too old: use prod.keys from your own Switch on firmware 22.5.0 or newer. (Details: data\\extracted\\.setup\\hactool.log)");
        }
        out("STEP", "Unpacking the game (one time, about 5-15 minutes)...");
        wchar_t cmd[MAX_PATH * 8];
        _snwprintf(cmd, MAX_PATH * 8, L"\"%ls\" --ncaoffset=%llu --basenca=\"%ls\" --basencaoffset=%llu --romfsdir=extracted\\romfs \"%ls\"",
                   hac, (unsigned long long)u->prog_off, base->path, (unsigned long long)base->prog_off, u->path);
        int rc = run_wait(cmd, log, unpack_tick);
        LOG("[d3setup] romfs: exit %d, %llu bytes\n", rc, rt_tree_size("extracted/romfs"));
        DeleteFileW(L"extracted\\.setup\\home\\.switch\\prod.keys");  // (our copy for hactool: not kept)
        DeleteFileW(L"extracted\\.setup\\home\\.switch\\title.keys");
        if (!romfs_complete())
            return fail("Unpacking the game stopped before it was done (%d).\n\nCheck the free disk space, then start again. (Details: data\\extracted\\.setup\\hactool.log)", rc);
    }
    // the game code
    out("STEP", "Building the game code (one time, 10-30 minutes)...");
    prog(0.4);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    int cores = (int)si.dwNumberOfProcessors, ram_gb = (int)(ms.ullTotalPhys >> 30);
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1, NULL};
    char why[300] = "";
    if (!gamebuild_run("extracted", &o, why, sizeof why))
        return fail("The game code could not be built: %s", why);
    if (!gamelib_present("extracted")) return fail("The game code was built but does not match this version of Diablo III Recomp.");
    prog(1);
    try_pc_fonts();
    tidy_inputs(here, up);
    out("READY", "");
    return 0;
}
#else
int d3_setup_main(void) { return 1; }
#endif
