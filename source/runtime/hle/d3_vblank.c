// The display's refresh, for hle/d3_monitor.c: the time of a recent refresh and the refresh period, on plat_now_ns()'s
// clock. Windows only (the compositor's timing, dwmapi - loaded at run time, nothing to link); elsewhere: not known.
// Kept apart from the rest: windows.h and the runtime's headers don't mix well.
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>

int d3_vblank_info(uint64_t *vblank_ns, uint64_t *period_ns) {
    typedef HRESULT(WINAPI *TimingFn)(HWND, DWM_TIMING_INFO *);
    static TimingFn fn;
    static int tried;
    static LARGE_INTEGER qpf;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("dwmapi.dll");
        if (m) fn = (TimingFn)(void *)GetProcAddress(m, "DwmGetCompositionTimingInfo");
        QueryPerformanceFrequency(&qpf);
    }
    if (!fn || !qpf.QuadPart) return 0;
    DWM_TIMING_INFO ti;
    ZeroMemory(&ti, sizeof ti);
    ti.cbSize = sizeof ti;
    if (fn(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod) return 0;
    *vblank_ns = (uint64_t)((unsigned __int128)ti.qpcVBlank * 1000000000u / (uint64_t)qpf.QuadPart);
    *period_ns = (uint64_t)((unsigned __int128)ti.qpcRefreshPeriod * 1000000000u / (uint64_t)qpf.QuadPart);
    return *period_ns != 0;
}
#else
int d3_vblank_info(uint64_t *vblank_ns, uint64_t *period_ns) { (void)vblank_ns; (void)period_ns; return 0; }
#endif
