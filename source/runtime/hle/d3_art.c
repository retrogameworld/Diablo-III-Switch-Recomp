// Diablo III's menu art for our settings menu and the PlayStation / Xbox controller page, made on the player's PC from
// their own running game (Claude 2026-11-17, the tester: "please include everything we did in the public build" / "make the
// setup do this for them"). Nothing of the game ships: the pieces are taken from the game's own textures as it uploads
// them (runtime/nvn/vulkan/textures.c calls d3_art_capture), the first time each one shows up, and written to the
// data folder:
//   build/d3_ui/{streak,smoke,flare}.rgba  the main menu's highlight pieces, cut to what they show
//   build/d3_ui/glow.rgba                  its moving glow, 48 frames of 512 x 128 (as src/analysis/d3_ui_art.py)
//   build/hdtex/87a23a48fce614ab[_xbox].dds  Options > Controller: the game's parchment with its Joy-Con drawing painted
//                                          out and our own PlayStation / Xbox line art (bin/d3_art/ctrl_*.rgba, drawn
//                                          by src/analysis/d3_controller_page.py - our art, no game content) on top
// runtime/ui/menu.mm and the HD texture path pick them up (the menu at once, the controller page at its next upload).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "game_addresses.h"
#include "paths.h"
#include "rt.h"

#define H_STREAK 0x8095855d7995a46eull
#define H_SMOKE 0xdf9495cfb3fe0166ull
#define H_FLARE 0x67fec321a2697ed8ull
#define H_VEINS 0xa7b47c14ed0546afull
#define H_CTRL 0x87a23a48fce614abull

volatile int g_d3_ui_art_gen;  // (bumped when a menu piece is written: menu.mm loads them again)
void d3_hd_reindex(void);       // (textures.c: the HD folder is read again)

typedef struct { int w, h; uint8_t *px; } Img;  // RGBA8

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}
static void mkdirs(void) {
    mkdir("build", 0755);
    mkdir("build/d3_ui", 0755);
    mkdir("build/hdtex", 0755);
}
static bool write_file(const char *path, const void *head, size_t hn, const void *data, size_t n) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = (!hn || fwrite(head, 1, hn, f) == hn) && fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    remove(path);
    if (ok && rename(tmp, path) == 0) return true;
    remove(tmp);
    return false;
}
static bool write_rgba(const char *path, const Img *im) {
    uint32_t wh[2] = {(uint32_t)im->w, (uint32_t)im->h};
    bool ok = write_file(path, wh, 8, im->px, (size_t)im->w * im->h * 4);
    LOG("[d3art] %s %s (%dx%d)\n", ok ? "made" : "could not write", path, im->w, im->h);
    return ok;
}

// BC3 (NVN format 0x44) -> RGBA8
static Img bc3_decode(const uint8_t *src, int w, int h, uint32_t row) {
    Img im = {w, h, malloc((size_t)w * h * 4)};
    if (!im.px) return im;
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            const uint8_t *b = src + (size_t)by * row + (size_t)bx * 16;
            uint8_t al[8] = {b[0], b[1]};
            if (al[0] > al[1]) for (int i = 1; i < 7; i++) al[i + 1] = (uint8_t)(((7 - i) * al[0] + i * al[1]) / 7);
            else {
                for (int i = 1; i < 5; i++) al[i + 1] = (uint8_t)(((5 - i) * al[0] + i * al[1]) / 5);
                al[6] = 0, al[7] = 255;
            }
            uint64_t ai = 0;
            for (int i = 0; i < 6; i++) ai |= (uint64_t)b[2 + i] << (8 * i);
            uint16_t c0 = (uint16_t)(b[8] | b[9] << 8), c1 = (uint16_t)(b[10] | b[11] << 8);
            uint8_t col[4][3];
            for (int k = 0; k < 2; k++) {
                uint16_t c = k ? c1 : c0;
                col[k][0] = (uint8_t)((c >> 11) * 255 / 31), col[k][1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
                col[k][2] = (uint8_t)((c & 31) * 255 / 31);
            }
            for (int ch = 0; ch < 3; ch++)
                col[2][ch] = (uint8_t)((2 * col[0][ch] + col[1][ch]) / 3), col[3][ch] = (uint8_t)((col[0][ch] + 2 * col[1][ch]) / 3);
            uint32_t ci = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
            for (int i = 0; i < 16; i++) {
                uint8_t *o = im.px + ((size_t)(by * 4 + i / 4) * w + (bx * 4 + i % 4)) * 4;
                const uint8_t *c = col[(ci >> (2 * i)) & 3];
                o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = al[(ai >> (3 * i)) & 7];
            }
        }
    return im;
}

// cut to the part whose alpha is above 6 (PIL getbbox of the thresholded alpha)
static Img crop_alpha(const Img *s) {
    int x0 = s->w, y0 = s->h, x1 = -1, y1 = -1;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (s->px[((size_t)y * s->w + x) * 4 + 3] > 6) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) x0 = 0, y0 = 0, x1 = s->w - 1, y1 = s->h - 1;
    Img o = {x1 - x0 + 1, y1 - y0 + 1, malloc((size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * 4)};
    if (o.px)
        for (int y = 0; y < o.h; y++) memcpy(o.px + (size_t)y * o.w * 4, s->px + ((size_t)(y + y0) * s->w + x0) * 4, (size_t)o.w * 4);
    return o;
}

// float RGBA (0..1) of s resized to w x h (bilinear, pixel centres)
static float *resize_f(const Img *s, int w, int h) {
    float *o = calloc((size_t)w * h * 4, sizeof(float));
    if (!o) return NULL;
    for (int y = 0; y < h; y++) {
        float fy = (y + 0.5f) * s->h / h - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - y0;
        int ya = y0 < 0 ? 0 : y0 >= s->h ? s->h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= s->h ? s->h - 1 : y0 + 1;
        for (int x = 0; x < w; x++) {
            float fx = (x + 0.5f) * s->w / w - 0.5f;
            int x0 = (int)floorf(fx);
            float tx = fx - x0;
            int xa = x0 < 0 ? 0 : x0 >= s->w ? s->w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= s->w ? s->w - 1 : x0 + 1;
            for (int c = 0; c < 4; c++) {
                float a = s->px[((size_t)ya * s->w + xa) * 4 + c], b = s->px[((size_t)ya * s->w + xb) * 4 + c];
                float d = s->px[((size_t)yb * s->w + xa) * 4 + c], e = s->px[((size_t)yb * s->w + xb) * 4 + c];
                o[((size_t)y * w + x) * 4 + c] = ((a * (1 - tx) + b * tx) * (1 - ty) + (d * (1 - tx) + e * tx) * ty) / 255.0f;
            }
        }
    }
    return o;
}

// a Gaussian blur (sigma) of one plane, as three box blurs (edges: the nearest pixel)
static void box_pass(float *p, int w, int h, int stride, int r, bool horiz, float *line) {
    int n = horiz ? w : h, m = horiz ? h : w;
    for (int j = 0; j < m; j++) {
        float *base = horiz ? p + (size_t)j * w * stride : p + (size_t)j * stride;
        size_t step = horiz ? (size_t)stride : (size_t)w * stride;
        for (int i = 0; i < n; i++) line[i] = base[i * step];
        double acc = 0;
        for (int i = -r; i <= r; i++) acc += line[i < 0 ? 0 : i >= n ? n - 1 : i];
        for (int i = 0; i < n; i++) {
            base[i * step] = (float)(acc / (2 * r + 1));
            int add = i + r + 1, sub = i - r;
            acc += line[add >= n ? n - 1 : add] - line[sub < 0 ? 0 : sub];
        }
    }
}
static void blur(float *p, int w, int h, int stride, float sigma) {
    // three boxes whose combined variance is sigma^2 (W. Jarosz, "Fast image convolutions")
    float wi = sqrtf(12 * sigma * sigma / 3 + 1);
    int wl = (int)floorf(wi);
    if (!(wl & 1)) wl--;
    int wu = wl + 2;
    float mi = (12 * sigma * sigma - 3 * wl * wl - 4 * 3 * wl - 3 * 3) / (-4.0f * wl - 4);
    int mm = (int)roundf(mi);
    float *line = malloc(sizeof(float) * (size_t)(w > h ? w : h));
    if (!line) return;
    for (int k = 0; k < 3; k++) {
        int r = ((k < mm ? wl : wu) - 1) / 2;
        if (r < 1) continue;
        box_pass(p, w, h, stride, r, true, line);
        box_pass(p, w, h, stride, r, false, line);
    }
    free(line);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the menu's highlight pieces
static Img g_piece[4];  // streak, smoke, flare, veins (decoded, whole)

static void make_glow(void) {
    if (!g_piece[0].px || !g_piece[1].px || !g_piece[3].px || exists("build/d3_ui/glow.rgba")) return;
    enum { FRAMES = 48, FW = 512, FH = 128 };
    Img streak = crop_alpha(&g_piece[0]), smoke = crop_alpha(&g_piece[1]);
    const Img *veins = &g_piece[3];
    float *st = calloc((size_t)FW * FH * 4, sizeof(float)), *smk = calloc((size_t)FW * 100 * 4, sizeof(float));
    float *mask = calloc((size_t)FW * FH, sizeof(float));
    uint8_t *atlas = malloc((size_t)FW * FH * FRAMES * 4);
    float *rs = streak.px ? resize_f(&streak, 340, 60) : NULL, *rm = smoke.px ? resize_f(&smoke, FW, 100) : NULL;
    if (!st || !smk || !mask || !atlas || !rs || !rm) goto out;
    for (int y = 0; y < 60; y++) memcpy(st + ((size_t)(y + 34) * FW + 20) * 4, rs + (size_t)y * 340 * 4, 340 * 4 * sizeof(float));
    // where the veins show: streak + smoke shape (smoke placed at y 14), widened and softened
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            float sa = y >= 14 && y < 114 ? rm[((size_t)(y - 14) * FW + x) * 4 + 3] : 0;
            mask[(size_t)y * FW + x] = floorf(clampf(st[((size_t)y * FW + x) * 4 + 3] + sa * 0.8f, 0, 1) * 255);
        }
    blur(mask, FW, FH, 1, 14);
    for (size_t i = 0; i < (size_t)FW * FH; i++) mask[i] = clampf(mask[i] / 255 * 1.6f, 0, 1);
    // the smoke made to wrap round without a seam (blended with itself shifted by half)
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < FW; x++) {
            float wx = 0.5f - 0.5f * cosf(2 * 3.14159265f * x / FW);
            int xs = (x - FW / 2 + FW) % FW;
            for (int c = 0; c < 4; c++)
                smk[((size_t)y * FW + x) * 4 + c] = rm[((size_t)y * FW + x) * 4 + c] * wx + rm[((size_t)y * FW + xs) * 4 + c] * (1 - wx);
        }
    for (int k = 0; k < FRAMES; k++) {
        float t = (float)k / FRAMES;
        for (int y = 0; y < FH; y++)
            for (int x = 0; x < FW; x++) {
                float env = powf(clampf(1.15f - x / (FW * 0.85f), 0, 1), 1.2f) * expf(-powf((y - 64.0f) / 30.0f, 2));
                float sxf = fmodf(x - t * FW, (float)FW);
                if (sxf < 0) sxf += FW;
                int sx = (int)sxf, sy = y - 14 < 0 ? 0 : y - 14 > 99 ? 99 : y - 14;
                const float *d = smk + ((size_t)sy * FW + sx) * 4;
                float sm_a = clampf(d[3] * 2.6f, 0, 1) * env * (y >= 14 && y < 114);
                float vxf = fmodf(x - t * 256, 256.0f);
                if (vxf < 0) vxf += 256;
                int vx = (int)vxf * veins->w / 256, vy = ((y + 40) % 256) * veins->h / 256;
                float v = veins->px[((size_t)vy * veins->w + vx) * 4 + 3] / 255.0f;
                float vlum = clampf((v - 0.10f) * 3, 0, 1) * mask[(size_t)y * FW + x] * 0.9f;
                float col[3] = {0, 0, 0};
                float la = sm_a * 0.85f;
                for (int c = 0; c < 3; c++) col[c] = col[c] * (1 - la) + d[c] * la;
                const float *s = st + ((size_t)y * FW + x) * 4;
                la = s[3] * 0.95f;
                for (int c = 0; c < 3; c++) col[c] = col[c] * (1 - la) + s[c] * la;
                const float vc[3] = {0.45f, 0.95f, 1.0f};
                for (int c = 0; c < 3; c++) col[c] = col[c] * (1 - vlum * 0.8f) + vc[c] * vlum * 0.95f;
                float lum = clampf(col[0] * 0.2f + col[1] * 0.5f + col[2] * 0.3f, 0, 1);
                float inten = clampf(lum * 1.35f, 0, 1);
                float hot = powf(clampf((inten - 0.55f) / 0.45f, 0, 1), 1.5f);
                const float teal[3] = {0.30f, 0.92f, 1.0f}, white[3] = {0.92f, 1.0f, 1.0f};
                float edge = powf(clampf(x / 150.0f, 0, 1), 1.3f) *
                             clampf(1.0f - (fabsf(y - 64.0f) / 64.0f) * clampf(1.0f - x / 200.0f, 0, 1), 0, 1) *
                             clampf((FW - 1 - x) / 60.0f, 0, 1);
                uint8_t *o = atlas + (((size_t)k * FH + y) * FW + x) * 4;
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)(clampf(teal[c] * (1 - hot) + white[c] * hot, 0, 1) * 255);
                o[3] = (uint8_t)(clampf(powf(inten, 1.1f) * 1.1f, 0, 1) * edge * 255);
            }
    }
    {
        Img g = {FW, FH * FRAMES, atlas};
        if (write_rgba("build/d3_ui/glow.rgba", &g)) g_d3_ui_art_gen++;
    }
out:
    free(st), free(smk), free(mask), free(atlas), free(rs), free(rm), free(streak.px), free(smoke.px);
}

static void menu_piece(int i, const char *name, const uint8_t *lin, int w, int h, uint32_t row) {
    char path[64];
    snprintf(path, sizeof path, "build/d3_ui/%s.rgba", name);
    bool need = name[0] && !exists(path);
    bool glow = (i == 0 || i == 1 || i == 3) && !exists("build/d3_ui/glow.rgba");
    if (!need && !glow) return;
    if (!g_piece[i].px) g_piece[i] = bc3_decode(lin, w, h, row);
    if (!g_piece[i].px) return;
    mkdirs();
    if (need) {
        Img c = crop_alpha(&g_piece[i]);
        if (c.px && write_rgba(path, &c)) g_d3_ui_art_gen++;
        free(c.px);
    }
    make_glow();
}

// ---- Options > Controller
static bool load_overlay(const char *name, Img *o) {
    char path[1100];
    if (!rt_resource(name, NULL, path, sizeof path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t wh[2];
    bool ok = fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 4096 && wh[1] <= 4096;
    if (ok) {
        o->w = (int)wh[0], o->h = (int)wh[1];
        o->px = malloc((size_t)o->w * o->h * 4);
        ok = o->px && fread(o->px, 1, (size_t)o->w * o->h * 4, f) == (size_t)o->w * o->h * 4;
    }
    fclose(f);
    if (!ok) free(o->px), o->px = NULL;
    return ok;
}

// RGBA8 DDS (DX10 header, UNORM) with its mip chain
static bool write_dds(const char *path, const Img *im) {
    int levels = 1;
    while ((im->w >> levels) >= 1 && (im->h >> levels) >= 1 && levels < 12) levels++;
    size_t total = 0;
    for (int l = 0; l < levels; l++) total += (size_t)(im->w >> l > 0 ? im->w >> l : 1) * (im->h >> l > 0 ? im->h >> l : 1) * 4;
    uint8_t *data = malloc(total);
    if (!data) return false;
    memcpy(data, im->px, (size_t)im->w * im->h * 4);
    uint8_t *prev = data;
    size_t at = (size_t)im->w * im->h * 4;
    int pw = im->w, ph = im->h;
    for (int l = 1; l < levels; l++) {
        int w = pw > 1 ? pw / 2 : 1, h = ph > 1 ? ph / 2 : 1;
        uint8_t *cur = data + at;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) {
                    int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1;
                    int y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
                    int s = prev[((size_t)y0 * pw + x0) * 4 + c] + prev[((size_t)y0 * pw + x1) * 4 + c] +
                            prev[((size_t)y1 * pw + x0) * 4 + c] + prev[((size_t)y1 * pw + x1) * 4 + c];
                    cur[((size_t)y * w + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                }
        prev = cur, at += (size_t)w * h * 4, pw = w, ph = h;
    }
    uint32_t hd[37];
    memset(hd, 0, sizeof hd);
    memcpy(hd, "DDS ", 4);
    hd[1] = 124, hd[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8, hd[3] = (uint32_t)im->h, hd[4] = (uint32_t)im->w;
    hd[5] = (uint32_t)im->w * 4, hd[7] = (uint32_t)levels;
    hd[19] = 32, hd[20] = 0x4, hd[21] = 0x30315844u;  // (pixel format: FOURCC "DX10")
    hd[27] = 0x1000 | 0x400000 | 0x8;                 // (caps: texture, mipmap, complex)
    hd[32] = 28, hd[33] = 3, hd[35] = 1;              // (DX10: R8G8B8A8_UNORM, 2D, one image)
    bool ok = write_file(path, hd, sizeof hd, data, total);
    free(data);
    LOG("[d3art] %s %s (%dx%d, %d levels)\n", ok ? "made" : "could not write", path, im->w, im->h, levels);
    return ok;
}

// the game's page with its Joy-Con drawing, its arrows and the icons we redraw painted out (the parchment stays):
// the parchment's colour from a wide blur around them, its grain from a clean corner on top (d3_controller_page.py)
static void clean_page(Img *im) {
    int W = im->w, H = im->h;
    size_t n = (size_t)W * H;
    float *num = malloc(n * 4 * sizeof(float)), *soft = malloc(n * sizeof(float));
    uint8_t *mask = calloc(n, 1);
    if (!num || !soft || !mask) goto out;
    static const int R[6][4] = {{565, 96, 1362, 930}, {480, 120, 590, 245}, {1345, 120, 1450, 245},
                                {480, 345, 590, 400}, {1340, 335, 1455, 405}, {1410, 478, 1548, 612}};
    for (int k = 0; k < 6; k++)
        for (int y = R[k][1]; y < R[k][3] && y < H; y++)
            for (int x = R[k][0]; x < R[k][2] && x < W; x++) mask[(size_t)y * W + x] = 1;
    for (size_t i = 0; i < n; i++) {  // (RGB times "valid", and "valid" itself in the fourth plane)
        float v = mask[i] ? 0.0f : 1.0f;
        for (int c = 0; c < 3; c++) num[i * 4 + c] = im->px[i * 4 + c] * v;
        num[i * 4 + 3] = v;
        soft[i] = (float)mask[i];
    }
    for (int c = 0; c < 4; c++) blur(num + c, W, H, 4, 260);
    blur(soft, W, H, 1, 30);
    // the grain: a 280 x 280 corner minus its own blur, tiled
    enum { TS = 280, TX = 20, TY = 60 };
    static float grain[TS * TS * 3];
    for (int c = 0; c < 3; c++) {
        static float tile[TS * TS];
        for (int y = 0; y < TS; y++)
            for (int x = 0; x < TS; x++) tile[y * TS + x] = im->px[((size_t)(y + TY) * W + x + TX) * 4 + c];
        static float bl[TS * TS];
        memcpy(bl, tile, sizeof bl);
        blur(bl, TS, TS, 1, 12);
        for (int i = 0; i < TS * TS; i++) grain[i * 3 + c] = tile[i] - bl[i];
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            float s = soft[i], den = num[i * 4 + 3] > 1e-3f ? num[i * 4 + 3] : 1e-3f;
            for (int c = 0; c < 3; c++) {
                float fill = num[i * 4 + c] / den + grain[((y % TS) * TS + x % TS) * 3 + c];
                im->px[i * 4 + c] = (uint8_t)clampf(im->px[i * 4 + c] * (1 - s) + fill * s, 0, 255);
            }
        }
out:
    free(num), free(soft), free(mask);
}

// PIL alpha_composite of art over the page at (0, 0)
static void composite(Img *dst, const Img *art) {
    for (int y = 0; y < art->h && y < dst->h; y++)
        for (int x = 0; x < art->w && x < dst->w; x++) {
            uint8_t *d = dst->px + ((size_t)y * dst->w + x) * 4;
            const uint8_t *s = art->px + ((size_t)y * art->w + x) * 4;
            float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
            if (oa <= 0) continue;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)clampf((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5f, 0, 255);
            d[3] = (uint8_t)clampf(oa * 255 + 0.5f, 0, 255);
        }
}

static void controller_page(const uint8_t *lin, int w, int h, uint32_t row) {
    static const char *const out[2] = {"build/hdtex/87a23a48fce614ab.dds", "build/hdtex/87a23a48fce614ab_xbox.dds"};
    static const char *const art[2] = {"d3_art/ctrl_ps.rgba", "d3_art/ctrl_xbox.rgba"};
    static bool done;
    if (done || w != 2048 || h != 2048) return;
    done = true;
    if (exists(out[0]) && exists(out[1])) return;
    Img page = bc3_decode(lin, w, h, row);
    if (!page.px) return;
    clean_page(&page);
    mkdirs();
    bool any = false;
    for (int v = 0; v < 2; v++) {
        Img a = {0, 0, NULL};
        if (exists(out[v]) || !load_overlay(art[v], &a)) {
            if (!a.px && !exists(out[v])) LOG("[d3art] %s missing: no %s picture\n", art[v], v ? "Xbox" : "PlayStation");
            continue;
        }
        Img im = {w, h, malloc((size_t)w * h * 4)};
        if (im.px) {
            memcpy(im.px, page.px, (size_t)w * h * 4);
            composite(&im, &a);
            any |= write_dds(out[v], &im);
            free(im.px);
        }
        free(a.px);
    }
    free(page.px);
    if (any) d3_hd_reindex();
}

void d3_art_capture(uint64_t hash, const uint8_t *lin, uint32_t w, uint32_t h, uint32_t row, uint32_t fmt) {
    if (fmt != 0x44 || !game_is_d3() || getenv("D3_NO_ART_CAPTURE")) return;
    switch (hash) {
    case H_STREAK: menu_piece(0, "streak", lin, (int)w, (int)h, row); break;
    case H_SMOKE: menu_piece(1, "smoke", lin, (int)w, (int)h, row); break;
    case H_FLARE: menu_piece(2, "flare", lin, (int)w, (int)h, row); break;
    case H_VEINS: menu_piece(3, "", lin, (int)w, (int)h, row); break;
    case H_CTRL: controller_page(lin, (int)w, (int)h, row); break;
    default: break;
    }
}
