// Sampling profiler (bring-up, Windows): LARECOMP_SAMPLE=<from>:<to> - between those presented frames, every ~1 ms
// each thread of the process is paused for a moment and where it is (its instruction pointer) is counted, by thread.
// Written to build/samples.txt at <to>: "tid module+offset count", most first; game code is in lasgame.dll (resolve
// with nm -n build/lasgame.dll: functions are f_<guest address>). Finds where a long frame's time goes - fnprof only
// counts calls. Diagnostic only: pausing threads perturbs timing a little.
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <mmsystem.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "platform.h"

#define NS (1 << 16)
typedef struct { uint32_t tid; uint64_t rip; uint32_t count; } Sample;
static Sample tab[NS];
static _Atomic int running;
static HANDLE th;

static void add(uint32_t tid, uint64_t rip) {
    rip &= ~0xFull;  // (16-byte buckets)
    uint64_t h = (rip ^ (uint64_t)tid * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull;
    for (uint32_t j = (uint32_t)(h >> 40) & (NS - 1), n = 0; n < NS; j = (j + 1) & (NS - 1), n++) {
        if (tab[j].count && (tab[j].tid != tid || tab[j].rip != rip)) continue;
        tab[j].tid = tid, tab[j].rip = rip, tab[j].count++;
        return;
    }
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    timeBeginPeriod(1);
    // (the threads' handles: listed again every 100 rounds - opening each one per round made a round ~50 ms)
    static HANDLE hs[1024];
    static DWORD ids[1024];
    int nh = 0;
    for (unsigned round = 0; atomic_load(&running); round++) {
        if (round % 100 == 0) {
            for (int i = 0; i < nh; i++) CloseHandle(hs[i]);
            nh = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {.dwSize = sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok && nh < 1024; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (t) hs[nh] = t, ids[nh++] = te.th32ThreadID;
            }
            CloseHandle(snap);
        }
        for (int i = 0; i < nh; i++) {
            if (SuspendThread(hs[i]) == (DWORD)-1) continue;
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
            if (GetThreadContext(hs[i], &c)) add(ids[i], c.Rip);
            ResumeThread(hs[i]);
        }
        Sleep(1);
    }
    for (int i = 0; i < nh; i++) CloseHandle(hs[i]);
    timeEndPeriod(1);
    return 0;
}

static int cmp(const void *a, const void *b) {
    uint32_t x = ((const Sample *)a)->count, y = ((const Sample *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

void sampler_frame(uint64_t frame) {
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_SAMPLE");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }
    if (from < 0) return;
    if ((long)frame == from && !atomic_load(&running)) {
        atomic_store(&running, 1);
        th = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
        SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
        LOG("[sample] on at frame %llu\n", (unsigned long long)frame);
    }
    if ((long)frame != to) return;
    atomic_store(&running, 0);
    WaitForSingleObject(th, 2000);
    // module of each address: the modules' bases
    HMODULE mods[512];
    DWORD need = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need);
    int nm = (int)(need / sizeof(HMODULE));
    if (nm > 512) nm = 512;
    MODULEINFO mi[512];
    char names[512][64];
    for (int i = 0; i < nm; i++) {
        GetModuleInformation(GetCurrentProcess(), mods[i], &mi[i], sizeof mi[i]);
        char p[MAX_PATH] = "";
        GetModuleBaseNameA(GetCurrentProcess(), mods[i], p, sizeof p);
        snprintf(names[i], sizeof names[i], "%s", p);
    }
    Sample *rows = malloc(sizeof tab);
    int n = 0;
    uint64_t total = 0;
    for (int i = 0; i < NS; i++) if (tab[i].count) rows[n++] = tab[i], total += tab[i].count;
    qsort(rows, (size_t)n, sizeof rows[0], cmp);
    FILE *f = fopen("build/samples.txt", "w");
    for (int i = 0; f && i < n; i++) {
        const char *mod = "?";
        uint64_t off = rows[i].rip;
        for (int m = 0; m < nm; m++) {
            uint64_t b = (uint64_t)(uintptr_t)mi[m].lpBaseOfDll;
            if (off >= b && off < b + mi[m].SizeOfImage) { mod = names[m]; off -= b; break; }
        }
        fprintf(f, "%u %s+%llx %u\n", rows[i].tid, mod, (unsigned long long)off, rows[i].count);
    }
    if (f) fclose(f);
    free(rows);
    LOG("[sample] %llu samples, %d places, written build/samples.txt (frames %ld-%ld)\n", (unsigned long long)total, n, from, to);
    from = -1;
}
#else
#include <stdint.h>
void sampler_frame(uint64_t frame) { (void)frame; }
#endif
