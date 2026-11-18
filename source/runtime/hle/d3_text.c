// Diablo III: the game's own text without mentions of the console it was made for ("...do not turn off or disconnect
// your Nintendo Switch system." -> "...your system."). The string tables are compressed in the game's data packs, so
// they are edited where the game keeps them once unpacked: a background thread looks through the game's writable
// memory every few seconds (UTF-8 and UTF-16) and rewrites each mention in place, keeping the string's length (the
// freed bytes at the end become zeros, which end the string). Turn off with D3_KEEP_CONSOLE_TEXT=1.
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../game_addresses.h"
#include "../hle.h"

extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);

#define NEEDLE "Nintendo Switch"
#define NLEN 15

static bool lower(uint8_t c) { return c >= 'a' && c <= 'z'; }

// UTF-8 / ASCII: "Nintendo Switch <lower-case word>" loses the console's name ("system", "console" stay);
// on its own it becomes "PC". The rest of the string moves left; zeros fill the end.
static int fix8(uint8_t *p, uint8_t *end) {
    uint8_t *s = p + NLEN;
    size_t tail = 0;
    while (s + tail < end && s[tail]) tail++;  // the rest of this string
    if (s + 7 <= end && !memcmp(s, " Online", 7)) {  // "Nintendo Switch Online" -> "online services"
        static const char rep[] = "online services";
        size_t rest = tail - 7, rl = sizeof rep - 1;
        memcpy(p, rep, rl);
        memmove(p + rl, s + 7, rest);
        memset(p + rl + rest, 0, NLEN + 7 - rl);
    } else if (s < end && *s == ' ' && s + 1 < end && lower(s[1])) {
        memmove(p, s + 1, tail - 1);
        memset(p + tail - 1, 0, NLEN + 1);
    } else {
        memcpy(p, "PC", 2);
        memmove(p + 2, s, tail);
        memset(p + 2 + tail, 0, NLEN - 2);
    }
    return 1;
}

// UTF-16LE: the same, two bytes a character
static int fix16(uint16_t *p, uint16_t *end) {
    uint16_t *s = p + NLEN;
    size_t tail = 0;
    while (s + tail < end && s[tail]) tail++;
    if (s + 1 < end && s[0] == ' ' && s[1] < 128 && lower((uint8_t)s[1])) {
        memmove(p, s + 1, (tail - 1) * 2);
        memset(p + tail - 1, 0, (NLEN + 1) * 2);
    } else {
        p[0] = 'P'; p[1] = 'C';
        memmove(p + 2, s, tail * 2);
        memset(p + 2 + tail, 0, (NLEN - 2) * 2);
    }
    return 1;
}

// Button icons in text: "{icon:joycon_neutral_right, 2.0}" / "{icon:joycon_right}" are the sideways Joy-Con
// pictures (four circles, one lit) the game shows in "Press ... to join!" and the talk prompts (Stash, people) when it
// thinks a single Joy-Con is held. The face buttons have their own icons, "{icon:button1}" (A) to "button4" (Y),
// drawn as Cross / Circle / Triangle / Square with the DualSense layout: the name is swapped, the rest of the string
// moves left. Sideways, the lit circle is the button in that spot: right A, down B, up X, left Y (the tester 2026-11-15:
// talking still showed "d-pad right" after only joycon_neutral_right was swapped).
static const struct { const char *from, *to; } ICONS[] = {
    {"joycon_neutral_right", "button1"}, {"joycon_neutral_down", "button2"}, {"joycon_neutral_up", "button3"},
    {"joycon_neutral_left", "button4"},  {"joycon_right", "button1"},        {"joycon_down", "button2"},
    {"joycon_up", "button3"},            {"joycon_left", "button4"},
};
#define NICONS (int)(sizeof ICONS / sizeof ICONS[0])
#define JCN_LEN 20  // (the longest name)
static bool name_end(uint8_t c) { return c == 0 || c == '}' || c == ',' || c == ' '; }
// p: a name of ICONS at p? Its index, -1 if none (the whole name: "joycon_up" is not "joycon_upper")
static int icon_at(const uint8_t *p, const uint8_t *end) {
    for (int i = 0; i < NICONS; i++) {
        size_t l = strlen(ICONS[i].from);
        if (p + l < end && !memcmp(p, ICONS[i].from, l) && name_end(p[l])) return i;
    }
    return -1;
}
static int fix_icon_n(uint8_t *p, uint8_t *end, int i) {
    size_t fl = strlen(ICONS[i].from), tl = strlen(ICONS[i].to);
    uint8_t *s = p + fl;
    size_t tail = 0;
    while (s + tail < end && s[tail]) tail++;
    memcpy(p, ICONS[i].to, tl);
    memmove(p + tl, s, tail);
    memset(p + tl + tail, 0, fl - tl);
    return 1;
}

// Whole strings swapped for others (no longer than the original; the rest becomes zeros): the main menu's spare entry
// "Connect to Diablo Servers" is our settings menu now (runtime/hle/d3_menu.c, the tester 2026-11-15)
// (and the demo's "Quit Demo" label is the main menu's "Exit Game", the tester 2026-11-16)
static const struct { const char *from, *to; } WHOLE[] = {{"Connect to Diablo Servers", "PC Settings"},
                                                           {"Quit Demo", "Exit Game"}, {"Exit Demo", "Exit Game"}};
#define NWHOLE (int)(sizeof WHOLE / sizeof WHOLE[0])
static int fix_whole(uint8_t *a, uint64_t n, uint8_t *rend) {
    int fixed = 0;
    for (int k = 0; k < NWHOLE; k++) {
        size_t fl = strlen(WHOLE[k].from), tl = strlen(WHOLE[k].to);
        uint8_t c0 = (uint8_t)WHOLE[k].from[0];
        for (uint8_t *p = a; p < a + n && p + 2 * fl + 2 <= rend;) {
            uint8_t *q = memchr(p, c0, (size_t)(a + n - p));
            if (!q || q + 2 * fl + 2 > rend) break;
            if (!memcmp(q, WHOLE[k].from, fl) && q[fl] == 0) {  // UTF-8, the whole string
                memcpy(q, WHOLE[k].to, tl);
                memset(q + tl, 0, fl - tl);
                fixed++;
            } else if (((uintptr_t)q & 1) == 0 && q[1] == 0 && q[2 * fl] == 0 && q[2 * fl + 1] == 0) {  // UTF-16LE
                size_t i = 0;
                while (i < fl && q[2 * i] == (uint8_t)WHOLE[k].from[i] && q[2 * i + 1] == 0) i++;
                if (i == fl) {
                    for (i = 0; i < fl; i++) q[2 * i] = i < tl ? (uint8_t)WHOLE[k].to[i] : 0, q[2 * i + 1] = 0;
                    fixed++;
                }
            }
            p = q + 1;
        }
    }
    return fixed;
}

// [a, a + n) is searched; matches are fixed up to rend, the end of the region (a chunk of it is searched at a time)
static int scan(uint8_t *a, uint64_t n, uint8_t *rend) {
    int fixed = fix_whole(a, n, rend);
    {   // (the icon names first: a separate pass, they start with 'j')
        uint8_t *end = a + n + JCN_LEN < rend ? a + n + JCN_LEN : rend;
        for (uint8_t *p = a; p < a + n && p + JCN_LEN <= end;) {
            uint8_t *q = memchr(p, 'j', (size_t)(end - p - JCN_LEN + 1));
            if (!q) break;
            if (q >= a + n) break;
            int k = q > a && q[-1] == ':' ? icon_at(q, rend) : -1;
            if (k >= 0) fixed += fix_icon_n(q, rend, k);
            p = q + 1;
        }
    }
    static const uint16_t w[NLEN] = {'N', 'i', 'n', 't', 'e', 'n', 'd', 'o', ' ', 'S', 'w', 'i', 't', 'c', 'h'};
    uint8_t *end = a + n + 2 * NLEN < rend ? a + n + 2 * NLEN : rend;
    for (uint8_t *p = a; p < a + n && p + 2 * NLEN <= end;) {
        uint8_t *q = memchr(p, 'N', (size_t)(end - p - NLEN));
        if (!q || q >= a + n) break;
        if (!memcmp(q, NEEDLE, NLEN) && getenv("D3_TEXT_DRY")) {  // (bring-up: show each mention, change nothing)
            char ctx[81];
            uint8_t *from = q - 24 > a ? q - 24 : a;
            int k = 0;
            for (uint8_t *c = from; c < end && k < 80; c++) ctx[k++] = *c >= 32 && *c < 127 ? (char)*c : '.';
            ctx[k] = 0;
            LOG("[text] at %llx: %s\n", (unsigned long long)(uintptr_t)q, ctx);
        } else if (!memcmp(q, NEEDLE, NLEN)) fixed += fix8(q, rend);
        else if (((uintptr_t)q & 1) == 0 && q + 2 * NLEN <= end && !memcmp(q, w, 2 * NLEN))
            fixed += fix16((uint16_t *)q, (uint16_t *)rend);
        p = q + 1;
    }
    return fixed;
}

// D3_TEXT_FIND=<text>: log where <text> is in memory with what surrounds it (bring-up: the markup of button icons)
static void find_text(uint64_t *ra, uint64_t *rs, int nr) {
    const char *needle = getenv("D3_TEXT_FIND");
    static int hits;
    if (!needle || !*needle || hits >= 60) return;
    size_t nl = strlen(needle);
    for (int i = 0; i < nr && hits < 60; i++) {
        uint8_t *a = (uint8_t *)(uintptr_t)ra[i], *end = a + rs[i];
        for (uint8_t *p = a; p + nl <= end && hits < 60;) {
            uint8_t *q = memchr(p, needle[0], (size_t)(end - p - nl + 1));
            if (!q) break;
            if (!memcmp(q, needle, nl)) {
                char ctx[161];
                uint8_t *from = q - 80 > a ? q - 80 : a;
                int k = 0;
                for (uint8_t *c = from; c < end && k < 160; c++) ctx[k++] = *c >= 32 && *c < 127 ? (char)*c : '.';
                ctx[k] = 0;
                LOG("[text] found at %llx: %s\n", (unsigned long long)(uintptr_t)q, ctx);
                hits++;
            }
            p = q + 1;
        }
    }
}

// D3_DUMP_FSB=<folder> (bring-up): every FMOD sound bank (FSB5) the game has in memory, written once each as
// <folder>/<address>_<bytes>.fsb (the console menu sounds: the player's own game, never shared)
static void dump_fsb(uint8_t *a, uint64_t n, uint8_t *rend) {
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("D3_DUMP_FSB");
    if (!dir) return;
    static uint64_t seen[4096];
    static int nseen;
    for (uint8_t *p = a; p + 0x40 <= a + n;) {
        uint8_t *q = memchr(p, 'F', (size_t)(a + n - p));
        if (!q || q + 0x40 > rend) break;
        if (!memcmp(q, "FSB5", 4)) {
            uint32_t h[6];
            memcpy(h, q + 4, sizeof h);  // version, samples, sample headers, names, data, mode
            uint64_t hdr = h[0] == 1 ? 0x3C : 0x40, total = hdr + (uint64_t)h[2] + h[3] + h[4];
            uint64_t key = (uint64_t)(uintptr_t)q ^ total << 40;
            bool dup = false;
            for (int i = 0; i < nseen; i++) dup |= seen[i] == key;
            if (!dup && h[1] && h[1] < 100000 && total < (256u << 20) && q + total <= rend && nseen < 4096) {
                seen[nseen++] = key;
                char path[600];
                snprintf(path, sizeof path, "%s/%012llx_%llu_n%u_m%u.fsb", dir, (unsigned long long)(uintptr_t)q,
                         (unsigned long long)total, h[1], h[5]);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(q, 1, total, f); fclose(f); }
            }
        }
        p = q + 1;
    }
}

// D3_FIND_FONTS=1 (bring-up): log each font file (TrueType / OpenType) the game has in memory, with its name and size;
// D3_DUMP_FONTS=<folder> also writes each once (the player's own game, never shared)
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void font_name(const uint8_t *f, uint64_t size, const uint8_t *name, uint32_t nlen, char *out, size_t outn) {
    out[0] = 0;
    if (nlen < 6) return;
    uint16_t count = be16(name + 2), soff = be16(name + 4);
    for (int want = 4; want >= 1 && !out[0]; want -= 3)  // (full name, else family)
        for (uint16_t i = 0; i < count && 6 + (i + 1) * 12u <= nlen; i++) {
            const uint8_t *r = name + 6 + i * 12;
            uint16_t plat = be16(r), nid = be16(r + 6), len = be16(r + 8), off = be16(r + 10);
            if (nid != want || soff + off + len > nlen) continue;
            const uint8_t *str = name + soff + off;
            size_t k = 0;
            if (plat == 0 || plat == 3) { for (uint16_t j = 1; j < len && k + 1 < outn; j += 2) out[k++] = str[j] >= 32 && str[j] < 127 ? (char)str[j] : '?'; }
            else { for (uint16_t j = 0; j < len && k + 1 < outn; j++) out[k++] = str[j] >= 32 && str[j] < 127 ? (char)str[j] : '?'; }
            out[k] = 0;
            if (k) break;
        }
    (void)f, (void)size;
}
static void find_fonts(uint8_t *a, uint64_t n, uint8_t *rend) {
    static int mode = -1;
    static const char *dir;
    if (mode < 0) dir = getenv("D3_DUMP_FONTS"), mode = getenv("D3_FIND_FONTS") || dir;
    if (!mode) return;
    static uint64_t seen[256];
    static int nseen;
    for (uint8_t *p = a; p + 12 <= a + n; p++) {  // (any alignment: inside the game's own files they need not be)
        if (p[0] != 0 && p[0] != 'O' && p[0] != 't') continue;
        uint32_t tag = be32(p);
        if (tag == 0x74746366u && be16(p + 4) <= 2 && be32(p + 8) > 0 && be32(p + 8) < 64) {  // "ttcf": a collection
            static int ttc;
            if (ttc++ < 20) LOG("[font] collection (ttcf) at %llx, %u fonts\n", (unsigned long long)(uintptr_t)p, be32(p + 8));
        }
        if (tag != 0x00010000u && tag != 0x4F54544Fu && tag != 0x74727565u) continue;  // (1.0, "OTTO", "true")
        uint16_t nt = be16(p + 4);
        if (nt < 6 || nt > 48 || p + 12 + nt * 16u > rend) continue;
        uint64_t total = 0;
        uint32_t prev = 0;
        const uint8_t *name = NULL;
        uint32_t nlen = 0;
        bool ok = true;
        for (uint16_t i = 0; i < nt && ok; i++) {
            const uint8_t *r = p + 12 + i * 16;
            uint32_t t = be32(r), off = be32(r + 8), len = be32(r + 12);
            for (int k = 0; k < 4; k++) ok &= r[k] >= 32 && r[k] < 127;
            ok &= t > prev && off >= 12u + nt * 16u && (uint64_t)off + len < (64u << 20);
            prev = t;
            if (ok && (uint64_t)off + len > total) total = (uint64_t)off + len;
            if (ok && t == 0x6E616D65u) name = p + off, nlen = len;  // "name"
        }
        if (!ok || !name || p + total > rend) continue;
        uint64_t key = (uint64_t)(uintptr_t)p ^ total << 40;
        bool dup = false;
        for (int i = 0; i < nseen; i++) dup |= seen[i] == key;
        if (dup || nseen >= 256) continue;
        seen[nseen++] = key;
        char nm[96];
        font_name(p, total, name, nlen, nm, sizeof nm);
        LOG("[font] at %llx: %llu bytes, %s, \"%s\"\n", (unsigned long long)(uintptr_t)p, (unsigned long long)total,
            tag == 0x4F54544Fu ? "OpenType" : "TrueType", nm);
        if (dir) {
            char path[700];
            snprintf(path, sizeof path, "%s/%012llx_%llu.%s", dir, (unsigned long long)(uintptr_t)p, (unsigned long long)total,
                     tag == 0x4F54544Fu ? "otf" : "ttf");
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(p, 1, total, f); fclose(f); }
        }
    }
}

#define CHUNK (1u << 20)

// Only memory the game has used: reading a page it never touched makes the OS set it up (zeroed) on the spot, and the
// first scan's gigabyte of those froze the game for 1-2 s when the title screen came up. Windows: the pages in the
// working set; elsewhere all of it.
static int scan_resident(uint8_t *c, uint64_t n, uint8_t *rend) {
    dump_fsb(c, n, rend);
    find_fonts(c, n, rend);
#ifdef _WIN32
    enum { PG = 4096 };
    static PSAPI_WORKING_SET_EX_INFORMATION wi[CHUNK / PG];
    size_t np = (size_t)((n + PG - 1) / PG);
    for (size_t i = 0; i < np; i++) wi[i].VirtualAddress = c + i * PG;
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), wi, (DWORD)(np * sizeof wi[0]))) return scan(c, n, rend);
    int fixed = 0;
    for (size_t i = 0; i < np;) {
        if (!wi[i].VirtualAttributes.Valid) { i++; continue; }
        size_t j = i;
        while (j < np && wi[j].VirtualAttributes.Valid) j++;
        uint8_t *s = c + i * PG;
        uint64_t len = (uint64_t)(j - i) * PG;
        if (s + len > c + n) len = (uint64_t)(c + n - s);
        fixed += scan(s, len, rend);
        i = j;
    }
    return fixed;
#else
    return scan(c, n, rend);
#endif
}

// Rush (the tester 2026-11-16: after Quit to Title the autosave notice said "Nintendo Switch system" for a moment): for 25 s
// after the game starts (and after each in-place reboot, once it runs again) passes go back to back with no pauses, so
// the freshly loaded text is fixed before it shows. Otherwise the gentle pace (a pass takes seconds, then 3 s rest).
#include <stdatomic.h>
#include <SDL3/SDL.h>
static _Atomic uint64_t g_rush_until;  // (SDL ms)
static _Atomic int g_rush_pending = 1;  // (the first start too)
RT_ON_RESET(d3_text_reset) { atomic_store(&g_rush_pending, 1); }
static bool rushing(void) { return SDL_GetTicks() < atomic_load(&g_rush_until); }
static void *scanner(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    static uint64_t ra[4096], rs[4096];
    uint64_t total = 0;
    for (;;) {
        uint64_t t0 = SDL_GetTicks();
        bool rush = rushing();
        int nr = kernel_data_regions(ra, rs, 4096), fixed = 0;
        find_text(ra, rs, nr);
        // (a chunk at a time with a pause between: one pass over all of the game's memory at once froze the game for
        // a second every scan - 1.1 s frames on the title screen, 2026-11-15)
        for (int i = 0; i < nr; i++) {
            uint8_t *a = (uint8_t *)(uintptr_t)ra[i], *rend = a + rs[i];
            for (uint8_t *c = a; c < rend; c += CHUNK) {
                fixed += scan_resident(c, (uint64_t)(rend - c) < CHUNK ? (uint64_t)(rend - c) : CHUNK, rend);
                if (rushing()) continue;
                struct timespec ps = {0, 300000};
                nanosleep(&ps, NULL);
            }
        }
        total += fixed;
        if (fixed) LOG("[text] %d console mentions rewritten (%llu so far; pass %llu ms%s, at %llu ms)\n", fixed,
                       (unsigned long long)total, (unsigned long long)(SDL_GetTicks() - t0), rush ? ", rush" : "",
                       (unsigned long long)SDL_GetTicks());
        if (rush) { SDL_Delay(50); continue; }
        struct timespec ts = {3, 0};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// The world's interaction prompts (above the stash, people to talk to) take their icon names from the game program's
// own read-only strings, which the scanner above never sees (it reads writable memory). After loading, the main
// program's copies get the same swaps (ICONS): the face buttons' icons instead of the four-circle pictures.
void d3_patch_program_strings(void) {
    if (!game_is_d3() || getenv("D3_KEEP_CONSOLE_TEXT")) return;
    extern Module g_modules[4];
    if (getenv("D3_FIND_FONTS")) {  // (bring-up: what the program says about fonts - which rasterizer, which files)
        static const char *const needles[] = {"FreeType", "FT_Load", "stbtt", "ttcf", ".ttf", ".otf", "Font\\", "FontFile",
                                              "glyph", "Glyph", "BMFont", "Exocet"};
        for (int m = 0; m < 2; m++) {
            uint8_t *a0 = (uint8_t *)(uintptr_t)g_modules[m].base, *e0 = a0 + g_modules[m].size;
            for (size_t k = 0; k < sizeof needles / sizeof *needles; k++) {
                size_t nl = strlen(needles[k]);
                int hits = 0;
                for (uint8_t *q = a0; q + nl <= e0 && hits < 6; q++) {
                    if (*q != (uint8_t)needles[k][0] || memcmp(q, needles[k], nl)) continue;
                    uint8_t *f = q;
                    while (f > a0 && f[-1] >= 32 && f[-1] < 127 && q - f < 60) f--;
                    char ctx[121];
                    int c = 0;
                    for (uint8_t *x = f; x < e0 && c < 120 && *x >= 32 && *x < 127; x++) ctx[c++] = (char)*x;
                    ctx[c] = 0;
                    LOG("[font] module %d +%llx: %s\n", m, (unsigned long long)(q - a0), ctx);
                    hits++;
                    q += nl;
                }
            }
        }
    }
    uint8_t *a = (uint8_t *)(uintptr_t)g_modules[0].base, *end = a + g_modules[0].size;
    int n = 0;
    // "{icon:<name>...}", and the bare name as a string of its own (code that puts it into "{icon:%s}" itself)
    for (uint8_t *p = a + 1; p + JCN_LEN + 1 <= end;) {
        uint8_t *q = memchr(p, 'j', (size_t)(end - p - JCN_LEN));
        if (!q) break;
        int k = (q[-1] == 0 || (q - a >= 6 && !memcmp(q - 6, "{icon:", 6))) ? icon_at(q, end) : -1;
        if (k >= 0 && (q[-1] == ':' || q[strlen(ICONS[k].from)] == 0)) n += fix_icon_n(q, end, k);
        p = q + 1;
    }
    LOG("[text] %d Joy-Con button icons in the program's strings now face buttons\n", n);
}

// Called by the input poll (runtime/hle/input.c) once the game runs; starts the scanner once
void d3_text_start(void) {
    if (atomic_exchange(&g_rush_pending, 0)) atomic_store(&g_rush_until, SDL_GetTicks() + 25000);
    static int started;
    if (started || !game_is_d3() || getenv("D3_KEEP_CONSOLE_TEXT")) return;
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, scanner, NULL) == 0) pthread_detach(t);
    LOG("[text] console-name scanner started\n");
}
