// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) { return kernel_new_readable_event(); }

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u (add-on content)
// Diablo III's DLC packs on the Switch hold no content - one placeholder file each ("intentionally empty"): owning the
// pack is what unlocks its items. So the player's own packs are looked for in <data folder>\dlc\ (the .nsp files as
// they are; no keys needed): each one's add-on ID is read from the names in its file table (the ticket is named after
// it), and those - and only those - are reported to the game as installed. Other games: none, as before.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include "../paths.h"
extern bool game_is_d3(void);
#define D3_AOC_BASE 0x01001B300B9BF000ull
static uint32_t aoc_idx[64];
static int aoc_n = -1;
static void aoc_scan(void) {
    if (aoc_n >= 0) return;
    aoc_n = 0;
    if (!game_is_d3() || getenv("D3_NO_DLC")) return;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/dlc", rt_data_root());
    DIR *d = opendir(dir);
    if (!d) { LOG("[aoc] no DLC folder (%s): none installed\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) && aoc_n < 64) {
        size_t l = strlen(e->d_name);
        if (l == 16 && strspn(e->d_name, "0123456789abcdefABCDEF") == 16) {  // (dlc\<id>\romfs.bin: an unpacked voice
            uint64_t id = strtoull(e->d_name, NULL, 16);                       //  pack - installed without its .nsp)
            char img[1500];
            snprintf(img, sizeof img, "%s/%s/romfs.bin", dir, e->d_name);
            FILE *t = fopen(img, "rb");
            if (t && (id & ~0xFFFull) == D3_AOC_BASE && (id & 0xFFF)) {
                bool dup = false;
                for (int i = 0; i < aoc_n; i++) dup |= aoc_idx[i] == (uint32_t)(id & 0xFFF);
                if (!dup) aoc_idx[aoc_n++] = (uint32_t)(id & 0xFFF), LOG("[aoc] DLC %016llx installed (unpacked data)\n", (unsigned long long)id);
            }
            if (t) fclose(t);
            continue;
        }
        if (l < 5 || strcmp(e->d_name + l - 4, ".nsp") != 0) continue;
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint8_t h[16];
        uint32_t found = 0;
        if (fread(h, 1, 16, f) == 16 && !memcmp(h, "PFS0", 4)) {
            uint32_t nf = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24, st = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
            if (nf && nf < 64 && st && st < 0x10000) {
                char *names = malloc(st + 1);
                if (names && !fseek(f, 16 + 0x18 * (long)nf, SEEK_SET) && fread(names, 1, st, f) == st) {
                    names[st] = 0;
                    for (char *q = names; q < names + st; q += strlen(q) + 1) {  // ("<add-on ID><key gen>.tik")
                        size_t ql = strlen(q);
                        if (ql < 20 || strcmp(q + ql - 4, ".tik") != 0) continue;
                        char hex[17];
                        memcpy(hex, q, 16); hex[16] = 0;
                        uint64_t id = strtoull(hex, NULL, 16);
                        if ((id & ~0xFFFull) == D3_AOC_BASE && (id & 0xFFF)) found = (uint32_t)(id & 0xFFF);
                    }
                }
                free(names);
            }
        }
        fclose(f);
        if (!found) { LOG("[aoc] %s: not one of this game's DLC packs\n", e->d_name); continue; }
        bool dup = false;
        for (int i = 0; i < aoc_n; i++) dup |= aoc_idx[i] == found;
        if (!dup) aoc_idx[aoc_n++] = found, LOG("[aoc] DLC %016llx installed (%s)\n", (unsigned long long)(D3_AOC_BASE + found), e->d_name);
    }
    closedir(d);
}
bool aoc_owned(uint64_t id) {  // (fs.c: the DLC's data storage)
    aoc_scan();
    for (int i = 0; i < aoc_n; i++) if (D3_AOC_BASE + aoc_idx[i] == id) return true;
    return false;
}
SERVICE("aoc:u", aoc_u) {
    (void)s;
    aoc_scan();
    switch (req->cmd) {
    case 0: case 2: OUT(uint32_t, 0) = (uint32_t)aoc_n; resp_size(resp, 4); break;  // CountAddOnContent(ByApplicationId)
    case 1: case 3: {  // ListAddOnContent(ByApplicationId)(offset, count) -> B buffer of indices
        uint32_t off = IN(uint32_t, 0), cnt = IN(uint32_t, 4), w = 0;
        IpcBuf *b = out_buf(req, 0);
        uint32_t cap = b ? (uint32_t)(b->size / 4) : 0;
        for (uint32_t i = off; i < (uint32_t)aoc_n && w < cnt && w < cap; i++, w++)
            ((uint32_t *)(uintptr_t)b->addr)[w] = aoc_idx[i];
        OUT(uint32_t, 0) = w; resp_size(resp, 4);
        LOG("[aoc] ListAddOnContent(%u, %u): %u\n", off, cnt, w);
        break;
    }
    case 4: case 5:  // GetAddOnContentBaseId(ByApplicationId)
        OUT(uint64_t, 0) = game_is_d3() ? D3_AOC_BASE : 0x01006BB00C6F1000ull; resp_size(resp, 8); break;
    case 6: case 7: LOG("[aoc] PrepareAddOnContent %u\n", IN(uint32_t, 0)); break;
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}
