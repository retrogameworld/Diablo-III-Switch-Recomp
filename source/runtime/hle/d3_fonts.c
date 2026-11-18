// Diablo III's own fonts, saved from the player's running game for our menu (Claude 2026-11-17, the tester: "Can't you use the
// fonts from the switch game?"). The game hands each font to the SDK's font engine: nn::fontll::ScalableFontEngine::
// LoadFont(this, name buffer, data, index, name buffer size), after nn::fontll::ScalableFontEngineHelper::Decode(data,
// size) has undone the font's packing. Both are watched (hle_override: the game reaches them through its import table)
// and each font is written once to cache/d3_fonts/<its name>.<ttf|otf> - the player's own game, on their own PC, never
// shared. runtime/ui/menu.mm uses them from the next start. D3_FONT_LOG=1: log what is seen.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cpu.h"
#include "game_addresses.h"
#include "rt.h"

#define SDK_LOADFONT 0x700419d0f0ull  // _ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj
#define SDK_DECODE 0x700419da40ull    // _ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj

GuestFn dispatch_lookup(uint64_t addr);
void hle_override(uint64_t addr, HostFn fn, const char *name);

static bool font_log(void) {
    static int v = -1;
    if (v < 0) v = getenv("D3_FONT_LOG") != NULL;
    return v;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// a plain font file's size from its table directory (0: not one)
static uint64_t sfnt_size(const uint8_t *p) {
    uint32_t tag = be32(p);
    if (tag != 0x00010000u && tag != 0x4F54544Fu && tag != 0x74727565u) return 0;
    uint16_t nt = be16(p + 4);
    if (nt < 4 || nt > 64) return 0;
    uint64_t total = 0;
    for (uint16_t i = 0; i < nt; i++) {
        const uint8_t *r = p + 12 + i * 16;
        uint64_t end = (uint64_t)be32(r + 8) + be32(r + 12);
        if (end > (128u << 20)) return 0;
        if (end > total) total = end;
    }
    return total;
}

static void save(const char *name, const uint8_t *p, uint64_t n) {
    char safe[80];
    size_t k = 0;
    for (const char *s = name; *s && k + 1 < sizeof safe; s++)
        safe[k++] = (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '-' || *s == '_' ? *s : '_';
    safe[k] = 0;
    if (!k) return;
    mkdir("cache", 0755);
    mkdir("cache/d3_fonts", 0755);
    char path[200];
    snprintf(path, sizeof path, "cache/d3_fonts/%s.%s", safe, be32(p) == 0x4F54544Fu ? "otf" : "ttf");
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    char tmp[220];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return;
    bool ok = fwrite(p, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok) rename(tmp, path);
    else remove(tmp);
    LOG("[d3fonts] saved %s (%llu bytes)\n", path, (unsigned long long)n);
}

static uint64_t g_last_data, g_last_size;  // (Decode's buffer: the size LoadFont isn't told)

static void on_decode(Cpu *c) {
    uint64_t data = c->x[0], size = c->x[1];
    dispatch_lookup(SDK_DECODE)(c);
    g_last_data = data, g_last_size = size;
    if (font_log())
        LOG("[d3fonts] Decode(%llx, %llu) -> %llx; starts %08x\n", (unsigned long long)data, (unsigned long long)size,
            (unsigned long long)c->x[0], data ? be32((const uint8_t *)(uintptr_t)data) : 0);
}

static void on_loadfont(Cpu *c) {
    uint64_t nameb = c->x[1], data = c->x[2], index = c->x[3], nlen = c->x[4];
    dispatch_lookup(SDK_LOADFONT)(c);
    const uint8_t *p = (const uint8_t *)(uintptr_t)data;
    uint64_t n = p ? sfnt_size(p) : 0;
    char name[80] = "";
    if (nameb && nlen) snprintf(name, sizeof name, "%.*s", (int)(nlen < 79 ? nlen : 79), (const char *)(uintptr_t)nameb);
    if (font_log())
        LOG("[d3fonts] LoadFont(name buf %llx, data %llx, index %llu) -> %llx: \"%s\", starts %08x, font size %llu (last "
            "Decode %llx %llu)\n", (unsigned long long)nameb, (unsigned long long)data, (unsigned long long)index,
            (unsigned long long)c->x[0], name, p ? be32(p) : 0, (unsigned long long)n, (unsigned long long)g_last_data,
            (unsigned long long)g_last_size);
    if (n && name[0] && !getenv("D3_NO_FONT_SAVE")) save(name, p, n);
}

void d3_fonts_init(void) {
    if (!game_is_d3()) return;
    hle_override(SDK_DECODE, on_decode, "d3 font decode");
    hle_override(SDK_LOADFONT, on_loadfont, "d3 font load");
}
