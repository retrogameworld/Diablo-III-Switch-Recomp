// Diablo3Recomp.exe: the starter in the top of the packaged build (Claude 2026-11-17, the tester: "pack up the game to look more
// professional with less files lurking around"). The program itself and its libraries are in bin\, the game's data
// (game files, settings, saves, caches, resources) in data\. (The program keeps its own name: the recompiled game code,
// lasgame.dll, is linked against it.)
//
// Every start: bin\lasrecomp.exe --d3-setup (runtime/d3_setup.c) checks the game files. Ready -> the game starts at
// once. Not yet (first start: the player's base + update NSPs and prod.keys in data\) -> this window shows the setup's
// progress (unpacking, building the game code), then the game starts. Closing the window stops the setup; the next
// start picks up where it left off.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_SETUP_LINE (WM_APP + 1)
#define WM_SETUP_END (WM_APP + 2)
static const wchar_t TITLE[] = L"Diablo III Recomp";
static HWND g_win, g_text, g_bar, g_note;
static HANDLE g_job, g_read;
static bool g_shown, g_done;
static wchar_t g_exe[MAX_PATH + 64], g_data[MAX_PATH + 64];

static void fail_box(const wchar_t *msg) {
    if (g_win) ShowWindow(g_win, SW_HIDE);
    MessageBoxW(NULL, msg, TITLE, MB_OK | MB_ICONERROR);
}

static DWORD WINAPI reader(LPVOID arg) {
    (void)arg;
    char buf[4096], line[2048];
    size_t n = 0;
    DWORD got;
    while (ReadFile(g_read, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n == sizeof line - 1) {
                line[n] = 0;
                int wn = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
                wchar_t *w = malloc(wn * sizeof(wchar_t));
                if (w) {
                    MultiByteToWideChar(CP_UTF8, 0, line, -1, w, wn);
                    PostMessageW(g_win, WM_SETUP_LINE, 0, (LPARAM)w);
                }
                n = 0;
            } else if (buf[i] != '\r') {
                line[n++] = buf[i];
            }
        }
    }
    PostMessageW(g_win, WM_SETUP_END, 0, 0);
    return 0;
}

static const wchar_t *g_args = L"";
static bool start_game(void) {
    wchar_t cmd[MAX_PATH + 600];
    _snwprintf(cmd, MAX_PATH + 600, L"\"%ls\" --play %ls", g_exe, g_args);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(g_exe, cmd, NULL, NULL, FALSE, 0, NULL, g_data, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// the FAIL text's line breaks come as the two characters \ and n (one line per message: runtime/d3_setup.c)
static void unescape(wchar_t *s) {
    wchar_t *o = s;
    for (wchar_t *p = s; *p; p++) {
        if (p[0] == L'\\' && p[1] == L'n') { *o++ = L'\n'; p++; }
        else *o++ = *p;
    }
    *o = 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SETUP_LINE: {
        wchar_t *w = (wchar_t *)lp;
        if (!wcsncmp(w, L"STEP ", 5)) {
            SetWindowTextW(g_text, w + 5);
            if (!g_shown) { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); g_shown = true; }
        } else if (!wcsncmp(w, L"PROG ", 5)) {
            SendMessageW(g_bar, PBM_SETPOS, (WPARAM)_wtoi(w + 5), 0);
        } else if (!wcsncmp(w, L"FAIL ", 5)) {
            g_done = true;
            unescape(w + 5);
            fail_box(w + 5);
            DestroyWindow(h);
        } else if (!wcsncmp(w, L"READY", 5)) {
            g_done = true;
            if (!start_game()) fail_box(L"Diablo III could not be started.");
            DestroyWindow(h);
        }
        free(w);
        return 0;
    }
    case WM_SETUP_END:
        if (!g_done) {
            g_done = true;
            fail_box(L"The setup stopped unexpectedly.\n\nStart Diablo3Recomp.exe again; it continues where it left off. "
                     L"(Details: data\\lasrecomp.log)");
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        if (g_done || MessageBoxW(h, L"Stop the setup?\n\nNext time it continues where it left off.", TITLE,
                                  MB_YESNO | MB_ICONQUESTION) == IDYES) {
            g_done = true;
            if (g_job) TerminateJobObject(g_job, 1);
            DestroyWindow(h);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)prev; (void)show;
    if (args && wcslen(args) < 500) g_args = args;
    wchar_t here[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, here, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(here, L'\\');
    if (slash) *slash = 0;
    _snwprintf(g_exe, MAX_PATH + 64, L"%ls\\bin\\lasrecomp.exe", here);
    _snwprintf(g_data, MAX_PATH + 64, L"%ls\\data", here);
    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(g_data) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The bin and data folders must be next to Diablo3Recomp.exe.\n\nKeep the whole folder together.",
                    TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    {   // (the data folder is the program's own: runtime/paths.c takes it as the data root when extracted\ is there)
        wchar_t ex[MAX_PATH + 80];
        _snwprintf(ex, MAX_PATH + 80, L"%ls\\extracted", g_data);
        CreateDirectoryW(ex, NULL);
    }
    SetEnvironmentVariableW(L"LARECOMP_NO_LAUNCHER", L"1");

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"D3RecompSetup";
    RegisterClassExW(&wc);
    UINT dpi = 96;
    HDC sdc = GetDC(NULL);
    if (sdc) { dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(NULL, sdc); }
#define S(v) MulDiv((v), (int)dpi, 96)
    RECT r = {0, 0, S(520), S(150)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    g_win = CreateWindowExW(0, wc.lpszClassName, L"Diablo III Recomp - first-time setup", style,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt,
                            NULL, NULL, inst, NULL);
    HFONT font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_text = CreateWindowExW(0, L"STATIC", L"Checking your game files...", WS_CHILD | WS_VISIBLE, S(20), S(18), S(480),
                             S(44), g_win, NULL, inst, NULL);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, S(20), S(70), S(480), S(20), g_win, NULL,
                            inst, NULL);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    g_note = CreateWindowExW(0, L"STATIC", L"This happens once. You can close this window to stop; it continues next time.",
                             WS_CHILD | WS_VISIBLE, S(20), S(104), S(480), S(36), g_win, NULL, inst, NULL);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_note, WM_SETFONT, (WPARAM)small, TRUE);

    // the setup, its output into a pipe; it and what it starts end with this process (the job)
    g_job = CreateJobObjectW(NULL, NULL);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE wr;
    if (!CreatePipe(&g_read, &wr, &sa, 0)) return 1;
    SetHandleInformation(g_read, HANDLE_FLAG_INHERIT, 0);
    wchar_t cmd[MAX_PATH + 100];
    _snwprintf(cmd, MAX_PATH + 100, L"\"%ls\" --d3-setup", g_exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    {   // (its log setup takes over stdout: runtime/d3_setup.c writes to this handle, inherited)
        wchar_t hv[32];
        _snwprintf(hv, 32, L"%llx", (unsigned long long)(uintptr_t)wr);
        SetEnvironmentVariableW(L"D3_SETUP_OUT", hv);
    }
    BOOL started = CreateProcessW(g_exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, g_data, &si, &pi);
    SetEnvironmentVariableW(L"D3_SETUP_OUT", NULL);
    if (!started) {
        MessageBoxW(NULL, L"Diablo III could not be started.", TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(wr);
    CreateThread(NULL, 0, reader, NULL, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
