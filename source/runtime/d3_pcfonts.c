// Diablo III's own menu fonts from the player's PC copy of Diablo III (Claude 2026-11-17, the tester: "give the player an option
// to extract the menu fonts from their pc copy"). Read on the player's PC from their own Battle.net install (CASC, read
// only) and written to the data folder - nothing is shipped:
//   build/d3_title_font.otf  Exocet Light (SNO 52792)   titles, tabs, "Loading..."
//   build/d3_sans_font.ttf   the sans of the console Options screen (SNO 72350)   rows, descriptions
//   build/d3_body_font.ttf   D3 Serif (SNO 72348)        (the rows' face when the sans isn't there)
// runtime/ui/menu.mm uses them before its free look-alikes. Asked for from PC Settings (a folder picker), and tried by
// the first-time setup (runtime/d3_setup.c) on the usual install folders.
//
// CASC read the way src/analysis/casc.py does (the publicly documented layout: .build.info, build config, index files,
// data archives, BLTE "N" / "Z" chunks, the encoding file), then Diablo III's root (named sub-roots: "Base" lists
// SNO id -> content key). Each font SNO carries a plain OpenType / TrueType file; its table directory gives its size.
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "rt.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct { uint8_t key[9]; uint8_t arch; uint32_t off, size; } IdxEnt;
typedef struct {
    char data[1100];
    IdxEnt *idx;
    size_t nidx;
    uint8_t *enc;
    size_t enc_n;
} Casc;

static bool rd_file(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = len > 0 ? malloc((size_t)len + 1) : NULL;
    bool ok = *out && fread(*out, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!ok) { free(*out); *out = NULL; return false; }
    (*out)[len] = 0;
    *n = (size_t)len;
    return true;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool unhex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int a = hexv(s[2 * i]), b = a < 0 ? -1 : hexv(s[2 * i + 1]);
        if (b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}
static int idx_cmp(const void *a, const void *b) { return memcmp(a, b, 9); }

// BLTE -> plain bytes
static uint8_t *blte(const uint8_t *d, size_t n, size_t *out_n) {
    if (n < 8 || memcmp(d, "BLTE", 4)) return NULL;
    uint32_t hs = be32(d + 4);
    size_t cap = 1 << 20, len = 0;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    uint32_t chunks = 1;
    const uint8_t *info = NULL;
    size_t at = 8;
    if (hs) {
        if (hs > n || n < 12) goto bad;
        chunks = (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
        info = d + 12;
        at = hs;
    }
    for (uint32_t c = 0; c < chunks; c++) {
        size_t cs = hs ? be32(info + c * 24) : n - 8, dsz = hs ? be32(info + c * 24 + 4) : 0;
        if (at + cs > n || cs < 1) goto bad;
        const uint8_t *ch = d + at;
        size_t need = len + (dsz ? dsz : (ch[0] == 'N' ? cs : cs * 8 + 4096));
        if (need > cap) {
            while (cap < need) cap *= 2;
            uint8_t *o2 = realloc(out, cap);
            if (!o2) goto bad;
            out = o2;
        }
        if (ch[0] == 'N') {
            memcpy(out + len, ch + 1, cs - 1);
            len += cs - 1;
        } else if (ch[0] == 'Z') {
            for (;;) {
                uLongf dl = (uLongf)(cap - len);
                int r = uncompress(out + len, &dl, ch + 1, (uLong)(cs - 1));
                if (r == Z_OK) { len += dl; break; }
                if (r != Z_BUF_ERROR || cap > (1u << 30)) goto bad;
                cap *= 2;
                uint8_t *o2 = realloc(out, cap);
                if (!o2) goto bad;
                out = o2;
            }
        } else goto bad;  // (encrypted chunks: not for fonts)
        at += cs;
    }
    *out_n = len;
    return out;
bad:
    free(out);
    return NULL;
}

static uint8_t *read_ekey(Casc *c, const uint8_t *ekey, size_t *n) {
    IdxEnt k;
    memcpy(k.key, ekey, 9);
    IdxEnt *e = bsearch(&k, c->idx, c->nidx, sizeof *c->idx, idx_cmp);
    if (!e || e->size < 0x1e) return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/data.%03u", c->data, e->arch);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *raw = malloc(e->size);
    bool ok = raw && _fseeki64(f, e->off, SEEK_SET) == 0 && fread(raw, 1, e->size, f) == e->size;
    fclose(f);
    uint8_t *out = ok ? blte(raw + 0x1e, e->size - 0x1e, n) : NULL;
    free(raw);
    return out;
}

// content key -> encoded key (the encoding file's pages)
static bool ekey_of(Casc *c, const uint8_t *ckey, uint8_t *ekey) {
    const uint8_t *d = c->enc;
    if (c->enc_n < 22 || d[0] != 'E' || d[1] != 'N') return false;
    int ck = d[3], ek = d[4];
    uint32_t pg = (uint32_t)(d[5] << 8 | d[6]) * 1024, npg = be32(d + 9), espec = be32(d + 18);
    size_t o = 22 + (size_t)espec + (size_t)npg * 32;
    for (uint32_t p = 0; p < npg; p++) {
        size_t q = o + (size_t)p * pg, end = q + pg;
        if (end > c->enc_n) return false;
        // (pages are sorted: the page header's first key tells whether this page can hold it)
        const uint8_t *first = d + 22 + espec + (size_t)p * 32;
        if (p + 1 < npg && memcmp(d + 22 + espec + (size_t)(p + 1) * 32, ckey, (size_t)ck) <= 0) continue;
        if (memcmp(first, ckey, (size_t)ck) > 0) return false;
        while (q + 6 + ck <= end) {
            int kc = d[q];
            if (!kc) break;
            if (!memcmp(d + q + 6, ckey, (size_t)ck)) { memcpy(ekey, d + q + 6 + ck, (size_t)(ek < 16 ? ek : 16)); return true; }
            q += 6 + ck + (size_t)kc * ek;
        }
        return false;
    }
    return false;
}

static uint8_t *read_ckey(Casc *c, const uint8_t *ckey, size_t *n) {
    uint8_t ek[16];
    return ekey_of(c, ckey, ek) ? read_ekey(c, ek, n) : NULL;
}

static void casc_free(Casc *c) { free(c->idx), free(c->enc); }

static bool casc_open(Casc *c, const char *install, uint8_t *root_ckey) {
    memset(c, 0, sizeof *c);
    char p[1200];
    snprintf(p, sizeof p, "%s/.build.info", install);
    uint8_t *bi;
    size_t bn;
    if (!rd_file(p, &bi, &bn)) return false;
    // header "Name!TYPE:n|Name!TYPE:n|..." then the first row: the column "Build Key"
    char *l1 = (char *)bi, *l2 = strchr(l1, '\n');
    char bk[64] = "";
    if (l2) {
        *l2++ = 0;
        char *e2 = strchr(l2, '\n');
        if (e2) *e2 = 0;
        int col = 0, want = -1;
        for (char *s = l1; *s; col++) {
            char *bar = strchr(s, '|');
            if (!strncmp(s, "Build Key!", 10)) want = col;
            if (!bar) break;
            s = bar + 1;
        }
        col = 0;
        for (char *s = l2; want >= 0 && *s; col++) {
            char *bar = strchr(s, '|');
            if (col == want) { snprintf(bk, sizeof bk, "%.*s", (int)(bar ? bar - s : (ptrdiff_t)strlen(s)), s); break; }
            if (!bar) break;
            s = bar + 1;
        }
    }
    free(bi);
    for (char *s = bk; *s; s++) if (*s == '\r') *s = 0;
    if (strlen(bk) != 32) return false;
    snprintf(p, sizeof p, "%s/Data/config/%.2s/%.2s/%s", install, bk, bk + 2, bk);
    uint8_t *cfg;
    size_t cn;
    if (!rd_file(p, &cfg, &cn)) return false;
    uint8_t enc_ekey[16];
    bool have_root = false, have_enc = false;
    for (char *line = (char *)cfg; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "root = ", 7)) have_root = unhex(line + 7, root_ckey, 16);
        if (!strncmp(line, "encoding = ", 11)) {
            char *sp = strchr(line + 11, ' ');
            have_enc = sp && unhex(sp + 1, enc_ekey, 16);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(cfg);
    if (!have_root || !have_enc) return false;
    snprintf(c->data, sizeof c->data, "%s/Data/data", install);
    // the newest index file of each bucket (xxVVVVVVVV.idx)
#ifdef _WIN32
    char best[16][1200];
    uint32_t bestv[16];
    bool has[16] = {0};
    snprintf(p, sizeof p, "%s/*.idx", c->data);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strlen(fd.cFileName) != 14) continue;
        int b = hexv(fd.cFileName[0]) * 16 + hexv(fd.cFileName[1]);
        uint32_t v = (uint32_t)strtoul(fd.cFileName + 2, NULL, 16);
        if (b < 0 || b > 15) continue;
        if (!has[b] || v > bestv[b]) has[b] = true, bestv[b] = v, snprintf(best[b], sizeof best[b], "%s/%s", c->data, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    size_t cap = 0;
    for (int b = 0; b < 16; b++) {
        if (!has[b]) continue;
        uint8_t *d;
        size_t n;
        if (!rd_file(best[b], &d, &n) || n < 0x28) continue;
        uint32_t size = le32(d + 0x20);
        for (size_t o = 0x28; o + 18 <= 0x28 + (size_t)size && o + 18 <= n; o += 18) {
            if (c->nidx == cap) {
                cap = cap ? cap * 2 : 1 << 16;
                IdxEnt *ni = realloc(c->idx, cap * sizeof *ni);
                if (!ni) { free(d); return false; }
                c->idx = ni;
            }
            IdxEnt *e = &c->idx[c->nidx++];
            memcpy(e->key, d + o, 9);
            uint64_t off = 0;
            for (int i = 0; i < 5; i++) off = off << 8 | d[o + 9 + i];
            e->arch = (uint8_t)(off >> 30), e->off = (uint32_t)(off & 0x3fffffff), e->size = le32(d + o + 14);
        }
        free(d);
    }
    qsort(c->idx, c->nidx, sizeof *c->idx, idx_cmp);
#endif
    c->enc = read_ekey(c, enc_ekey, &c->enc_n);
    return c->enc != NULL;
}

// a font file inside an SNO: its start and size
static bool find_font(const uint8_t *d, size_t n, size_t *at, size_t *len) {
    for (size_t o = 0; o + 12 <= n && o < 4096; o++) {
        uint32_t tag = be32(d + o);
        if (tag != 0x4F54544Fu && tag != 0x00010000u) continue;
        uint16_t nt = (uint16_t)(d[o + 4] << 8 | d[o + 5]);
        if (nt < 4 || nt > 64 || o + 12 + nt * 16u > n) continue;
        uint64_t total = 0;
        bool ok = true;
        for (int i = 0; i < nt && ok; i++) {
            const uint8_t *r = d + o + 12 + i * 16;
            for (int k = 0; k < 4; k++) ok &= r[k] >= 32 && r[k] < 127;
            uint64_t end = (uint64_t)be32(r + 8) + be32(r + 12);
            if (end > total) total = end;
        }
        if (ok && total && o + total <= n) { *at = o, *len = (size_t)total; return true; }
    }
    return false;
}

static const struct { uint32_t sno; const char *path; } FONTS[] = {
    {52792, "build/d3_title_font.otf"}, {72350, "build/d3_sans_font.ttf"}, {72348, "build/d3_body_font.ttf"}};

bool d3_pcfonts_present(void) {
    FILE *f = fopen(FONTS[0].path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// Reads the fonts from the Diablo III install in dir (or a folder above / below it the player picked). why: what failed.
bool d3_pcfonts_extract(const char *dir, char *why, size_t why_n) {
    char install[1100];
    snprintf(install, sizeof install, "%s", dir);
    size_t L = strlen(install);
    while (L && (install[L - 1] == '/' || install[L - 1] == '\\')) install[--L] = 0;
    char p[1200];
    snprintf(p, sizeof p, "%s/.build.info", install);
    struct stat st;
    if (stat(p, &st) != 0) {  // (the picked folder may be "Data" or the Battle.net games folder above it)
        static const char *const tries[] = {"/..", "/Diablo III", "/Diablo III/"};
        bool found = false;
        for (int i = 0; i < 3 && !found; i++) {
            snprintf(p, sizeof p, "%s%s/.build.info", install, tries[i]);
            if (stat(p, &st) == 0) {
                snprintf(p, sizeof p, "%s%s", install, tries[i]);
                snprintf(install, sizeof install, "%s", p);
                found = true;
            }
        }
        if (!found) { snprintf(why, why_n, "That folder isn't a PC Diablo III install (no .build.info in it)."); return false; }
    }
    Casc c;
    uint8_t root_ckey[16];
    if (!casc_open(&c, install, root_ckey)) {
        snprintf(why, why_n, "The Diablo III install couldn't be read (is it fully installed and up to date?).");
        casc_free(&c);
        return false;
    }
    size_t rn = 0;
    uint8_t *root = read_ckey(&c, root_ckey, &rn);
    // D3 root: magic, count, then {content key, name} - the "Base" sub-root lists the SNOs
    uint8_t base_ckey[16];
    bool have_base = false;
    if (root && rn >= 8 && le32(root) == 0x8007D0C4u) {
        uint32_t cnt = le32(root + 4);
        size_t o = 8;
        for (uint32_t i = 0; i < cnt && o + 17 <= rn; i++) {
            const uint8_t *ck = root + o;
            const char *name = (const char *)root + o + 16;
            size_t nl = strnlen(name, rn - o - 16);
            if (nl == 4 && !memcmp(name, "Base", 4)) memcpy(base_ckey, ck, 16), have_base = true;
            o += 16 + nl + 1;
        }
    }
    free(root);
    size_t sn = 0;
    uint8_t *sub = have_base ? read_ckey(&c, base_ckey, &sn) : NULL;
    int made = 0;
    if (sub && sn >= 8) {
        uint32_t n1 = le32(sub + 4);
        mkdir("build", 0755);
        for (size_t f = 0; f < sizeof FONTS / sizeof *FONTS; f++) {
            for (uint32_t i = 0; i < n1 && 8 + (size_t)(i + 1) * 20 <= sn; i++) {
                const uint8_t *e = sub + 8 + (size_t)i * 20;
                if (le32(e + 16) != FONTS[f].sno) continue;
                size_t dn = 0;
                uint8_t *d = read_ckey(&c, e, &dn);
                size_t at, len;
                if (d && find_font(d, dn, &at, &len)) {
                    char tmp[300];
                    snprintf(tmp, sizeof tmp, "%s.tmp", FONTS[f].path);
                    FILE *o = fopen(tmp, "wb");
                    bool ok = o && fwrite(d + at, 1, len, o) == len;
                    if (o) ok &= fclose(o) == 0;
                    remove(FONTS[f].path);
                    if (ok && rename(tmp, FONTS[f].path) == 0) {
                        made++;
                        LOG("[pcfonts] %s from SNO %u (%zu bytes)\n", FONTS[f].path, FONTS[f].sno, len);
                    } else remove(tmp);
                }
                free(d);
                break;
            }
        }
    }
    free(sub);
    casc_free(&c);
    if (!made) { snprintf(why, why_n, "No fonts were found in that Diablo III install."); return false; }
    return true;
}

// The usual places of a PC install (Battle.net's default folders on each drive, and its uninstall entry)
bool d3_pcfonts_find_install(char *out, size_t n) {
#ifdef _WIN32
    HKEY k;
    static const char *const keys[] = {"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Diablo III",
                                       "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Diablo III"};
    for (int i = 0; i < 2; i++)
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_READ, &k) == ERROR_SUCCESS) {
            char v[1024];
            DWORD vn = sizeof v, type;
            LONG r = RegQueryValueExA(k, "InstallLocation", NULL, &type, (BYTE *)v, &vn);
            RegCloseKey(k);
            if (r == ERROR_SUCCESS && type == REG_SZ && vn > 1) {
                char p[1200];
                snprintf(p, sizeof p, "%s/.build.info", v);
                struct stat st;
                if (stat(p, &st) == 0) { snprintf(out, n, "%s", v); return true; }
            }
        }
    static const char *const rel[] = {":/Program Files (x86)/Diablo III", ":/Program Files/Diablo III", ":/Games/Diablo III",
                                      ":/Battle.net/Diablo III", ":/Diablo III"};
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {
        if (!(drives >> d & 1)) continue;
        for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
            char p[300];
            snprintf(p, sizeof p, "%c%s/.build.info", 'A' + d, rel[i]);
            struct stat st;
            if (stat(p, &st) == 0) { snprintf(out, n, "%c%s", 'A' + d, rel[i]); return true; }
        }
    }
#endif
    (void)out, (void)n;
    return false;
}
