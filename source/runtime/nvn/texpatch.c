// Button prompts in the controller's own style, and the Hero Mode HUD icon hidden: edits made to the game's own UI
// textures as they are uploaded (ideas from the GameBanana mods "XBOX UI", "PS4 UI Buttons" and "No Hero Mode
// Indicator", credited in README.md; none of their files are used and no game art ships with the recomp).
//
// The prompt glyphs are 48x48 layout textures (BtnA_00, BtnL_01, ... in the UI archives' __Combined.bntx: BC5
// masks, BC3 / ASTC colour versions) and the pictogram font's sheet (pictogram.bffnt: RGBA8, the inline icons of
// message text). They are recognised by content: the user's own UI archives are read once and each texture the game
// uploads is compared with the known ones (same size, same bytes). A known glyph keeps the game's button shape and
// colours; only its label is redrawn: the old label (the pixels inside the shape that differ from the body colour)
// is painted over with the body colour and the new label (Xbox letters, PlayStation symbols, drawn here with a
// system font / simple geometry) composited in its place. Changed 4x4 blocks are re-encoded to the original format.
//
// Labels follow the controller bindings (runtime/hle/bindings.c; by position unless remapped): each Switch button's
// glyph names the controller button now driving it. Defaults: Switch A (east) = Xbox B / circle, B (south) = Xbox A /
// cross, X (north) = Y / triangle, Y (west) = X / square; L / R = LB / RB, L1 / R1; ZL / ZR = LT / RT, L2 / R2;
// Plus / Minus = Menu / View, Options / Create. A button remapped on a Nintendo-style pad gets Nintendo letters; one
// moved to a stick click / the d-pad gets LS / RS (L3 / R3) or an arrow. Stick glyphs stay. (The keyboard has no
// glyphs of its own: keyboard bindings don't change the prompts.)
#include "texpatch.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../rando.h"
#include "../settings.h"
#include "astc.h"
#include "recolour.h"
#include "skin_face.h"
#include "tegra.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"

enum { K_BC5, K_BC3, K_ASTC4, K_RGBA8, K_BC1 };
enum { R_A = 1, R_B, R_X, R_Y, R_L, R_R, R_ZL, R_ZR, R_PLUS, R_MINUS, R_HERO, R_SHEET, R_BOTTLE, R_BOTTLE_MODEL, R_FAIRY_MODEL,
       R_ITEM_CURSOR, R_TUNIC, R_TUNIC_RED, R_TUNIC_BLUE, R_HEART, R_POWDER_MODEL, R_DGN_MAP, R_MAP_MODEL, R_POWER_PIECE_MODEL,
       R_SKIN };
enum { STYLE_NINTENDO, STYLE_XBOX, STYLE_PS };
// labels: R_A..R_MINUS = the button that role is on by default; or these
enum { L_LS = 32, L_RS, L_DUP, L_DDOWN, L_DLEFT, L_DRIGHT, L_OTHER };

typedef struct {
    uint8_t role, kind, srgb, bhl;  // bhl: log2 of the block height (GOBs) it is stored with
    uint16_t w, h, levels;
    uint32_t size;
    uint8_t *data;  // the texture's (block-linear) bytes as stored in the game files
    char name[32];
} Ref;

static Ref g_refs[96];
static int g_nrefs;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// ------------------------------------------------------------------ game files (SARC / BNTX / BFFNT)
static uint8_t *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len ? *len : 1);
    if (fread(d, 1, *len, f) != *len) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static uint8_t *read_all(const char *rel, size_t *len) {  // (the game's own files, never a seed's)
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs", rel);
    return read_file(p, len);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool g_scanning_link;  // (Playerlink.bfres: its face maps are Link's skin)

static int role_of(const char *name) {
    // cosmetics: Link's skin (Cosmetics tab "Skin color"): his face's maps (eyes / mouth expressions, ears; the body
    // map's arms and hands are recoloured with the tunic map, tunic_apply)
    if (g_scanning_link && (!strncmp(name, "MI_eye_alb", 10) || !strncmp(name, "MI_mouth_alb", 12) ||
                            !strncmp(name, "MI_ear_alb", 10)))
        return R_SKIN;
    static const struct { const char *prefix; int role; } m[] = {
        {"BtnA_", R_A}, {"BtnB_", R_B}, {"BtnX_", R_X}, {"BtnY_", R_Y}, {"BtnL_", R_L}, {"BtnR_", R_R},
        {"BtnZL_", R_ZL}, {"BtnZR_", R_ZR}, {"BtnPlus_", R_PLUS}, {"BtnMinus_", R_MINUS},
        {"Spicy_00^t", R_HERO},  // Ld_Spicy (StartUp.arc): the HUD's Hero Mode icon (the file select uses Spicy_00^f)
        // the empty bottle's item icon and its 3D model's colour map (ItemBottle.bfres): identified only, never edited
        // (the gold Escape Bottle's draws are tinted: runtime/hle/escape.c)
        {"ItemBottle_00^w", R_BOTTLE}, {"MI_bottle_alb", R_BOTTLE_MODEL},
        // the fairy's colour map (ItemFairy.bfres): boss / mini-boss souls are a fairy drawn evil red (runtime/hle/souls.c)
        {"MI_fairy_alb", R_FAIRY_MODEL},
        // cosmetics: Link's tunics (recoloured on upload: green, and with a colour chosen the red / blue too, so the
        // Color Dungeon's tunics keep it - user 2026-10-11; the red map is also the recolour's mask)
        // and the hearts of Cm_HeartGauge (HUD, file select): Hp_00 = an empty heart (its own draw); each quarter of
        // a full one is one draw of Hp_01 (fill) + Hp_02 (frame, coloured by the layout's material) through the
        // Hp_04 mask; HeartUp = the life-up flash. Their draws are tinted.
        {"MI_body_alb.0", R_TUNIC}, {"MI_body_alb.1", R_TUNIC_RED}, {"MI_body_alb.2", R_TUNIC_BLUE},
        // the Magic Powder sack's colour map (ItemMagicPowder.bfres; after the tunic's: same name prefix): held up as
        // the Bomb Bag, tinted bomb blue then (runtime/hle/escape.c bomb_bag_tint)
        {"MI_body_alb", R_POWDER_MODEL},
        // the Hint Sheet (runtime/hle/hint_sheet.c): the dungeon map's UI icon (drawn in place of the first bottle's,
        // texpatch_hint_icon) and its 3D model's colour map (ItemDungeonMap.bfres: Tarin's hand-over, tinted)
        {"DgnMap_00^q", R_DGN_MAP}, {"MI_dungeonMap_alb", R_MAP_MODEL},
        // the Piece of Power's colour map (ItemPowerPiece.bfres): Triforce pieces are one drawn gold (runtime/hle/triforce.c)
        {"MI_powerPiece_alb", R_POWER_PIECE_MODEL},
        {"Hp_00^w", R_HEART}, {"Hp_01^w", R_HEART}, {"Hp_02^w", R_HEART}, {"HeartUp^w", R_HEART},
        // the Items page's cursor brackets: identified only (hidden while the companion card has the cursor, pets.c)
        {"ItemCursor_", R_ITEM_CURSOR},
    };
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++)
        if (!strncmp(name, m[i].prefix, strlen(m[i].prefix))) return m[i].role;
    return 0;
}

static void add_ref(int role, int kind, int srgb, uint32_t w, uint32_t h, uint32_t levels, const uint8_t *data, uint32_t size,
                    const char *name, uint32_t bhl) {
    for (int i = 0; i < g_nrefs; i++)  // (the archives share copies of the same textures)
        if (g_refs[i].w == w && g_refs[i].h == h && g_refs[i].size == size && !memcmp(g_refs[i].data, data, size)) return;
    if (g_nrefs == (int)(sizeof g_refs / sizeof *g_refs)) return;
    Ref *r = &g_refs[g_nrefs++];
    *r = (Ref){(uint8_t)role, (uint8_t)kind, (uint8_t)srgb, (uint8_t)bhl, (uint16_t)w, (uint16_t)h, (uint16_t)levels, size,
               malloc(size), ""};
    memcpy(r->data, data, size);
    strlcpy(r->name, name, sizeof r->name);
}

// Textures of a BNTX (`b` .. `b + len`): the glyph ones (or, sheet = true, its only one: a font sheet).
static void scan_bntx(const uint8_t *b, size_t len, bool sheet) {
    if (len < 0x40 || memcmp(b, "BNTX", 4) || memcmp(b + 0x20, "NX  ", 4)) return;
    uint32_t count = rd32(b + 0x24);
    uint64_t infos = rd64(b + 0x28);
    for (uint32_t i = 0; i < count && infos + 8 * i + 8 <= len; i++) {
        uint64_t brti = rd64(b + infos + 8 * i);
        if (brti + 0x90 > len || memcmp(b + brti, "BRTI", 4)) continue;
        const uint8_t *t = b + brti + 16;
        uint64_t name_at = rd64(t + 80), ptrs = rd64(t + 96);
        char name[64] = "";
        if (name_at + 2 < len) {
            uint16_t n = rd16(b + name_at);
            if (n < sizeof name && name_at + 2 + n <= len) { memcpy(name, b + name_at + 2, n); name[n] = 0; }
        }
        int role = sheet ? R_SHEET : role_of(name);
        if (!role || ptrs + 8 > len) continue;
        uint32_t fmt = rd32(t + 12), w = rd32(t + 20), h = rd32(t + 24), size = rd32(t + 64), levels = rd16(t + 6);
        uint64_t data = rd64(b + ptrs);
        if (data + size > len) continue;
        int kind = (fmt >> 8) == 0x1e ? K_BC5 : (fmt >> 8) == 0x1c ? K_BC3 : (fmt >> 8) == 0x2d ? K_ASTC4 :
                   (fmt >> 8) == 0x0b ? K_RGBA8 : (fmt >> 8) == 0x1a ? K_BC1 : -1;
        if (kind < 0 || (sheet && kind != K_RGBA8)) continue;
        add_ref(role, kind, (fmt & 0xff) == 6, w, h, levels, b + data, size, name, rd32(t + 36) & 7);
    }
}

static void scan_sarc(const char *rel) {
    size_t len;
    uint8_t *d = read_all(rel, &len);
    if (!d) return;
    if (len > 0x20 && !memcmp(d, "SARC", 4) && !memcmp(d + 0x14, "SFAT", 4)) {
        uint32_t data_off = rd32(d + 0xC);
        uint16_t n = rd16(d + 0x1A);
        const uint8_t *ent = d + 0x20, *names = d + 0x20 + 16 * n + 8;  // (SFNT header: 8 bytes)
        for (uint16_t i = 0; i < n && ent + 16 * (i + 1) <= d + len; i++) {
            uint32_t attr = rd32(ent + 16 * i + 4), s = rd32(ent + 16 * i + 8), e = rd32(ent + 16 * i + 12);
            const char *nm = (attr >> 24) ? (const char *)names + (attr & 0xffffff) * 4 : "";
            if ((const uint8_t *)nm >= d + len || strcmp(nm, "timg/__Combined.bntx") || data_off + e > len) continue;
            scan_bntx(d + data_off + s, e - s, false);
        }
    }
    free(d);
}

static void load_refs(void) {
    scan_sarc("region_common/ui/Game.arc");
    scan_sarc("region_common/ui/StartUp.arc");
    scan_sarc("regionUS/common/ui/GameRegionCommon.arc");
    scan_sarc("regionEU/common/ui/GameRegionCommon.arc");
    size_t len;
    uint8_t *f;
    static const char *const models[] = {"region_common/actor/ItemBottle.bfres", "region_common/actor/Playerlink.bfres",
                                         "region_common/actor/ItemFairy.bfres", "region_common/actor/ItemMagicPowder.bfres",
                                         "region_common/actor/ItemDungeonMap.bfres",
                                         "region_common/actor/ItemPowerPiece.bfres"};
    for (int m = 0; m < (int)(sizeof models / sizeof *models); m++)
        if ((f = read_all(models[m], &len))) {  // (their textures: one BNTX inside)
            g_scanning_link = m == 1;
            for (size_t i = 0; i + 8 < len; i += 4)
                if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, false); break; }
            g_scanning_link = false;
            free(f);
        }
    f = read_all("region_common/ui/pictogram.bffnt", &len);  // message icons: one RGBA8 sheet in a BNTX
    if (f) {
        for (size_t i = 0; i + 8 < len; i += 4)
            if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, true); break; }
        free(f);
    }
    LOG("[texpatch] %d known textures (button glyphs, pictogram sheet, Hero Mode icon, bottle icon / model, tunic, hearts)\n",
        g_nrefs);
}

int texpatch_bottle_kind(int id) {
    if (id < 0 || id >= g_nrefs) return 0;
    return g_refs[id].role == R_BOTTLE ? 1 : g_refs[id].role == R_BOTTLE_MODEL ? 2 : g_refs[id].role == R_FAIRY_MODEL ? 3
         : g_refs[id].role == R_POWDER_MODEL ? 4 : g_refs[id].role == R_MAP_MODEL ? 5 : g_refs[id].role == R_POWER_PIECE_MODEL ? 6 : 0;
}

int texpatch_heart_kind(int id) {
    if (id < 0 || id >= g_nrefs) return 0;
    return g_refs[id].role == R_HEART;
}

static int ref_of(int role) {
    for (int i = 0; i < g_nrefs; i++) if (g_refs[i].role == role) return i;
    return -1;
}

// A seed made with a tunic colour (older seeds: tools/rando/cosmetics.py) ships its own recoloured Playerlink.bfres:
// its tunic map is recognised too, so the Cosmetics tab's colour wins over it. Read again when the seed changes
// (seeds switch in place).
static uint8_t *g_seed_tunic;
static uint32_t g_seed_tunic_size;
static char g_seed_tunic_dir[300] = "\1";
static const uint8_t *seed_tunic(uint32_t *size) {
    const char *dir = rando_seed_dir();
    if (strcmp(dir ? dir : "", g_seed_tunic_dir)) {
        strlcpy(g_seed_tunic_dir, dir ? dir : "", sizeof g_seed_tunic_dir);
        free(g_seed_tunic);
        g_seed_tunic = NULL;
        char p[600];
        size_t len;
        uint8_t *f = NULL;
        if (dir) snprintf(p, sizeof p, "%s/romfs/region_common/actor/Playerlink.bfres", dir), f = read_file(p, &len);
        int n0 = g_nrefs;
        for (size_t i = 0; f && i + 8 < len; i += 4)  // (a copy only differing from the game's adds a ref: taken out)
            if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, false); break; }
        free(f);
        for (int i = n0; i < g_nrefs; i++) {
            if (g_refs[i].role == R_TUNIC && !g_seed_tunic) { g_seed_tunic = g_refs[i].data; g_seed_tunic_size = g_refs[i].size; }
            else free(g_refs[i].data);
        }
        g_nrefs = n0;
        if (g_seed_tunic) LOG("[texpatch] seed tunic map (%s)\n", dir);
    }
    *size = g_seed_tunic_size;
    return g_seed_tunic;
}

static TexLayout g_tunic_layout;  // (the game's tunic texture: its levels' places in the map's data)
static bool g_tunic_layout_ok;
bool texpatch_is_item_cursor(int id) { return id >= 0 && id < g_nrefs && g_refs[id].role == R_ITEM_CURSOR; }

int texpatch_identify(const NvnTexture *t) {
    pthread_once(&g_once, load_refs);
    if (!t->addr || t->depth > 1) return -1;
    uint64_t total = tegra_total_size(&t->layout);
    for (int i = 0; i < g_nrefs; i++) {
        const Ref *r = &g_refs[i];
        // (only the bottle model's and the tunic's maps have mip levels: compared in full, all levels)
        bool tunic = r->role == R_TUNIC || r->role == R_TUNIC_RED || r->role == R_TUNIC_BLUE;
        if (r->w != t->width || r->h != t->height || (r->role != R_POWDER_MODEL && r->role != R_MAP_MODEL &&
            r->role != R_POWER_PIECE_MODEL &&
            (t->levels > 1) != (r->role == R_BOTTLE_MODEL || r->role == R_FAIRY_MODEL || tunic || r->role == R_SKIN))) continue;
        uint64_t n = r->size < total ? r->size : total;
        if (!memcmp((const void *)(uintptr_t)t->addr, r->data, n)) {
            static int nlog;
            if (nlog++ < 64) LOG("[texpatch] %s (%ux%u fmt %#x) uploaded\n", r->name, r->w, r->h, t->format);
            if (tunic) { g_tunic_layout = t->layout; g_tunic_layout_ok = true; }  // (all three: the same layout)
            return i;
        }
    }
    int tu = ref_of(R_TUNIC);
    if (tu >= 0 && t->levels > 1 && t->width == g_refs[tu].w && t->height == g_refs[tu].h) {
        uint32_t n;
        const uint8_t *s = seed_tunic(&n);
        if (s && n == g_refs[tu].size && n <= total && !memcmp((const void *)(uintptr_t)t->addr, s, n)) {
            LOG("[texpatch] seed's tunic map uploaded\n");
            g_tunic_layout = t->layout;
            g_tunic_layout_ok = true;
            return tu;
        }
    }
    int rc = recolour_identify(t, total);  // (Cosmetics recolours: identified only, runtime/nvn/recolour.c)
    return rc >= 0 ? TEXPATCH_RECOLOUR_BASE + rc : -1;
}

static int prompt_style(void) {
    // LARECOMP_PROMPTS=nintendo|xbox|playstation: test override, until frame LARECOMP_PROMPTS_UNTIL (then the setting)
    static int env = -2;
    static long until;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PROMPTS");
        env = !e ? -1 : !strcmp(e, "xbox") ? PROMPTS_XBOX : !strcmp(e, "playstation") ? PROMPTS_PLAYSTATION :
              !strcmp(e, "nintendo") ? PROMPTS_NINTENDO : PROMPTS_AUTO;
        until = getenv("LARECOMP_PROMPTS_UNTIL") ? atol(getenv("LARECOMP_PROMPTS_UNTIL")) : -1;
    }
    extern uint64_t renderer_present_count(void);
    int p = env >= 0 && (until < 0 || (long)renderer_present_count() < until) ? env : g_settings.prompts;
    if (p == PROMPTS_XBOX) return STYLE_XBOX;
    if (p == PROMPTS_PLAYSTATION) return STYLE_PS;
    if (p == PROMPTS_NINTENDO) return STYLE_NINTENDO;
    extern int input_pad_family(void);  // runtime/hle/input.c: the controller SDL reports
    int f = input_pad_family();
    return f == 1 ? STYLE_XBOX : f == 2 ? STYLE_PS : STYLE_NINTENDO;
}

static bool hide_hero(void) {
    static int env = -2;  // LARECOMP_HIDE_HERO=0/1: test override
    if (env == -2) { const char *e = getenv("LARECOMP_HIDE_HERO"); env = e ? atoi(e) != 0 : -1; }
    return env >= 0 ? env : g_settings.hide_hero_icon;
}

// The label a glyph of `role` gets: whichever button is bound to that Switch input now
static int prompt_label(int role) {
    static const int8_t slot[] = {[R_A] = BIND_A, [R_B] = BIND_B, [R_X] = BIND_X, [R_Y] = BIND_Y, [R_L] = BIND_L,
                                  [R_R] = BIND_R, [R_ZL] = BIND_ZL, [R_ZR] = BIND_ZR, [R_PLUS] = BIND_PLUS,
                                  [R_MINUS] = BIND_MINUS};
    if (role < R_A || role > R_MINUS) return role;
    switch (bind_prompt_host(slot[role])) {
    case 1 /* east */: return R_A;
    case 0 /* south */: return R_B;
    case 3 /* north */: return R_X;
    case 2 /* west */: return R_Y;
    case 9 /* left shoulder */: return R_L;
    case 10 /* right shoulder */: return R_R;
    case PADB_LTRIGGER: return R_ZL;
    case PADB_RTRIGGER: return R_ZR;
    case 6 /* start */: return R_PLUS;
    case 4 /* back */: return R_MINUS;
    case 7: return L_LS;
    case 8: return L_RS;
    case 11: return L_DUP;
    case 12: return L_DDOWN;
    case 13: return L_DLEFT;
    case 14: return L_DRIGHT;
    default: return L_OTHER;
    }
}

static uint32_t labels_hash(void) {
    uint32_t h = 0;
    for (int r = R_A; r <= R_MINUS; r++) h = h * 41 + (uint32_t)(prompt_label(r) - r + 64);
    return h;
}

// (a Nintendo-style glyph is only redrawn when its button was remapped)
static bool restyled(int role, int style) { return style != STYLE_NINTENDO || prompt_label(role) != role; }

static void cosmetics_test(void);
static uint32_t skin_rgb(void);
static uint32_t tunic_rgb(void);

uint32_t texpatch_generation(void) {  // (a rebind re-uploads the glyphs, like a style change; so does a tunic colour)
    cosmetics_test();
    return (1 + (uint32_t)prompt_style() + (hide_hero() ? 4 : 0) + (labels_hash() << 3)) ^ tunic_rgb() * 0x9E3779B1u ^
           skin_rgb() * 0x85EBCA6Bu;
}

// ------------------------------------------------------------------ block codecs (BC4/BC5, BC1/BC3, ASTC 4x4)
static void bc4_decode(const uint8_t *b, uint8_t out[16]) {
    int e0 = b[0], e1 = b[1], pal[8] = {e0, e1};
    if (e0 > e1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * e0 + i * e1 + 3) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * e0 + i * e1 + 2) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}

static void bc4_encode(const uint8_t v[16], uint8_t *b) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    uint64_t bits = 0;
    if (hi == lo) { b[0] = b[1] = (uint8_t)hi; }
    else {
        int pal[8] = {hi, lo};
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
        b[0] = (uint8_t)hi; b[1] = (uint8_t)lo;
        for (int i = 0; i < 16; i++) {
            int best = 0, be = 1 << 30;
            for (int k = 0; k < 8; k++) { int e = abs(pal[k] - v[i]); if (e < be) { be = e; best = k; } }
            bits |= (uint64_t)best << (3 * i);
        }
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
}

static void rgb565(uint16_t c, int o[3]) {
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = b << 3 | b >> 2;
}

static void bc1_decode4(const uint8_t *b, uint8_t out[16][4]) {  // (BC2/BC3 colour: always the 4-colour mode)
    int c[4][3];
    rgb565(rd16(b), c[0]); rgb565(rd16(b + 2), c[1]);
    for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
    uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) out[i][k] = (uint8_t)c[(idx >> (2 * i)) & 3][k];
}

static uint16_t to565(const uint8_t *p) { return (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3); }

static void bc1_encode4(const uint8_t px[16][4], uint8_t *b) {
    int bi = 0, bj = 0, bd = -1;  // the two most distant colours of the block as endpoints
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++) {
            int d = 0;
            for (int k = 0; k < 3; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    uint16_t c0 = to565(px[bi]), c1 = to565(px[bj]);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    int c[4][3];
    rgb565(c0, c[0]); rgb565(c1, c[1]);
    for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int s = 0; s < 4; s++) {
            int e = 0;
            for (int k = 0; k < 3; k++) e += (c[s][k] - px[i][k]) * (c[s][k] - px[i][k]);
            if (e < be) { be = e; best = s; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    b[0] = (uint8_t)c0; b[1] = (uint8_t)(c0 >> 8); b[2] = (uint8_t)c1; b[3] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) b[4 + i] = (uint8_t)(idx >> (8 * i));
}

// ASTC 4x4: one partition, colour endpoint mode 12 (LDR RGBA direct) with 8-bit endpoints, a 4x4 grid of 2-bit
// weights (block mode 0x42: 11 + 2 + 4 + 64 endpoint + 32 weight bits). Only blocks we change are re-encoded.
static void put_bits(uint8_t *b, int at, int n, uint32_t v) {
    for (int i = 0; i < n; i++) if (v >> i & 1) b[(at + i) >> 3] |= (uint8_t)(1 << ((at + i) & 7));
}

static void astc_encode4(const uint8_t px[16][4], bool srgb, uint8_t *b) {
    int bi = 0, bj = 0, bd = -1;
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++) {
            int d = 0;
            for (int k = 0; k < 4; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    const uint8_t *e0 = px[bi], *e1 = px[bj];
    if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2]) { const uint8_t *t = e0; e0 = e1; e1 = t; }  // (no blue contraction)
    memset(b, 0, 16);
    put_bits(b, 0, 11, 0x42);
    put_bits(b, 13, 4, 12);
    for (int k = 0; k < 4; k++) { put_bits(b, 17 + 16 * k, 8, e0[k]); put_bits(b, 25 + 16 * k, 8, e1[k]); }
    static const int wq[4] = {0, 21, 43, 64};
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int q = 0; q < 4; q++) {
            int e = 0;
            for (int k = 0; k < 4; k++) {
                int c0 = srgb && k < 3 ? (e0[k] << 8 | 0x80) : e0[k] * 257, c1 = srgb && k < 3 ? (e1[k] << 8 | 0x80) : e1[k] * 257;
                int v = ((c0 * (64 - wq[q]) + c1 * wq[q] + 32) >> 6) >> 8;
                e += (v - px[i][k]) * (v - px[i][k]);
            }
            if (e < be) { be = e; best = q; }
        }
        if (best & 1) b[(127 - 2 * i) >> 3] |= (uint8_t)(1 << ((127 - 2 * i) & 7));  // weights: bit-reversed from the top
        if (best & 2) b[(126 - 2 * i) >> 3] |= (uint8_t)(1 << ((126 - 2 * i) & 7));
    }
}

// Level data <-> RGBA8 image (BC5: red, green -> R, G; alpha = green, the glyph's shape)
static void decode_image(const Ref *r, const uint8_t *src, uint32_t row, uint8_t *img) {
    uint32_t W = r->w, H = r->h, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (r->kind == K_RGBA8) { for (uint32_t y = 0; y < H; y++) memcpy(img + y * W * 4, src + y * row, W * 4); return; }
    if (r->kind == K_ASTC4) {
        uint8_t *blocks = malloc((size_t)bw * bh * 16);
        for (uint32_t y = 0; y < bh; y++) memcpy(blocks + y * bw * 16, src + y * row, bw * 16);
        astc_decode_ldr(blocks, W, H, 4, 4, r->srgb, img);
        free(blocks);
        return;
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + by * row + bx * 16;
            uint8_t px[16][4] = {{0}}, a[16], c2[16];
            if (r->kind == K_BC5) {
                bc4_decode(b, a); bc4_decode(b + 8, c2);
                for (int i = 0; i < 16; i++) { px[i][0] = a[i]; px[i][1] = c2[i]; px[i][2] = 0; px[i][3] = c2[i]; }
            } else {
                bc4_decode(b, a); bc1_decode4(b + 8, px);
                for (int i = 0; i < 16; i++) px[i][3] = a[i];
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < W && y < H) memcpy(img + (y * W + x) * 4, px[i], 4);
            }
        }
}

static void encode_image(const Ref *r, const uint8_t *img, const uint8_t *dirty, uint8_t *dst, uint32_t row) {
    uint32_t W = r->w, H = r->h, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (r->kind == K_RGBA8) { for (uint32_t y = 0; y < H; y++) memcpy(dst + y * row, img + y * W * 4, W * 4); return; }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            if (!dirty[by * bw + bx]) continue;
            uint8_t px[16][4], *b = dst + by * row + bx * 16;
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= W) x = W - 1;
                if (y >= H) y = H - 1;
                memcpy(px[i], img + (y * W + x) * 4, 4);
            }
            if (r->kind == K_BC5) {
                uint8_t c0[16], c1[16];
                for (int i = 0; i < 16; i++) { c0[i] = px[i][0]; c1[i] = px[i][1]; }
                bc4_encode(c0, b); bc4_encode(c1, b + 8);
            } else if (r->kind == K_BC3) {
                uint8_t a[16];
                for (int i = 0; i < 16; i++) a[i] = px[i][3];
                bc4_encode(a, b); bc1_encode4(px, b + 8);
            } else {
                astc_encode4(px, r->srgb, b);
            }
        }
}

// ------------------------------------------------------------------ labels
static stbtt_fontinfo g_font;
static bool g_font_ok;
static uint8_t *g_font_data;

static void try_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_font_data = malloc((size_t)n);
    if (fread(g_font_data, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0)))
        g_font_ok = true;
    else { free(g_font_data); g_font_data = NULL; }
    fclose(f);
}

static void load_font(void) {  // a bold system font for the letters (none of the game's fonts are needed)
    static const char *const paths[] = {
        "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/Library/Fonts/Arial Bold.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
    };
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && !g_font_ok; i++) try_font(paths[i]);
    char own[1100];  // none of the system's: the bundled DejaVu Sans Bold (third_party/fonts/DejaVu, in every release)
    if (!g_font_ok && rt_resource("DejaVuSans-Bold.ttf", "third_party/fonts/DejaVu/DejaVuSans-Bold.ttf", own, sizeof own)) try_font(own);
    if (!g_font_ok) LOG("[texpatch] no system font for the button letters: Xbox letters stay Nintendo's\n");
}

#define CV 256  // label canvas (drawn large, then fitted into the glyph)

static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = sat((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f));
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

typedef struct { float ax, ay, bx, by; } Seg;

static void draw_segs(float *cv, const Seg *s, int n, float r) {  // round-capped strokes
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float d = 1e9f;
            for (int i = 0; i < n; i++) { float e = seg_dist(x + 0.5f, y + 0.5f, s[i].ax, s[i].ay, s[i].bx, s[i].by); if (e < d) d = e; }
            float a = sat(r - d + 0.5f);
            if (a > cv[y * CV + x]) cv[y * CV + x] = a;
        }
}

static void draw_ring(float *cv, float cx, float cy, float rad, float th) {
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
            float a = sat(th / 2 - fabsf(d - rad) + 0.5f);
            if (a > cv[y * CV + x]) cv[y * CV + x] = a;
        }
}

static void draw_rect(float *cv, float x0, float y0, float x1, float y1, float th) {
    Seg s[4] = {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}};
    draw_segs(cv, s, 4, th / 2);
}

static bool draw_text(float *cv, const char *text) {
    if (!g_font_ok) return false;
    float sc = stbtt_ScaleForPixelHeight(&g_font, 200);
    float x = 8;
    for (const char *p = text; *p; p++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, *p, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, *p, sc, sc, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0, ox = (int)x + x0, oy = 200 + y0;
        if (w > 0 && h > 0) {
            unsigned char *bm = malloc((size_t)w * h);
            stbtt_MakeCodepointBitmap(&g_font, bm, w, h, w, sc, sc, *p);
            for (int yy = 0; yy < h; yy++)
                for (int xx = 0; xx < w; xx++) {
                    int X = ox + xx, Y = oy + yy;
                    if (X >= 0 && X < CV && Y >= 0 && Y < CV) { float a = bm[yy * w + xx] / 255.0f; if (a > cv[Y * CV + X]) cv[Y * CV + X] = a; }
                }
            free(bm);
        }
        x += adv * sc;
        if (p[1]) x += stbtt_GetCodepointKernAdvance(&g_font, p[0], p[1]) * sc;
    }
    return true;
}

// The new label `role` (R_* / L_*, canvas coverage). symbol = true: a square pictogram (fitted by its size, not cap
// height).
static bool draw_label(int role, int style, float *cv, bool *symbol) {
    memset(cv, 0, sizeof(float) * CV * CV);
    *symbol = role == R_PLUS || role == R_MINUS || (style == STYLE_PS && role <= R_Y) || role >= L_DUP;
    if (role >= L_DUP && role <= L_DRIGHT) {  // d-pad: an arrow (drawn pointing up, then turned)
        static const float up[3][4] = {{128, 228, 128, 40}, {128, 34, 46, 116}, {128, 34, 210, 116}};
        Seg s[3];
        for (int i = 0; i < 3; i++) {
            float p[4];
            for (int k = 0; k < 4; k += 2) {
                float x = up[i][k] - 128, y = up[i][k + 1] - 128;
                float rx = role == L_DUP ? x : role == L_DDOWN ? -x : role == L_DLEFT ? y : -y;
                float ry = role == L_DUP ? y : role == L_DDOWN ? -y : role == L_DLEFT ? -x : x;
                p[k] = rx + 128, p[k + 1] = ry + 128;
            }
            s[i] = (Seg){p[0], p[1], p[2], p[3]};
        }
        draw_segs(cv, s, 3, 16);
        return true;
    }
    if (role == L_OTHER) { draw_ring(cv, 128, 128, 40, 80); return true; }  // a paddle / extra button: a dot
    if (role == L_LS || role == L_RS)
        return draw_text(cv, style == STYLE_PS ? (role == L_LS ? "L3" : "R3") : (role == L_LS ? "LS" : "RS"));
    if (style == STYLE_NINTENDO && role == R_PLUS) {
        Seg s[2] = {{128, 40, 128, 216}, {40, 128, 216, 128}};
        draw_segs(cv, s, 2, 18);
        return true;
    }
    if (style == STYLE_NINTENDO && role == R_MINUS) {
        Seg s[1] = {{40, 128, 216, 128}};
        draw_segs(cv, s, 1, 18);
        return true;
    }
    if (role == R_PLUS) {  // Menu / Options: three bars
        Seg s[3] = {{48, 64, 208, 64}, {48, 128, 208, 128}, {48, 192, 208, 192}};
        draw_segs(cv, s, 3, 15);
        return true;
    }
    if (role == R_MINUS) {
        if (style == STYLE_XBOX) {  // View: two overlapping windows
            draw_rect(cv, 92, 40, 220, 140, 22);
            for (int y = 104; y < 228; y++) for (int x = 24; x < 164; x++) cv[y * CV + x] = 0;  // (front window hides the back)
            draw_rect(cv, 36, 116, 152, 216, 22);
        } else {  // Create / Share: three rays
            Seg s[3] = {{128, 40, 128, 140}, {48, 92, 104, 164}, {208, 92, 152, 164}};
            draw_segs(cv, s, 3, 15);
            Seg d[1] = {{128, 206, 128, 206}};
            draw_segs(cv, d, 1, 20);
        }
        return true;
    }
    if (style == STYLE_PS && role <= R_Y) {
        if (role == R_A) draw_ring(cv, 128, 128, 96, 26);  // east: circle
        else if (role == R_B) { Seg s[2] = {{44, 44, 212, 212}, {212, 44, 44, 212}}; draw_segs(cv, s, 2, 14); }  // south: cross
        else if (role == R_X) { Seg s[3] = {{128, 30, 226, 200}, {226, 200, 30, 200}, {30, 200, 128, 30}}; draw_segs(cv, s, 3, 13); }  // north: triangle
        else draw_rect(cv, 44, 44, 212, 212, 26);  // west: square
        return true;
    }
    static const char *const xbox[] = {"", "B", "A", "Y", "X", "LB", "RB", "LT", "RT"};
    static const char *const ps[] = {"", "", "", "", "", "L1", "R1", "L2", "R2"};
    static const char *const nin[] = {"", "A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    if (role > R_ZR) return false;
    return draw_text(cv, style == STYLE_XBOX ? xbox[role] : style == STYLE_PS ? ps[role] : nin[role]);
}

// Relabel one glyph: img (W x H RGBA8, alpha channel `ach` = the shape), region x0,y0 w,h. flip: the region is
// stored upside down (font sheets).
static void relabel(uint8_t *img, int W, int x0, int y0, int w, int h, int ach, bool flip, int role, int style,
                    uint8_t *dirty, int bw) {
    int n = w * h;
    uint8_t *px = malloc((size_t)n * 4), *S = calloc((size_t)n, 1), *I = calloc((size_t)n, 1), *L = calloc((size_t)n, 1);
    int *stack = malloc(sizeof(int) * (size_t)n * 4);
    float *cv = malloc(sizeof(float) * CV * CV);
    for (int y = 0; y < h; y++) {
        int sy = flip ? h - 1 - y : y;
        memcpy(px + y * w * 4, img + ((y0 + sy) * W + x0) * 4, (size_t)w * 4);
    }
    // shape: alpha > 0.5 with its holes filled (the region outside it is what the border reaches)
    uint8_t *out = calloc((size_t)n, 1);
    int sp = 0;
    for (int i = 0; i < n; i++) {
        int x = i % w, y = i / w;
        if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && px[i * 4 + ach] <= 127) { out[i] = 1; stack[sp++] = i; }
    }
    while (sp) {
        int i = stack[--sp], x = i % w, y = i / w;
        const int nb[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
        for (int k = 0; k < 4; k++)
            if (nb[k] >= 0 && !out[nb[k]] && px[nb[k] * 4 + ach] <= 127) { out[nb[k]] = 1; stack[sp++] = nb[k]; }
    }
    int sx0 = w, sy0 = h, sx1 = -1, sy1 = -1, area = 0;
    for (int i = 0; i < n; i++)
        if (!out[i]) {
            S[i] = 1, area++;
            int x = i % w, y = i / w;
            if (x < sx0) sx0 = x;
            if (x > sx1) sx1 = x;
            if (y < sy0) sy0 = y;
            if (y > sy1) sy1 = y;
        }
    if (area < 30) goto done;
    for (int i = 0; i < n; i++) {  // interior: 2 pixels in from the shape's edge
        int x = i % w, y = i / w, ok = S[i];
        for (int dy = -2; dy <= 2 && ok; dy++)
            for (int dx = -2; dx <= 2 && ok; dx++) {
                int xx = x + dx, yy = y + dy;
                ok = xx >= 0 && yy >= 0 && xx < w && yy < h && S[yy * w + xx];
            }
        I[i] = (uint8_t)ok;
    }
    // body colour = the most common colour inside, label colour = the most common one that clearly differs
    {
        static uint16_t hist[1 << 16];
        int best = -1, bc = 0;
        memset(hist, 0, sizeof hist);
        for (int i = 0; i < n; i++)
            if (I[i]) {
                const uint8_t *p = px + i * 4;
                int q = (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4;
                if (++hist[q] > bc) { bc = hist[q]; best = q; }
            }
        if (best < 0) goto done;
        float body[4] = {0}, lab[4] = {0};
        int nb = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            if (I[i] && ((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4) == best) {
                for (int k = 0; k < 4; k++) body[k] += p[k];
                nb++;
            }
        }
        for (int k = 0; k < 4; k++) body[k] /= nb;
        memset(hist, 0, sizeof hist);
        best = -1, bc = 0;
        int lx0 = w, ly0 = h, lx1 = -1, ly1 = -1;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            float d = 0;
            for (int k = 0; k < 4; k++) d += fabsf(p[k] - body[k]);
            if (!I[i] || d < 64) continue;
            L[i] = 1;
            int x = i % w, y = i / w;
            if (x < lx0) lx0 = x;
            if (x > lx1) lx1 = x;
            if (y < ly0) ly0 = y;
            if (y > ly1) ly1 = y;
            int q = (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4;
            if (++hist[q] > bc) { bc = hist[q]; best = q; }
        }
        if (best < 0) goto done;
        int nl = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            if (L[i] && ((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4) == best) {
                for (int k = 0; k < 4; k++) lab[k] += p[k];
                nl++;
            }
        }
        for (int k = 0; k < 4; k++) lab[k] /= nl;
        bool symbol;
        if (!draw_label(prompt_label(role), style, cv, &symbol)) goto done;
        // paint the old label (and its anti-aliased fringe) over with the body colour
        for (int i = 0; i < n; i++) {
            int x = i % w, y = i / w, near = 0, in = S[i];
            for (int dy = -2; dy <= 2 && !near; dy++)
                for (int dx = -2; dx <= 2 && !near; dx++) {
                    int xx = x + dx, yy = y + dy;
                    near = xx >= 0 && yy >= 0 && xx < w && yy < h && L[yy * w + xx];
                }
            for (int dy = -1; dy <= 1 && in; dy++)
                for (int dx = -1; dx <= 1 && in; dx++) {
                    int xx = x + dx, yy = y + dy;
                    in = xx >= 0 && yy >= 0 && xx < w && yy < h && S[yy * w + xx];
                }
            if (near && in) for (int k = 0; k < 4; k++) px[i * 4 + k] = (uint8_t)lroundf(body[k]);
        }
        // fit the new label: text at the old label's height (narrower than the shape), symbols by the old label's size
        int cx0 = CV, cy0 = CV, cx1 = -1, cy1 = -1;
        for (int i = 0; i < CV * CV; i++)
            if (cv[i] > 0.02f) {
                int x = i % CV, y = i / CV;
                if (x < cx0) cx0 = x;
                if (x > cx1) cx1 = x;
                if (y < cy0) cy0 = y;
                if (y > cy1) cy1 = y;
            }
        if (cx1 < 0) goto done;
        float lw = (float)(lx1 - lx0 + 1), lh = (float)(ly1 - ly0 + 1), iw = (float)(sx1 - sx0 + 1), ih = (float)(sy1 - sy0 + 1);
        float cw = (float)(cx1 - cx0 + 1), ch = (float)(cy1 - cy0 + 1);
        float tcx = (lx0 + lx1 + 1) / 2.0f, tcy = (ly0 + ly1 + 1) / 2.0f, th;
        if (symbol) {
            float s = fmaxf(lw, lh);
            if (role == R_PLUS || role == R_MINUS) s = lw;  // ("-" has almost no height)
            s = fminf(s, fminf(iw, ih) * 0.62f);
            th = s * ch / fmaxf(cw, ch);
            tcx = (sx0 + sx1 + 1) / 2.0f, tcy = (sy0 + sy1 + 1) / 2.0f;
        } else {
            th = role == R_PLUS || role == R_MINUS ? fminf(lw, ih * 0.6f) : lh;  // (remapped Plus / Minus: by "+"'s width)
            if (th * cw / ch > iw * 0.78f) th = iw * 0.78f * ch / cw;
        }
        float tw = th * cw / ch, k = ch / th;  // canvas pixels per glyph pixel
        float bx0 = tcx - tw / 2, by0 = tcy - th / 2;
        for (int y = (int)floorf(by0); y <= (int)ceilf(by0 + th); y++)
            for (int x = (int)floorf(bx0); x <= (int)ceilf(bx0 + tw); x++) {
                if (x < 0 || y < 0 || x >= w || y >= h || !S[y * w + x]) continue;
                float a = 0;  // 4x4 samples of the canvas over this pixel
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float u = cx0 + (x + (sx + 0.5f) / 4 - bx0) * k, v = cy0 + (y + (sy + 0.5f) / 4 - by0) * k;
                        int iu = (int)u, iv = (int)v;
                        if (iu >= cx0 && iv >= cy0 && iu <= cx1 && iv <= cy1) a += cv[iv * CV + iu];
                    }
                a /= 16;
                if (a <= 0) continue;
                uint8_t *p = px + (y * w + x) * 4;
                for (int c = 0; c < 4; c++) p[c] = (uint8_t)lroundf(p[c] + (lab[c] - p[c]) * a);
            }
        // write back, marking changed blocks
        for (int y = 0; y < h; y++) {
            int sy = flip ? h - 1 - y : y;
            uint8_t *d = img + ((y0 + sy) * W + x0) * 4;
            for (int x = 0; x < w; x++)
                if (memcmp(d + x * 4, px + (y * w + x) * 4, 4)) {
                    memcpy(d + x * 4, px + (y * w + x) * 4, 4);
                    dirty[((y0 + sy) / 4) * bw + (x0 + x) / 4] = 1;
                }
        }
    }
done:
    free(px); free(S); free(I); free(L); free(stack); free(cv); free(out);
}

// ------------------------------------------------------------------ cosmetics (Cosmetics tab): tunic and heart colours
// The colour rows share one palette (runtime/cosmetic_colour.h): the chosen colour is a target for the thing's typical
// colour, reached by a hue / saturation / value change that keeps the shading.
// Tunic: tools/rando/cosmetics.py's recolour (a seed option once, now live): where the green and red tunic maps differ
// (cloth, cap, collar ring) the green pixels change (PIL HSV, 0-255) from the green tunic's typical colour (hue 145,
// saturation 0.75, value 0.55 - its map reads 0.89 / 0.38, lit it looks paler and brighter) to the chosen one. Every
// mip level is recoloured from the game's own green map, so a seed's own colour is replaced too; "Default" uploads
// whatever the files hold (green, or an older seed's colour).
// Hearts: the chosen colour is what a full heart gets; empty hearts and the frame keep their shade of it (darker).
static uint32_t tunic_rgb(void) {  // (+1: 0 = Default)
    uint32_t rgb;
    return col_target(&g_settings.tunic, COL_SLOT_TUNIC, &rgb) ? rgb + 1 : 0;
}

// LARECOMP_COSMETICS=<frame>:<tunic>:<hearts>,...  test hook: set the colours (palette names / #RRGGBB) at those frames
static void cosmetics_test(void) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_COSMETICS"); }
    long at;
    char tu[32], he[32];
    if (!next || !*next || sscanf(next, "%ld:%31[^:]:%31[^,]", &at, tu, he) != 3) return;
    extern uint64_t renderer_present_count(void);
    if ((long)renderer_present_count() < at) return;
    col_parse(&g_settings.tunic, tu);
    col_parse(&g_settings.hearts, he);
    LOG("[cosmetics] test: tunic %s, hearts %s\n", tu, he);
    next = strchr(next, ',');
    if (next) next++;
}

static float srgb_lin(int c) { float v = c / 255.0f; return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }

// Heart draws: the shader's luminance tint (translate.c `ltint`) maps the red's brightness onto the chosen colour,
// scaled so the full heart's red (255, 47, 47: linear luminance 0.319) becomes exactly that colour; the empty heart
// (92, 15, 15) and the frame's darker red keep their brightness relative to it.
void texpatch_heart_tint(int kind, float tint[8]) {
    cosmetics_test();
    uint32_t rgb;
    if (!kind || !col_target(&g_settings.hearts, COL_SLOT_HEARTS, &rgb)) return;
    for (int k = 0; k < 3; k++) tint[k] = srgb_lin((int)(rgb >> (16 - 8 * k) & 255)) / 0.319f;
    tint[3] = 1;
    tint[4] = tint[5] = tint[6] = 0;
    tint[7] = 1;
}

static void bc1_decode(const uint8_t *b, uint8_t out[16][4]) {  // (BC1: 3-colour mode when c0 <= c1)
    uint16_t e0 = rd16(b), e1 = rd16(b + 2);
    int c[4][3];
    rgb565(e0, c[0]); rgb565(e1, c[1]);
    for (int k = 0; k < 3; k++) {
        if (e0 > e1) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
        else { c[2][k] = (c[0][k] + c[1][k]) / 2; c[3][k] = 0; }
    }
    uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; i++) {
        int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 3; k++) out[i][k] = (uint8_t)c[s][k];
        out[i][3] = e0 <= e1 && s == 3 ? 0 : 255;
    }
}

static void bc1_image(const uint8_t *lin, uint32_t w, uint32_t h, uint8_t *rgb) {  // linear BC1 blocks -> RGB
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bc1_decode(lin + (by * bw + bx) * 8, px);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(rgb + (y * w + x) * 3, px[i], 3);
            }
        }
}

static uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void hsv_shift(uint8_t *p, const float *m) {  // PIL's RGB <-> HSV (all 0-255), hue moved from green
    float r = p[0], g = p[1], b = p[2], mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), h = 0, s = 0, v = mx;
    if (mx > mn) {
        s = (mx - mn) * 255 / mx;
        float rc = (mx - r) / (mx - mn), gc = (mx - g) / (mx - mn), bc = (mx - b) / (mx - mn);
        h = r == mx ? bc - gc : g == mx ? 2 + rc - bc : 4 + gc - rc;
        h = fmodf(h / 6 + 1, 1) * 255;
    }
    if (m[0] >= 0) h = fmodf(floorf(h) + (m[0] - 100 /* the green tunic's hue */) + 256, 256);
    s = clamp8(s * m[1]); v = clamp8(v * m[2]);
    float hh = h / 255 * 6, f, q, t, pp, ss = s / 255;
    int i = (int)hh % 6;
    f = hh - floorf(hh); pp = v * (1 - ss); q = v * (1 - ss * f); t = v * (1 - ss * (1 - f));
    float o[6][3] = {{v, t, pp}, {q, v, pp}, {pp, v, t}, {pp, q, v}, {t, pp, v}, {v, pp, q}};
    for (int k = 0; k < 3; k++) p[k] = clamp8(o[i][k] + 0.5f);
}

// One mip level of the recoloured tunic (linear BC1 blocks), made on first use and kept for that colour
static uint8_t *g_tunic_lv[16];
static uint32_t g_tunic_lv_colour;
static const uint8_t *tunic_level(uint32_t colour, uint32_t lv) {  // (colour: tunic_rgb())
    if (colour != g_tunic_lv_colour) {
        for (int i = 0; i < 16; i++) { free(g_tunic_lv[i]); g_tunic_lv[i] = NULL; }
        g_tunic_lv_colour = colour;
    }
    int gi = ref_of(R_TUNIC), ri = ref_of(R_TUNIC_RED);
    if (lv >= 16 || lv >= g_tunic_layout.levels || gi < 0 || ri < 0 || g_refs[gi].size != g_refs[ri].size) return NULL;
    if (g_tunic_lv[lv]) return g_tunic_lv[lv];
    LevelLayout l = tegra_level(&g_tunic_layout, lv);
    if (l.offset + (uint64_t)l.wb * l.hb * 8 > g_refs[gi].size) return NULL;
    uint32_t W = l.w, H = l.h, n = W * H, nb = l.wb * l.hb;
    uint8_t *green = malloc((size_t)nb * 8), *red = malloc((size_t)nb * 8), *gimg = malloc((size_t)n * 3), *rimg = malloc((size_t)n * 3);
    float *mask = malloc(n * sizeof(float)), *tmp = malloc(n * sizeof(float));
    tegra_deswizzle(g_refs[gi].data + l.offset, green, l.wb, l.hb, 1, 8, l.block_height_log2, l.block_depth_log2);
    tegra_deswizzle(g_refs[ri].data + l.offset, red, l.wb, l.hb, 1, 8, l.block_height_log2, l.block_depth_log2);
    bc1_image(green, W, H, gimg);
    bc1_image(red, W, H, rimg);
    // mask: luma of the difference, (d - 12) * 6, Gaussian blur (sigma 1.2) - as cosmetics.py did
    for (uint32_t i = 0; i < n; i++) {
        int d[3];
        for (int k = 0; k < 3; k++) d[k] = abs(gimg[i * 3 + k] - rimg[i * 3 + k]);
        float lu = (d[0] * 299 + d[1] * 587 + d[2] * 114) / 1000.0f;
        mask[i] = fminf(fmaxf((lu - 12) * 6, 0), 255) / 255;
    }
    float kern[7], ks = 0;
    for (int i = -3; i <= 3; i++) ks += kern[i + 3] = expf(-(float)(i * i) / (2 * 1.2f * 1.2f));
    for (int pass = 0; pass < 2; pass++) {  // (separable: rows into tmp, then columns back)
        const float *src = pass ? tmp : mask;
        float *dst = pass ? mask : tmp;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                float acc = 0;
                for (int i = -3; i <= 3; i++) {
                    int xx = (int)x + (pass ? 0 : i), yy = (int)y + (pass ? i : 0);
                    xx = xx < 0 ? 0 : xx >= (int)W ? (int)W - 1 : xx;
                    yy = yy < 0 ? 0 : yy >= (int)H ? (int)H - 1 : yy;
                    acc += src[yy * W + xx] * kern[i + 3];
                }
                dst[y * W + x] = acc / ks;
            }
    }
    float sh, sf, vf;
    col_mapping(145 / 360.0f, 0.75f, 0.55f, colour - 1, &sh, &sf, &vf);
    float m3[3] = {sh == 0 ? -1 : 100 + (sh - floorf(sh)) * 255, sf, vf};  // (hsv_shift: PIL hue, moved from the green's 100)
    uint8_t *out = malloc((size_t)nb * 8);
    memcpy(out, green, (size_t)nb * 8);
    for (uint32_t by = 0; by < l.hb; by++)
        for (uint32_t bx = 0; bx < l.wb; bx++) {
            uint8_t px[16][4];
            bool any = false;
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                x = x < W ? x : W - 1;
                y = y < H ? y : H - 1;
                uint32_t at = y * W + x;
                float m = mask[at];
                uint8_t c[3] = {gimg[at * 3], gimg[at * 3 + 1], gimg[at * 3 + 2]};
                if (m > 0.002f) {
                    any = true;
                    hsv_shift(c, m3);
                    for (int k = 0; k < 3; k++) c[k] = clamp8(gimg[at * 3 + k] + (c[k] - gimg[at * 3 + k]) * m + 0.5f);
                }
                memcpy(px[i], c, 3);
                px[i][3] = 255;
            }
            if (any) bc1_encode4(px, out + (by * l.wb + bx) * 8);
        }
    free(green); free(red); free(gimg); free(rimg); free(mask); free(tmp);
    return g_tunic_lv[lv] = out;
}

static void tunic_apply(uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    uint32_t c = tunic_rgb();
    if (!c || !g_tunic_layout_ok) return;
    const uint8_t *src = tunic_level(c, lv);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (!src || row < bw * 8) return;
    for (uint32_t y = 0; y < bh; y++) memcpy(blocks + y * row, src + y * bw * 8, bw * 8);
}

// Link's skin colour (Cosmetics tab "Body color"): any colour over his skin, keeping the shading. The skin is told by
// its colour in the maps (BC1): a peach hue, not grey, bright - so the brows and eyes (dark), the mouth's dark lines
// and open mouths, the hair, the tunic and the leather stay (skin_face.h skin_weight). The colour row's target is
// reached from the skin's typical colour (hue 21, saturation 0.47, value 0.96).
static uint32_t skin_rgb(void) {  // (+1: 0 = Default)
    uint32_t rgb;
    return col_target(&g_settings.skin, COL_SLOT_SKIN, &rgb) ? rgb + 1 : 0;
}

// body: the tunic map (arms, hands), recoloured block by block, the skin told on `orig` (the map as uploaded: a
// tunic recoloured to a peach hue would read as skin; NULL: on the blocks themselves); else a face map, recoloured as
// a whole level (skin_face: with a very dark colour its details are told apart by shape). mouth: one of the mouth maps.
// The face maps' smaller mip levels are made from the recoloured full-size level (box filter), as the game's own are
// from its: recoloured on their own, the details' shapes are too few pixels to tell apart (an open mouth's outline
// would fill it). The levels of one upload come in order, on one thread: the full-size result is kept per thread.
static __thread struct { int id; uint32_t c, w, h; uint8_t *img; } t_face;

static void skin_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row, bool body, bool mouth,
                       const uint8_t *orig) {
    uint32_t c = skin_rgb();
    if (!c) return;
    float sh, ts, tv;
    SkinParams k;
    col_mapping(21 / 360.0f, 0.47f, 0.96f, c - 1, &sh, &k.sf, &k.vf);
    col_hsv(c - 1, &k.th, &ts, &tv);
    k.grey = sh == 0 && ts < 0.08f;
    // A very dark body colour (Black, or any Custom / Random one this dark: luminance under 0.22) would swallow the
    // face's dark details - pupils, brows, the mouth's and ears' lines, all drawn dark in the face maps on the skin.
    // Then they turn light, open mouths keeping their dark inside within a light outline (skin_face).
    float lum = (0.299f * (float)((c - 1) >> 16 & 255) + 0.587f * (float)((c - 1) >> 8 & 255) + 0.114f * (float)((c - 1) & 255)) / 255;
    k.light = !body && lum < 0.22f;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (row < bw * 8) return;
    if (body) {
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                uint8_t *b = blocks + by * row + bx * 8, px[16][4], ox[16][4];
                bc1_decode(b, px);
                bc1_decode(orig ? orig + by * row + bx * 8 : b, ox);
                bool any = false, holes = false;
                for (int i = 0; i < 16; i++) {
                    if (!px[i][3]) { holes = true; continue; }
                    float m = skin_weight(ox[i], true, false);
                    if (m < 0.004f) continue;
                    any = true;
                    skin_recolour(px[i], m, &k);
                }
                if (any && !holes) bc1_encode4(px, b);
            }
        return;
    }
    uint32_t iw = bw * 4, ih = bh * 4;  // (whole blocks: a level under 4 x 4 is padded)
    uint8_t *img = malloc((size_t)iw * ih * 4), *was = malloc((size_t)iw * ih * 4);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bc1_decode(blocks + by * row + bx * 8, px);
            for (int i = 0; i < 16; i++) memcpy(img + (((by * 4 + (i >> 2)) * iw) + bx * 4 + (i & 3)) * 4, px[i], 4);
        }
    memcpy(was, img, (size_t)iw * ih * 4);
    if (lv && t_face.img && t_face.id == id && t_face.c == c && (t_face.w >> lv ? t_face.w >> lv : 1) == w &&
        (t_face.h >> lv ? t_face.h >> lv : 1) == h) {
        uint32_t f = 1u << lv;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t acc = 0, cnt = 0;
                    for (uint32_t yy = y * f; yy < (y + 1) * f && yy < t_face.h; yy++)
                        for (uint32_t xx = x * f; xx < (x + 1) * f && xx < t_face.w; xx++, cnt++)
                            acc += t_face.img[(yy * t_face.w + xx) * 4 + ch];
                    if (cnt) img[(y * iw + x) * 4 + ch] = (uint8_t)((acc + cnt / 2) / cnt);
                }
    } else {
        skin_face(img, iw, ih, mouth, &k);
        if (!lv && iw == w && ih == h) {
            free(t_face.img);
            t_face.img = malloc((size_t)w * h * 4);
            memcpy(t_face.img, img, (size_t)w * h * 4);
            t_face.id = id, t_face.c = c, t_face.w = w, t_face.h = h;
        }
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bool any = false, holes = false;
            for (int i = 0; i < 16; i++) {
                size_t at = (((by * 4 + (i >> 2)) * iw) + bx * 4 + (i & 3)) * 4;
                memcpy(px[i], img + at, 4);
                holes |= !was[at + 3];
                any |= memcmp(img + at, was + at, 3) != 0;
            }
            if (any && !holes) bc1_encode4(px, blocks + by * row + bx * 8);
        }
    free(img);
    free(was);
}

// pictogram.bffnt: glyph index -> role (its CMAP: '+' 0, '-' 1, 'A' 2, 'B' 3, 'L' 6, 'R' 8, 'X' 10, 'Y' 11, 'Z' 12
// = ZL, 'l' 13 / 'r' 15 = L / R (second style), 'z' 17 = ZR); 4 columns of 64x64 cells, rows from the bottom.
static const int8_t k_sheet_roles[18] = {R_PLUS, R_MINUS, R_A, R_B, 0, 0, R_L, 0, R_R, 0, R_X, R_Y, R_ZL, R_L, 0, R_R, 0, R_ZR};

void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    if (id < 0 || id >= g_nrefs) return;
    const Ref *r = &g_refs[id];
    if (r->role == R_TUNIC || r->role == R_TUNIC_RED || r->role == R_TUNIC_BLUE) {
        size_t n = (size_t)((h + 3) / 4 - 1) * row + (w + 3) / 4 * 8;  // (the level's blocks)
        uint8_t *orig = tunic_rgb() && skin_rgb() && row >= (w + 3) / 4 * 8 ? malloc(n) : NULL;
        if (orig) memcpy(orig, blocks, n);
        tunic_apply(lv, blocks, w, h, row);
        skin_apply(id, lv, blocks, w, h, row, true, false, orig);  // (the arms and hands)
        free(orig);
        return;
    }
    if (r->role == R_SKIN) { skin_apply(id, lv, blocks, w, h, row, false, !strncmp(r->name, "MI_mouth_alb", 12), NULL); return; }
    if (lv || r->w != w || r->h != h) return;
    if (r->role == R_BOTTLE || r->role == R_BOTTLE_MODEL || r->role == R_FAIRY_MODEL || r->role == R_POWDER_MODEL ||
        r->role == R_ITEM_CURSOR || r->role == R_DGN_MAP || r->role == R_MAP_MODEL || r->role == R_POWER_PIECE_MODEL ||
        r->role == R_HEART) return;
    if (r->role == R_HERO) {  // fully transparent (BC5 blocks of zeros)
        if (hide_hero()) for (uint32_t y = 0; y < (h + 3) / 4; y++) memset(blocks + y * row, 0, (w + 3) / 4 * 16);
        return;
    }
    int style = prompt_style();
    if (style == STYLE_NINTENDO) {  // the game's own look, unless a button was remapped
        bool any = false;
        for (int rl = R_A; rl <= R_MINUS; rl++) any |= restyled(rl, style);
        if (!any) return;
    }
    static pthread_once_t font_once = PTHREAD_ONCE_INIT;
    pthread_once(&font_once, load_font);
    int bw = (int)(w + 3) / 4;
    uint8_t *img = malloc((size_t)w * h * 4), *dirty = calloc((size_t)bw * ((h + 3) / 4), 1);
    decode_image(r, blocks, row, img);
    int ach = r->kind == K_BC5 ? 1 : 3;
    if (r->role == R_SHEET) {
        for (int g = 0; g < (int)sizeof k_sheet_roles; g++) {
            int y0 = (int)h - (g / 4 + 1) * 64;
            if (k_sheet_roles[g] && y0 >= 0 && (g % 4 + 1) * 64 <= (int)w && restyled(k_sheet_roles[g], style))
                relabel(img, (int)w, (g % 4) * 64, y0, 64, 64, ach, true, k_sheet_roles[g], style, dirty, bw);
        }
    } else if (restyled(r->role, style)) {
        relabel(img, (int)w, 0, 0, (int)w, (int)h, ach, false, r->role, style, dirty, bw);
    }
    encode_image(r, img, dirty, blocks, row);
    free(img);
    free(dirty);
}

// The Hint Sheet's icon (runtime/hle/hint_sheet.c): the dungeon map's (120 x 120) centred on a bottle-sized canvas
// (144 x 144, as ItemBottle_00^w: the same pane draws it at the size the game draws its own item icons), RGBA8 sRGB,
// made once from the user's own game files. The renderers sample it in place of the bottle icon for the Hint Sheet's
// draws. NULL: the game files don't have it.
NvnTexture *texpatch_hint_icon(void) {
    static NvnTexture t;
    static int state;  // 0 not made yet, 1 made, -1 none
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!state) {
        state = -1;
        pthread_once(&g_once, load_refs);
        int i = ref_of(R_DGN_MAP);
        if (i >= 0 && g_refs[i].kind == K_BC3) {
            const Ref *r = &g_refs[i];
            uint32_t bw = (r->w + 3) / 4, bh = (r->h + 3) / 4, S = 144;
            uint8_t *lin = malloc((size_t)bw * bh * 16), *img = malloc((size_t)r->w * r->h * 4), *canvas = calloc((size_t)S * S, 4);
            tegra_deswizzle(r->data, lin, bw, bh, 1, 16, r->bhl, 0);
            decode_image(r, lin, bw * 16, img);
            uint32_t ox = (S - r->w) / 2, oy = (S - r->h) / 2;
            for (uint32_t y = 0; y < r->h; y++) memcpy(canvas + ((y + oy) * S + ox) * 4, img + y * r->w * 4, r->w * 4);
            t.target = 1;  // (2D)
            t.format = r->srgb ? 0x38 : 0x25;  // RGBA8 sRGB / unorm, as the map's own colour space
            t.width = t.height = S;
            t.depth = t.levels = 1;
            t.layout = (TexLayout){.width = S, .height = S, .depth = 1, .layers = 1, .levels = 1, .fmt = nvn_format_info(t.format)};
            tegra_layout_init(&t.layout);
            uint64_t total = tegra_total_size(&t.layout);
            uint8_t *mem = calloc(total, 1);
            LevelLayout l0 = tegra_level(&t.layout, 0);
            tegra_swizzle(canvas, mem, l0.wb, l0.hb, 1, 4, l0.block_height_log2, l0.block_depth_log2);
            t.addr = (uint64_t)(uintptr_t)mem;
            strlcpy(t.label, "HintSheetIcon", sizeof t.label);
            free(lin), free(img), free(canvas);
            state = 1;
            LOG("[texpatch] Hint Sheet icon made from %s\n", r->name);
        }
    }
    pthread_mutex_unlock(&lock);
    return state > 0 ? &t : NULL;
}

// The game's own UI textures, by name (e.g. "MapMark_05^w"), decoded to RGBA8 (w x h, the texture's own colour
// encoding) from the user's UI archives: out[i] NULL if it isn't there or its format isn't one of ours. Each archive is
// read once for the whole list. The caller frees them; returns how many were found.
// (Runtime-made icons for our overlays: runtime/ui/triforce_icons.mm, race_overlay.mm - nothing of the game ships.)
int texpatch_ui_rgba_list(const char *const *want, int nwant, uint8_t **out, int *w_out, int *h_out) {
    static const char *const arcs[] = {"region_common/ui/Game.arc", "regionUS/common/ui/GameRegionCommon.arc",
                                       "regionEU/common/ui/GameRegionCommon.arc", "region_common/ui/StartUp.arc"};
    int found = 0;
    for (int i = 0; i < nwant; i++) out[i] = NULL;
    for (int a = 0; a < 4 && found < nwant; a++) {
        size_t len;
        uint8_t *d = read_all(arcs[a], &len);
        if (!d) continue;
        if (len > 0x20 && !memcmp(d, "SARC", 4)) {
            uint32_t data_off = rd32(d + 0xC);
            uint16_t n = rd16(d + 0x1A);
            const uint8_t *ent = d + 0x20, *names = d + 0x20 + 16 * n + 8;
            for (uint16_t i = 0; i < n && found < nwant && ent + 16 * (i + 1) <= d + len; i++) {
                uint32_t attr = rd32(ent + 16 * i + 4), s = rd32(ent + 16 * i + 8), e = rd32(ent + 16 * i + 12);
                const char *nm = (attr >> 24) ? (const char *)names + (attr & 0xffffff) * 4 : "";
                if ((const uint8_t *)nm >= d + len || strcmp(nm, "timg/__Combined.bntx") || data_off + e > len) continue;
                const uint8_t *b = d + data_off + s;
                size_t blen = e - s;
                if (blen < 0x40 || memcmp(b, "BNTX", 4)) continue;
                uint32_t count = rd32(b + 0x24);
                uint64_t infos = rd64(b + 0x28);
                for (uint32_t k = 0; k < count && found < nwant && infos + 8 * k + 8 <= blen; k++) {
                    uint64_t brti = rd64(b + infos + 8 * k);
                    if (brti + 0x90 > blen || memcmp(b + brti, "BRTI", 4)) continue;
                    const uint8_t *t = b + brti + 16;
                    uint64_t name_at = rd64(t + 80), ptrs = rd64(t + 96);
                    if (name_at + 2 >= blen) continue;
                    uint16_t nl = rd16(b + name_at);
                    int wi = -1;
                    for (int q = 0; q < nwant && wi < 0; q++)
                        if (!out[q] && nl == strlen(want[q]) && !memcmp(b + name_at + 2, want[q], nl)) wi = q;
                    if (wi < 0 || ptrs + 8 > blen) continue;
                    uint32_t fmt = rd32(t + 12), w = rd32(t + 20), h = rd32(t + 24), size = rd32(t + 64);
                    uint64_t data = rd64(b + ptrs);
                    int kind = (fmt >> 8) == 0x1c ? K_BC3 : (fmt >> 8) == 0x2d ? K_ASTC4 : -1;
                    if (kind < 0 || data + size > blen) continue;
                    Ref r = {0, (uint8_t)kind, (uint8_t)((fmt & 0xff) == 6), 0, (uint16_t)w, (uint16_t)h, 1, size, NULL, ""};
                    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                    uint8_t *lin = calloc((size_t)bw * bh, 16);
                    tegra_deswizzle(b + data, lin, bw, bh, 1, 16, rd32(t + 36) & 7, 0);
                    out[wi] = malloc((size_t)w * h * 4);
                    decode_image(&r, lin, bw * 16, out[wi]);
                    free(lin);
                    w_out[wi] = (int)w, h_out[wi] = (int)h;
                    found++;
                }
            }
        }
        free(d);
    }
    return found;
}

// One of them (NULL: not there)
uint8_t *texpatch_ui_rgba(const char *want, int *w_out, int *h_out) {
    uint8_t *out;
    return texpatch_ui_rgba_list(&want, 1, &out, w_out, h_out) ? out : NULL;
}

static void load_font_once(void) {
    static int done;
    if (!done) { done = 1; if (!g_font_ok) load_font(); }
}

// ================================================================== Diablo III: PlayStation button prompts
// Diablo III keeps its controller prompts in one 2048x256 BC3 atlas (recognised by its contents' hash at upload).
// Its Nintendo labels are replaced as the texture is uploaded, following the default DualSense layout
// (runtime/hle/bindings.c: Cross = A, Circle = B, Triangle = X, Square = Y; L1 / R1 / L2 / R2 = L / R / ZL / ZR):
// the round buttons keep their grey body with a white symbol in place of the letter, the shoulder buttons get the
// PlayStation names, and the bare letters used inside text become white symbols / names. Simple geometry and the
// system font are drawn here; nothing is taken from other games or mods.
enum { D3_ROUND, D3_SHOULDER, D3_PLAIN };
enum { D3_CROSS, D3_CIRCLE, D3_SQUARE, D3_TRIANGLE, D3_TEXT };
typedef struct { uint16_t x0, x1, y0, y1; uint8_t kind, sym; const char *text; } D3Cell;
static const D3Cell d3_cells[] = {
    {280, 333, 64, 128, D3_ROUND, D3_CIRCLE, NULL},     // B
    {348, 401, 64, 128, D3_ROUND, D3_CROSS, NULL},      // A
    {416, 469, 64, 128, D3_ROUND, D3_SQUARE, NULL},     // Y
    {484, 537, 64, 128, D3_ROUND, D3_TRIANGLE, NULL},   // X
    {549, 608, 64, 128, D3_SHOULDER, D3_TEXT, "L2"},    // L
    {617, 676, 64, 128, D3_SHOULDER, D3_TEXT, "L1"},    // R
    {684, 745, 64, 128, D3_SHOULDER, D3_TEXT, "R1"},    // ZL
    {752, 813, 64, 128, D3_SHOULDER, D3_TEXT, "R2"},    // ZR
    {841, 861, 64, 128, D3_PLAIN, D3_CIRCLE, NULL},     // B (in text)
    {975, 997, 64, 128, D3_PLAIN, D3_SQUARE, NULL},     // Y
    {1043, 1066, 64, 128, D3_PLAIN, D3_TRIANGLE, NULL}, // X
    {1114, 1132, 64, 128, D3_PLAIN, D3_TEXT, "L2"},     // L
    {1181, 1201, 64, 128, D3_PLAIN, D3_TEXT, "L1"},     // R
    {1240, 1277, 64, 128, D3_PLAIN, D3_TEXT, "R1"},     // ZL
    {1307, 1346, 64, 128, D3_PLAIN, D3_TEXT, "R2"},     // ZR
    {1110, 1135, 128, 192, D3_PLAIN, D3_CROSS, NULL},   // A
};

static int luma(const uint8_t *p) { return (p[0] * 3 + p[1] * 6 + p[2]) / 10; }
static uint8_t *g_d3_full;  // the patched level 0 as RGBA (for the smaller levels)
static uint32_t g_d3_w, g_d3_h;

// The shoulder labels follow the controller's two layouts (runtime/hle/input.c d3_menu_shoulders): in play L = L2,
// R = L1, ZL = R1, ZR = R2; in the game's menus the tabs move with L / R, which are L1 / R1 there (and ZL / ZR are
// L2 / R2). The atlas is drawn for the mode it is uploaded in, and uploaded again when the mode changes
// (vulkan/textures.c).
// g_d3_label_menu: the menu layout (else play); g_d3_label_xbox: an Xbox-style controller is in use (the tester 2026-11-15:
// Xbox controllers too) - A / B / X / Y in their colours and LB / RB / LT / RT, by the same positions.
int g_d3_label_menu, g_d3_label_xbox;
static const char *d3_label(const char *t) {
    if (!t) return t;
    int sw = !strcmp(t, "L2") ? 0 : !strcmp(t, "L1") ? 1 : !strcmp(t, "R1") ? 2 : !strcmp(t, "R2") ? 3 : -1;  // (L R ZL ZR)
    if (sw < 0) return t;
    static const char *const lab[2][2][4] = {
        {{"L2", "L1", "R1", "R2"}, {"L1", "R1", "L2", "R2"}},  // PlayStation: play, menus
        {{"LT", "LB", "RB", "RT"}, {"LB", "RB", "LT", "RT"}},  // Xbox: play, menus
    };
    return lab[g_d3_label_xbox ? 1 : 0][g_d3_label_menu ? 1 : 0][sw];
}
// The face button symbols' colours: PlayStation (Cross blue, Circle red, Square pink, Triangle green) or Xbox (A green,
// B red, X blue, Y yellow), by the symbol the cell stands for (D3_CROSS = the bottom button ... )
static const uint8_t *d3_sym_colour(int sym) {
    static const uint8_t ps[4][3] = {{66, 124, 222}, {255, 102, 102}, {255, 105, 248}, {64, 226, 160}};
    static const uint8_t xb[4][3] = {{96, 196, 64}, {236, 70, 60}, {64, 136, 236}, {242, 196, 40}};
    return g_d3_label_xbox ? xb[sym & 3] : ps[sym & 3];
}

static void d3_symbol(float *cv, int sym, const char *text) {
    memset(cv, 0, sizeof(float) * CV * CV);
    if (g_d3_label_xbox && sym < D3_TEXT) {  // (Xbox: the letter of the button in that position)
        static const char *const letter[4] = {"A", "B", "X", "Y"};  // (bottom, right, left, top)
        draw_text(cv, letter[sym]);
        return;
    }
    switch (sym) {
    case D3_CROSS: { Seg s[2] = {{58, 58, 198, 198}, {198, 58, 58, 198}}; draw_segs(cv, s, 2, 17); break; }
    case D3_CIRCLE: draw_ring(cv, 128, 128, 80, 32); break;
    case D3_SQUARE: draw_rect(cv, 54, 54, 202, 202, 32); break;
    case D3_TRIANGLE: { Seg s[3] = {{128, 34, 222, 200}, {222, 200, 34, 200}, {34, 200, 128, 34}}; draw_segs(cv, s, 3, 16); break; }
    default: draw_text(cv, text); break;
    }
}

// Coverage of canvas box (cx0, cy0, cw, ch) in the cell pixel (px, py) of a box (bx, by, bw, bh): 4x4 supersampled
static float d3_cover(const float *cv, float u0, float v0, float uw, float vh, float fx, float fy) {
    float sum = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int u = (int)(u0 + (fx + (i + 0.5f) / 4) * uw), v = (int)(v0 + (fy + (j + 0.5f) / 4) * vh);
            if (u >= 0 && u < CV && v >= 0 && v < CV) sum += cv[v * CV + u];
        }
    return sum / 16;
}

// The canvas' drawn bounds (to fit text / symbols by their ink)
static void d3_ink(const float *cv, int *x0, int *y0, int *x1, int *y1) {
    *x0 = CV; *y0 = CV; *x1 = 0; *y1 = 0;
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++)
            if (cv[y * CV + x] > 0.2f) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
    if (*x1 < *x0) { *x0 = *y0 = 0; *x1 = *y1 = CV - 1; }
}

// The face-button diagrams (four circles, the button to press filled in, e.g. "Press [diagram] to join") show the
// Nintendo position: A is the right circle, B the bottom one. With the DualSense layout A is Cross (bottom) and B is
// Circle (right), so the right and bottom circles of every diagram trade places. (Measured: circle centres at
// (44.5, 32.6) and (26.8, 50.3) in each 55x64 cell of the atlas' first row.)
static void d3_swap_diagram(uint8_t *blocks, uint32_t row, uint32_t x0) {
    uint32_t bx0 = x0 / 4, bx1 = (x0 + 56 + 3) / 4, by1 = 64 / 4, cw = (bx1 - bx0) * 4, ch = 64;
    uint8_t *img = calloc((size_t)cw * ch, 4);
    if (!img) return;
    for (uint32_t by = 0; by < by1; by++)
        for (uint32_t bx = bx0; bx < bx1; bx++) {
            const uint8_t *b = blocks + (size_t)by * row + bx * 16;
            uint8_t a[16], px[16][4];
            bc4_decode(b, a);
            bc1_decode4(b + 8, px);
            for (int i = 0; i < 16; i++) {
                uint8_t *o = img + ((by * 4 + i / 4) * cw + (bx - bx0) * 4 + i % 4) * 4;
                o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2]; o[3] = a[i];
            }
        }
    uint32_t off = x0 - bx0 * 4;  // (the cell's x inside the decoded blocks)
    for (uint32_t y = 0; y < 18; y++)
        for (uint32_t x = 0; x < 18; x++) {
            uint8_t *r = img + ((24 + y) * cw + off + 36 + x) * 4, *b = img + ((41 + y) * cw + off + 18 + x) * 4, t[4];
            memcpy(t, r, 4); memcpy(r, b, 4); memcpy(b, t, 4);
        }
    for (uint32_t by = 0; by < by1; by++)
        for (uint32_t bx = bx0; bx < bx1; bx++) {
            uint8_t *b = blocks + (size_t)by * row + bx * 16;
            uint8_t a[16], px[16][4];
            for (int i = 0; i < 16; i++) {
                const uint8_t *s = img + ((by * 4 + i / 4) * cw + (bx - bx0) * 4 + i % 4) * 4;
                memcpy(px[i], s, 4);
                a[i] = s[3];
            }
            bc4_encode(a, b);
            bc1_encode4(px, b + 8);
        }
    free(img);
}

// RGBA of the blocks covering pixels [x0, x1) x [y0, y1) (block-aligned outwards); *ox = x0's column in it
static uint8_t *d3_region_get(const uint8_t *blocks, uint32_t row, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1,
                              uint32_t *bx0, uint32_t *bx1, uint32_t *cw) {
    *bx0 = x0 / 4; *bx1 = (x1 + 3) / 4; *cw = (*bx1 - *bx0) * 4;
    uint32_t ch = y1 - y0;
    uint8_t *img = calloc((size_t)*cw * ch, 4);
    if (!img) return NULL;
    for (uint32_t by = y0 / 4; by < y1 / 4; by++)
        for (uint32_t bx = *bx0; bx < *bx1; bx++) {
            const uint8_t *b = blocks + (size_t)by * row + bx * 16;
            uint8_t a[16], px[16][4];
            bc4_decode(b, a);
            bc1_decode4(b + 8, px);
            for (int i = 0; i < 16; i++) {
                uint8_t *o = img + (((by * 4 - y0) + i / 4) * *cw + (bx - *bx0) * 4 + i % 4) * 4;
                o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2]; o[3] = a[i];
            }
        }
    return img;
}
static void d3_region_put(uint8_t *blocks, uint32_t row, const uint8_t *img, uint32_t bx0, uint32_t bx1, uint32_t cw,
                          uint32_t y0, uint32_t y1) {
    for (uint32_t by = y0 / 4; by < y1 / 4; by++)
        for (uint32_t bx = bx0; bx < bx1; bx++) {
            uint8_t *b = blocks + (size_t)by * row + bx * 16;
            uint8_t a[16], px[16][4];
            for (int i = 0; i < 16; i++) {
                const uint8_t *s = img + (((by * 4 - y0) + i / 4) * cw + (bx - bx0) * 4 + i % 4) * 4;
                memcpy(px[i], s, 4);
                a[i] = s[3];
            }
            bc4_encode(a, b);
            bc1_encode4(px, b + 8);
        }
}

// The "single Joy-Con held sideways" diagrams (four small circles, one lit: "Press [right] to join!") become the
// PlayStation button that does it: lit right = A = Cross, bottom = B = Circle, top = X = Triangle, left = Y = Square.
// The already redrawn round button is copied in, centred (cells measured in the atlas' second row).
// (2026-11-16: these four are the D-PAD - the hints of the map (Zoom In / Out, Recenter, Reset Zoom = up / down / left /
// right) and "Stat Ranges" (left) use them; drawn as face buttons they sent the tester to the wrong buttons. Each is now a
// round button with a d-pad cross on it, the direction to press lit. dir: 0 up, 1 down, 2 left, 3 right)
static const struct { uint16_t dx0, dx1, sx0; uint8_t dir; } d3_sideways[] = {
    {1501, 1561, 416, 2},  // left lit
    {1569, 1629, 348, 3},  // right lit
    {1637, 1697, 484, 0},  // top lit
    {1705, 1765, 280, 1},  // bottom lit
};
// the d-pad cross over a round button already in `img` (cw wide, 64 high), centred in columns [x0, x0 + 54)
static void d3_dpad_draw(uint8_t *img, uint32_t cw, uint32_t x0, int dir) {
    int ix0 = 1 << 30, iy0 = 1 << 30, ix1 = -1, iy1 = -1;
    long sr = 0, sg = 0, sb = 0, n = 0;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = x0; x < x0 + 54 && x < cw; x++) {
            uint8_t *p = img + (y * cw + x) * 4;
            if (p[3] > 40) {
                if ((int)x < ix0) ix0 = x;
                if ((int)x > ix1) ix1 = x;
                if ((int)y < iy0) iy0 = y;
                if ((int)y > iy1) iy1 = y;
            }
            if (p[3] > 220 && luma(p) > 30 && luma(p) < 110) sr += p[0], sg += p[1], sb += p[2], n++;
        }
    if (ix1 < 0 || !n) return;
    float cx = (ix0 + ix1 + 1) / 2.0f, cy = (iy0 + iy1 + 1) / 2.0f, r = (ix1 - ix0 + iy1 - iy0 + 2) / 4.0f;
    uint8_t body[3] = {(uint8_t)(sr / n), (uint8_t)(sg / n), (uint8_t)(sb / n)};
    float a = 0.19f * r, L = 0.66f * r;  // (arm half-width, arm length)
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = x0; x < x0 + 54 && x < cw; x++) {
            uint8_t *p = img + (y * cw + x) * 4;
            if (hypotf(x + 0.5f - cx, y + 0.5f - cy) < r * 0.78f) p[0] = body[0], p[1] = body[1], p[2] = body[2];
            float lit = 0, dim = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    float u = x + (i + 0.5f) / 4 - cx, v = y + (j + 0.5f) / 4 - cy;
                    bool vert = fabsf(u) <= a && fabsf(v) <= L, horz = fabsf(v) <= a && fabsf(u) <= L;
                    if (!vert && !horz) continue;
                    bool on = (dir == 0 && v < -a) || (dir == 1 && v > a) || (dir == 2 && u < -a) || (dir == 3 && u > a);
                    if (on) lit += 1 / 16.0f; else dim += 1 / 16.0f;
                }
            for (int q = 0; q < 3; q++) {
                float c = p[q] * (1 - lit - dim) + 255 * lit + 52 * dim;  // (the other arms dark: the lit one stands out small)
                p[q] = (uint8_t)(c > 255 ? 255 : c);
            }
        }
}
static void d3_sideways_icons(uint8_t *blocks, uint32_t row) {
    for (unsigned i = 0; i < sizeof d3_sideways / sizeof *d3_sideways; i++) {
        uint32_t sbx0, sbx1, scw, dbx0, dbx1, dcw;
        uint8_t *src = d3_region_get(blocks, row, d3_sideways[i].sx0, d3_sideways[i].sx0 + 54, 64, 128, &sbx0, &sbx1, &scw);
        uint8_t *dst = d3_region_get(blocks, row, d3_sideways[i].dx0, d3_sideways[i].dx1, 64, 128, &dbx0, &dbx1, &dcw);
        if (src && dst) {
            for (uint32_t k = 0; k < dcw * 64; k++) dst[k * 4 + 3] = 0;  // (the old diagram goes)
            uint32_t soff = d3_sideways[i].sx0 - sbx0 * 4, doff = d3_sideways[i].dx0 - dbx0 * 4;
            uint32_t dw = d3_sideways[i].dx1 - d3_sideways[i].dx0, pad = dw > 54 ? (dw - 54) / 2 : 0;
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 54 && doff + pad + x < dcw && soff + x < scw; x++)
                    memcpy(dst + (y * dcw + doff + pad + x) * 4, src + (y * scw + soff + x) * 4, 4);
            if (!getenv("D3_DPAD_AS_FACE")) d3_dpad_draw(dst, dcw, doff + pad, d3_sideways[i].dir);
            d3_region_put(blocks, row, dst, dbx0, dbx1, dcw, 64, 128);
            // (the bottom-lit diagram's lit circle reaches below its cell: an orange dot stayed under the icon)
            uint32_t ebx0, ebx1, ecw;
            uint8_t *below = d3_region_get(blocks, row, d3_sideways[i].dx0, d3_sideways[i].dx1, 128, 136, &ebx0, &ebx1, &ecw);
            if (below) {
                for (uint32_t k = 0; k < ecw * 8; k++) below[k * 4 + 3] = 0;
                d3_region_put(blocks, row, below, ebx0, ebx1, ecw, 128, 136);
                free(below);
            }
        }
        free(src);
        free(dst);
    }
}

// The face-button diagrams of the first row (four yellow circles, the one to press filled in): the world's
// interaction prompts (Stash, Mailbox, people) and "Press [diagram] to join" draw these. Each becomes the PlayStation
// round button for the lit circle - right = A = Cross, bottom = B = Circle, top = X = Triangle, left = Y = Square
// (the tester 2026-11-15: swapping the circles still showed a diagram). Cells measured: 54 px wide from these x, circle
// centres 17.7 px from the cell centre (26.8, 32.6).
static const uint16_t d3_diag_x[] = {71, 203, 335, 467, 599, 731, 1195, 1263, 1331, 1399, 1467, 1535};
enum { D3_NDIAG = sizeof d3_diag_x / sizeof *d3_diag_x };
static int d3_diag_lit(const uint8_t *blocks, uint32_t row, uint32_t x0) {  // 0 right, 1 bottom, 2 top, 3 left
    static const float off[4][2] = {{17.7f, 0}, {0, 17.7f}, {0, -17.7f}, {-17.7f, 0}};
    uint32_t bx0, bx1, cw;
    uint8_t *img = d3_region_get(blocks, row, x0, x0 + 56, 0, 64, &bx0, &bx1, &cw);
    if (!img) return 0;
    uint32_t ox = x0 - bx0 * 4;
    int best = 0;
    float bestv = -1;
    for (int k = 0; k < 4; k++) {
        float cx = ox + 26.8f + off[k][0], cy = 32.6f + off[k][1], v = 0;
        for (int y = (int)cy - 4; y <= (int)cy + 4; y++)
            for (int x = (int)cx - 4; x <= (int)cx + 4; x++) {
                if (x < 0 || y < 0 || x >= (int)cw || y >= 64) continue;
                const uint8_t *q = img + ((size_t)y * cw + x) * 4;
                v += q[3] / 255.0f * luma(q);  // (a lit circle is filled; the others are rings, empty inside)
            }
        if (v > bestv) bestv = v, best = k;
    }
    free(img);
    return best;
}
// (test, D3_DIAG_NUMBERS=1) diagram cell i gets the letter 'A' + i instead, to see which cell a hint uses on screen
static void d3_diag_letter(uint8_t *blocks, uint32_t row, int i) {
    static float cv[CV * CV];
    char t[2] = {(char)('A' + i), 0};
    d3_symbol(cv, D3_TEXT, t);
    int u0, v0, u1, v1;
    d3_ink(cv, &u0, &v0, &u1, &v1);
    uint32_t bx0, bx1, cw;
    uint8_t *dst = d3_region_get(blocks, row, d3_diag_x[i], d3_diag_x[i] + 56, 0, 64, &bx0, &bx1, &cw);
    if (!dst) return;
    uint32_t doff = d3_diag_x[i] - bx0 * 4;
    float bw = 44, bh = 52, iw = (float)(u1 - u0 + 1), ih = (float)(v1 - v0 + 1), sc = fminf(bw / iw, bh / ih);
    float ox = doff + 5 + (bw - iw * sc) / 2, oy = 6 + (bh - ih * sc) / 2;
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = doff; x < doff + 54 && x < cw; x++) {
            uint8_t *p = dst + (y * cw + x) * 4;
            float a = d3_cover(cv, u0 + (x - ox) / sc, v0 + (y - oy) / sc, 1 / sc, 1 / sc, 0, 0);
            p[0] = (uint8_t)(255 * a); p[1] = (uint8_t)(220 * a); p[2] = 0; p[3] = a > 0.05f ? 255 : 160;
        }
    d3_region_put(blocks, row, dst, bx0, bx1, cw, 0, 64);
    free(dst);
}
static void d3_diagram_icons(uint8_t *blocks, uint32_t row, const int *lit) {
    if (getenv("D3_DIAG_NUMBERS")) {
        for (int i = 0; i < D3_NDIAG; i++) d3_diag_letter(blocks, row, i);
        return;
    }
    static const uint16_t src_x[4] = {348, 280, 484, 416};  // Cross, Circle, Triangle, Square (round buttons, row 2)
    for (int i = 0; i < D3_NDIAG; i++) {
        uint32_t sbx0, sbx1, scw, dbx0, dbx1, dcw, sx0 = src_x[lit[i]];
        uint8_t *src = d3_region_get(blocks, row, sx0, sx0 + 54, 64, 128, &sbx0, &sbx1, &scw);
        uint8_t *dst = d3_region_get(blocks, row, d3_diag_x[i], d3_diag_x[i] + 56, 0, 64, &dbx0, &dbx1, &dcw);
        if (src && dst) {
            uint32_t soff = sx0 - sbx0 * 4, doff = d3_diag_x[i] - dbx0 * 4;
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 54; x++) dst[(y * dcw + doff + x) * 4 + 3] = 0;  // (the diagram goes)
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 54 && doff + x < dcw && soff + x < scw; x++)
                    memcpy(dst + (y * dcw + doff + x) * 4, src + (y * scw + soff + x) * 4, 4);
            d3_region_put(blocks, row, dst, dbx0, dbx1, dcw, 0, 64);
        }
        free(src);
        free(dst);
    }
}

static void d3_buttons_draw(uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    load_font_once();
    static float cv[CV * CV];
    int patched = 0;
    int lit[D3_NDIAG];
    bool diagrams = w >= 2048 && h >= 128;
    if (diagrams)  // (which circle each diagram lights, before anything is drawn over)
        for (int i = 0; i < D3_NDIAG; i++) lit[i] = d3_diag_lit(blocks, row, d3_diag_x[i]);
    for (unsigned c = 0; c < sizeof d3_cells / sizeof *d3_cells; c++) {
        const D3Cell *k = &d3_cells[c];
        uint32_t bx0 = k->x0 / 4, bx1 = (k->x1 + 3) / 4, by0 = k->y0 / 4, by1 = k->y1 / 4;
        if (bx1 * 4 > w || by1 * 4 > h) continue;
        uint32_t cw = (bx1 - bx0) * 4, ch = (by1 - by0) * 4;
        uint8_t *img = calloc((size_t)cw * ch, 4);
        for (uint32_t by = by0; by < by1; by++)  // decode: BC3 = BC4 alpha + BC1 colour
            for (uint32_t bx = bx0; bx < bx1; bx++) {
                const uint8_t *b = blocks + (size_t)by * row + bx * 16;
                uint8_t a[16], px[16][4];
                bc4_decode(b, a);
                bc1_decode4(b + 8, px);
                for (int i = 0; i < 16; i++) {
                    uint8_t *o = img + (((by - by0) * 4 + i / 4) * cw + (bx - bx0) * 4 + i % 4) * 4;
                    o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2]; o[3] = a[i];
                }
            }
        // the button's / letter's extent
        int ix0 = cw, iy0 = ch, ix1 = -1, iy1 = -1;
        for (uint32_t y = 0; y < ch; y++)
            for (uint32_t x = 0; x < cw; x++)
                if (img[(y * cw + x) * 4 + 3] > 40) {
                    if ((int)x < ix0) ix0 = x;
                    if ((int)x > ix1) ix1 = x;
                    if ((int)y < iy0) iy0 = y;
                    if ((int)y > iy1) iy1 = y;
                }
        if (ix1 < 0) { free(img); continue; }
        int lx0 = ix0, ly0 = iy0, lx1 = ix1, ly1 = iy1;  // where the new label goes
        if (k->kind == D3_PLAIN) {
            for (uint32_t i = 0; i < cw * ch; i++) img[i * 4] = img[i * 4 + 1] = img[i * 4 + 2] = 16, img[i * 4 + 3] = 0;  // the bare letter (and its shadow) goes, colour too (its white showed at the new symbol's edges)
        } else {
            // body colour: the dark pixels of the button; label: the light ones inside it, painted with the body
            long sr = 0, sg = 0, sb = 0, n = 0;
            for (uint32_t i = 0; i < cw * ch; i++) {
                uint8_t *p = img + i * 4;
                if (p[3] > 220 && luma(p) > 30 && luma(p) < 110) { sr += p[0]; sg += p[1]; sb += p[2]; n++; }
            }
            if (!n) { free(img); continue; }
            uint8_t body[3] = {(uint8_t)(sr / n), (uint8_t)(sg / n), (uint8_t)(sb / n)};
            lx0 = cw; ly0 = ch; lx1 = -1; ly1 = -1;
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++) {
                    uint8_t *p = img + (y * cw + x) * 4;
                    if (p[3] > 200 && luma(p) > 150) {
                        p[0] = body[0]; p[1] = body[1]; p[2] = body[2];
                        if ((int)x < lx0) lx0 = x;
                        if ((int)x > lx1) lx1 = x;
                        if ((int)y < ly0) ly0 = y;
                        if ((int)y > ly1) ly1 = y;
                    }
                }
            if (lx1 < 0) { free(img); continue; }
            if (k->kind == D3_ROUND) {
                // the whole face of the disc in the body colour (the letter's dark outline went too), then a symbol
                // about half its size in the middle
                float cx = (ix0 + ix1) / 2.0f, cy = (iy0 + iy1) / 2.0f, rad = (ix1 - ix0 + iy1 - iy0) / 4.0f;
                for (uint32_t y = 0; y < ch; y++)
                    for (uint32_t x = 0; x < cw; x++) {
                        uint8_t *p = img + (y * cw + x) * 4;
                        if (hypotf(x + 0.5f - cx, y + 0.5f - cy) < rad * 0.74f) { p[0] = body[0]; p[1] = body[1]; p[2] = body[2]; }
                    }
                int r = (int)(rad * 0.46f);
                lx0 = (int)cx - r; lx1 = (int)cx + r; ly0 = (int)cy - r; ly1 = (int)cy + r;
            } else {  // shoulder: the new name as tall as the old one, as wide as the button allows
                int mid = (ix0 + ix1) / 2, half = (int)((ix1 - ix0) * 0.30f);
                lx0 = mid - half; lx1 = mid + half;
            }
        }
        d3_symbol(cv, k->sym, d3_label(k->text));
        int u0, v0, u1, v1;
        d3_ink(cv, &u0, &v0, &u1, &v1);
        // fit the ink into the label box, keeping its shape (centred)
        float bw = (float)(lx1 - lx0 + 1), bh = (float)(ly1 - ly0 + 1), iw = (float)(u1 - u0 + 1), ih = (float)(v1 - v0 + 1);
        float sc = fminf(bw / iw, bh / ih);
        float ox = lx0 + (bw - iw * sc) / 2, oy = ly0 + (bh - ih * sc) / 2;
        bool shadow = k->kind == D3_PLAIN;
        for (uint32_t y = 0; y < ch; y++)
            for (uint32_t x = 0; x < cw; x++) {
                float fx = (x - ox) / sc, fy = (y - oy) / sc;  // canvas units from the ink's corner
                float a = d3_cover(cv, u0 + fx, v0 + fy, 1 / sc, 1 / sc, 0, 0);
                float sh = shadow ? d3_cover(cv, u0 + (x - ox - 1) / sc, v0 + (y - oy - 1) / sc, 1 / sc, 1 / sc, 0, 0) : 0;
                uint8_t *p = img + (y * cw + x) * 4;
                if (shadow && sh > 0 && p[3] < sh * 200) { p[0] = p[1] = p[2] = 16; p[3] = (uint8_t)(sh * 200); }
                if (a > 0) {
                    // (the symbols under the skill buttons in the PlayStation colours, as the PS4 version shows them; the tester
                    // 2026-11-15. Names (L1, R2...) and the symbols on the round buttons stay white)
                    // (Xbox: the round buttons' letters in their colours too)
                    bool coloured = k->sym < D3_TEXT && (shadow || g_d3_label_xbox);
                    const uint8_t *col = coloured ? d3_sym_colour(k->sym) : (const uint8_t[3]){235, 235, 235};
                    for (int q = 0; q < 3; q++) p[q] = (uint8_t)(p[q] * (1 - a) + col[q] * a);
                    if (shadow) p[3] = (uint8_t)fmaxf(p[3], a * 255);
                }
            }
        for (uint32_t by = by0; by < by1; by++)  // encode the cell's blocks again
            for (uint32_t bx = bx0; bx < bx1; bx++) {
                uint8_t *b = blocks + (size_t)by * row + bx * 16;
                uint8_t a[16], px[16][4];
                for (int i = 0; i < 16; i++) {
                    const uint8_t *s = img + (((by - by0) * 4 + i / 4) * cw + (bx - bx0) * 4 + i % 4) * 4;
                    memcpy(px[i], s, 4);
                    a[i] = s[3];
                }
                bc4_encode(a, b);
                bc1_encode4(px, b + 8);
            }
        free(img);
        patched++;
    }
    if (w >= 2048 && h >= 128) d3_sideways_icons(blocks, row);  // (after the round buttons: it copies them)
    if (diagrams) d3_diagram_icons(blocks, row, lit);           // (likewise)
    static int logged;
    if (!logged++) LOG("[texpatch] Diablo III button prompts: %d glyphs drawn PlayStation style\n", patched);
    // keep the patched level 0 as RGBA: the smaller levels (the prompts are drawn small, from mip 1 / 2) are made
    // from it (texpatch_d3_buttons_mip)
    uint8_t *full = malloc((size_t)w * h * 4);
    if (!full) return;
    free(g_d3_full);
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++) {
            const uint8_t *b = blocks + (size_t)by * row + bx * 16;
            uint8_t a[16], px[16][4];
            bc4_decode(b, a);
            bc1_decode4(b + 8, px);
            for (int i = 0; i < 16; i++) {
                uint8_t *o = full + (((size_t)by * 4 + i / 4) * w + bx * 4 + i % 4) * 4;
                o[0] = px[i][0]; o[1] = px[i][1]; o[2] = px[i][2]; o[3] = a[i];
            }
        }
    g_d3_full = full;
    g_d3_w = w;
    g_d3_h = h;
}

// The prompt floating over things to use (Stash, Mailbox, banners, people) is a picture of its own, not the atlas: a
// 128x64 RGBA8 texture (known by its contents' hash in textures.c) with the four-circle diagram in its left half, the
// button's circle lit in yellow (the tester 2026-11-15: still the diagram after the atlas was redrawn). It becomes the
// PlayStation round button of the lit circle (right A = Cross, bottom B = Circle, top X = Triangle, left Y = Square),
// drawn here: a dark grey disc with a light rim and a white symbol. Every level (w: 128 at level 0).
void texpatch_d3_world_prompt(uint8_t *rgba, uint32_t w, uint32_t h, uint32_t row) {
    if (w < 8 || h < 4) return;
    float s = w / 128.0f;
    static const float pos[4][2] = {{51, 31}, {34, 48}, {34, 14}, {17, 31}};  // right, bottom, top, left (level 0)
    static const int sym_of[4] = {D3_CROSS, D3_CIRCLE, D3_TRIANGLE, D3_SQUARE};
    int lit = 0, best = -1;
    for (int k = 0; k < 4; k++) {
        int x = (int)(pos[k][0] * s), y = (int)(pos[k][1] * s);
        if (x >= (int)w || y >= (int)h) continue;
        const uint8_t *p = rgba + (size_t)y * row + x * 4;
        int v = p[3] ? p[0] + p[1] - p[2] : -1;  // (yellow: red and green high, blue low)
        if (v > best) best = v, lit = k;
    }
    static float cv[CV * CV];
    load_font_once();
    d3_symbol(cv, sym_of[lit], NULL);
    int u0, v0, u1, v1;
    d3_ink(cv, &u0, &v0, &u1, &v1);
    float cx = 34 * s, cy = 31 * s, rad = 24 * s;
    float sr = rad * 0.46f, iw = (float)(u1 - u0 + 1), ih = (float)(v1 - v0 + 1), sc = fminf(2 * sr / iw, 2 * sr / ih);
    float ox = cx - iw * sc / 2, oy = cy - ih * sc / 2;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = rgba + (size_t)y * row + x * 4;
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
            float cov = fminf(fmaxf(rad - d + 0.5f, 0), 1);  // (the disc, anti-aliased)
            float rim = cov * fminf(fmaxf(d - (rad - 2.5f * s) + 0.5f, 0), 1);
            float r = 58 + 62 * rim, g = 60 + 62 * rim, b = 66 + 62 * rim;
            float a = cov > 0 ? d3_cover(cv, u0 + (x - ox) / sc, v0 + (y - oy) / sc, 1 / sc, 1 / sc, 0, 0) : 0;
            // (the symbol in its PlayStation colour, as under the skills; the tester 2026-11-15)
            const uint8_t *c = d3_sym_colour(sym_of[lit]);
            p[0] = (uint8_t)(r * (1 - a) + c[0] * a);
            p[1] = (uint8_t)(g * (1 - a) + c[1] * a);
            p[2] = (uint8_t)(b * (1 - a) + c[2] * a);
            p[3] = (uint8_t)(cov * 255);
        }
    static int logged;
    if (!logged++) LOG("[texpatch] Diablo III world prompt: four-circle picture -> PlayStation button %d\n", sym_of[lit]);
}

// The redraw takes ~280 ms (the glyphs supersampled, the whole atlas decoded and encoded again), and the game loads
// the atlas right as the title screen's button is pressed: a visible freeze there (the tester 2026-11-15). The result is kept
// in cache/d3_atlas.bin (keyed by the game's atlas and this file's build) and later starts copy it.
// The four layouts (play / menus x PlayStation / Xbox), kept in memory: the atlas is drawn again each time a menu opens
// or closes. Filled from cache/d3_atlas*.bin at startup (texpatch_d3_preload, a thread of its own) so the first upload
// - right after the first button press on the title - is a copy instead of a file read (49 ms there, 2026-11-15).
// The cache files' version: was the build's date and time, so every new build drew the atlas again (~320 ms, at the
// first button press and again as the first level loads - the tester's "dip" right after a load, 2026-11-15). Bump this when
// what d3_buttons_draw / d3_cells / the colours make changes.
#define D3_ATLAS_VERSION "d3 atlas 6 (d-pad icons)"
typedef struct { char ver[32]; uint64_t in; uint32_t w, h, row, pad; } D3AtlasHdr;
static struct { uint64_t in; size_t nb, nf; uint8_t *blocks, *full; } d3_mem[4];
static pthread_mutex_t d3_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const d3_paths[4] = {"cache/d3_atlas.bin", "cache/d3_atlas_menu.bin", "cache/d3_atlas_xbox.bin",
                                        "cache/d3_atlas_xbox_menu.bin"};
static uint64_t g_d3_in;  // (texpatch_d3_buttons_mip's memory: the level 0 its levels are made from)

static void d3_mem_put(int mode, uint64_t in, const uint8_t *blocks, size_t nb, const uint8_t *full, size_t nf) {
    uint8_t *b = malloc(nb), *f = malloc(nf);
    if (!b || !f) { free(b); free(f); return; }
    memcpy(b, blocks, nb);
    memcpy(f, full, nf);
    pthread_mutex_lock(&d3_mem_lock);
    free(d3_mem[mode].blocks);
    free(d3_mem[mode].full);
    d3_mem[mode].blocks = b, d3_mem[mode].full = f;
    d3_mem[mode].in = in; d3_mem[mode].nb = nb; d3_mem[mode].nf = nf;
    pthread_mutex_unlock(&d3_mem_lock);
}

static void *d3_preload_thread(void *arg) {
    (void)arg;
    for (int mode = 0; mode < 4; mode++) {
        FILE *f = fopen(d3_paths[mode], "rb");
        if (!f) continue;
        D3AtlasHdr fh, want = {{0}};
        snprintf(want.ver, sizeof want.ver, "%s", D3_ATLAS_VERSION);
        if (fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(fh.ver, want.ver, sizeof want.ver) && fh.w && fh.h && fh.row &&
            fh.w <= 8192 && fh.h <= 8192) {
            size_t nb = (size_t)fh.row * (fh.h / 4), nf = (size_t)fh.w * fh.h * 4;
            uint8_t *b = malloc(nb), *full = malloc(nf);
            if (b && full && fread(b, 1, nb, f) == nb && fread(full, 1, nf, f) == nf) d3_mem_put(mode, fh.in, b, nb, full, nf);
            free(b);
            free(full);
        }
        fclose(f);
    }
    return NULL;
}

void texpatch_d3_preload(void) {
    static bool once;
    if (once) return;
    once = true;
    pthread_t th;
    if (pthread_create(&th, NULL, d3_preload_thread, NULL) == 0) pthread_detach(th);
}

void texpatch_d3_buttons(uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row, uint64_t content) {
    size_t nb = (size_t)row * (h / 4), nf = (size_t)w * h * 4;
    uint64_t in = 1469598103934665603ull;
    if (content) in = (in ^ content) * 1099511628211ull;
    else for (size_t i = 0; i < nb; i += 8) { uint64_t v; memcpy(&v, blocks + i, 8); in = (in ^ v) * 1099511628211ull; }
    g_d3_in = in;
    D3AtlasHdr hd = {{0}}, fh;
    snprintf(hd.ver, sizeof hd.ver, "%s", D3_ATLAS_VERSION);
    hd.in = in; hd.w = w; hd.h = h; hd.row = row;
    int mode = (g_d3_label_menu ? 1 : 0) + (g_d3_label_xbox ? 2 : 0);
    const char *path = d3_paths[mode];
    if (getenv("D3_DIAG_NUMBERS")) { d3_buttons_draw(blocks, w, h, row); return; }  // (test: never the caches)
    {
        uint8_t *full = malloc(nf);
        bool hit = false;
        pthread_mutex_lock(&d3_mem_lock);
        if (full && d3_mem[mode].blocks && d3_mem[mode].in == in && d3_mem[mode].nb == nb && d3_mem[mode].nf == nf) {
            memcpy(blocks, d3_mem[mode].blocks, nb);
            memcpy(full, d3_mem[mode].full, nf);
            hit = true;
        }
        pthread_mutex_unlock(&d3_mem_lock);
        if (hit) {
            free(g_d3_full);
            g_d3_full = full; g_d3_w = w; g_d3_h = h;
            return;
        }
        free(full);
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t *full = malloc(nf);
        bool ok = full && fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(&fh, &hd, sizeof hd) &&
                  fread(blocks, 1, nb, f) == nb && fread(full, 1, nf, f) == nf;
        fclose(f);
        if (ok) {
            free(g_d3_full);
            g_d3_full = full; g_d3_w = w; g_d3_h = h;
            LOG("[texpatch] Diablo III button prompts: from %s\n", path);
            goto keep;
        }
        free(full);  // (another game version or build: drawn again; blocks were only written on a full match)
    }
    d3_buttons_draw(blocks, w, h, row);
    if (g_d3_full && (f = fopen(path, "wb"))) {
        fwrite(&hd, sizeof hd, 1, f);
        fwrite(blocks, 1, nb, f);
        fwrite(g_d3_full, 1, nf, f);
        fclose(f);
    }
keep:
    if (g_d3_full) d3_mem_put(mode, in, blocks, nb, g_d3_full, nf);
}

// Level `lv` of the atlas: each glyph cell re-made by averaging the patched level 0 (box filter), re-encoded
static void d3_buttons_mip_draw(uint8_t *blocks, uint32_t lv, uint32_t w, uint32_t h, uint32_t row);
void texpatch_d3_buttons_mip(uint8_t *blocks, uint32_t lv, uint32_t w, uint32_t h, uint32_t row) {
    if (!g_d3_full || !lv || lv > 5) return;
    // (each layout's smaller levels kept too, by the level 0 they were made from: a menu opening redraws them all)
    static struct { uint64_t in; size_t n; uint8_t *data; } mip[4][6];
    int mode = (g_d3_label_menu ? 1 : 0) + (g_d3_label_xbox ? 2 : 0);
    size_t n = (size_t)row * ((h + 3) / 4);
    if (mip[mode][lv].data && mip[mode][lv].in == g_d3_in && mip[mode][lv].n == n) { memcpy(blocks, mip[mode][lv].data, n); return; }
    d3_buttons_mip_draw(blocks, lv, w, h, row);
    uint8_t *d = malloc(n);
    if (!d) return;
    memcpy(d, blocks, n);
    free(mip[mode][lv].data);
    mip[mode][lv].data = d, mip[mode][lv].in = g_d3_in, mip[mode][lv].n = n;
}
static void d3_buttons_mip_draw(uint8_t *blocks, uint32_t lv, uint32_t w, uint32_t h, uint32_t row) {
    if (!g_d3_full || !lv || lv > 5) return;
    uint32_t f = 1u << lv;
    enum { NCELLS = sizeof d3_cells / sizeof *d3_cells };
    const uint16_t *diag_x = d3_diag_x;  // (the face-button diagrams: d3_diagram_icons)
    enum { NDIAG = D3_NDIAG, NSIDE = sizeof d3_sideways / sizeof *d3_sideways };
    for (unsigned c = 0; c < NCELLS + NDIAG + NSIDE; c++) {
        D3Cell dk = {0};
        if (c >= NCELLS + NDIAG)
            dk = (D3Cell){d3_sideways[c - NCELLS - NDIAG].dx0, d3_sideways[c - NCELLS - NDIAG].dx1, 64, 128, 0, 0, NULL};
        else if (c >= NCELLS) dk = (D3Cell){diag_x[c - NCELLS], (uint16_t)(diag_x[c - NCELLS] + 55), 0, 64, 0, 0, NULL};
        const D3Cell *k = c < NCELLS ? &d3_cells[c] : &dk;
        uint32_t bx0 = (k->x0 / f) / 4, bx1 = ((k->x1 + f - 1) / f + 3) / 4, by0 = (k->y0 / f) / 4, by1 = ((k->y1 + f - 1) / f + 3) / 4;
        if (bx1 * 4 > ((w + 3) & ~3u) || by1 * 4 > ((h + 3) & ~3u)) continue;
        for (uint32_t by = by0; by < by1; by++)
            for (uint32_t bx = bx0; bx < bx1; bx++) {
                uint8_t a[16], px[16][4];
                for (int i = 0; i < 16; i++) {
                    uint32_t X = (bx * 4 + i % 4) * f, Y = (by * 4 + i / 4) * f, s[4] = {0};
                    uint32_t n = 0;
                    for (uint32_t yy = Y; yy < Y + f && yy < g_d3_h; yy++)
                        for (uint32_t xx = X; xx < X + f && xx < g_d3_w; xx++) {
                            const uint8_t *p = g_d3_full + ((size_t)yy * g_d3_w + xx) * 4;
                            for (int q = 0; q < 4; q++) s[q] += p[q];
                            n++;
                        }
                    for (int q = 0; q < 4; q++) px[i][q] = n ? (uint8_t)(s[q] / n) : 0;
                    a[i] = px[i][3];
                }
                uint8_t *b = blocks + (size_t)by * row + bx * 16;
                bc4_encode(a, b);
                bc1_encode4(px, b + 8);
            }
    }
}

// The loading screens' logo (runtime/ui/menu.mm loading_mark, the tester 2026-11-15 "put it in the game as part of the
// recomp"): the game's own logo, taken from the title screen's picture sheet as the game uploads it - the player's own
// copy, never shipped with the recomp. Its logo part (with the glow around it) is decoded from BC3 and written once as
// raw RGBA (u32 width, u32 height, pixels) to cache/d3_title_logo.rgba; from then on every loading screen has it.
volatile int g_d3_logo_ready;
void texpatch_d3_title_logo(const uint8_t *bc3, uint32_t w, uint32_t h, uint32_t row) {
    static const char *const path = "cache/d3_title_logo.rgba";
    enum { X0 = 64, Y0 = 64, X1 = 1856, Y1 = 1248 };  // (the logo and its glow, block-aligned; the sheet's top part)
    if (w != 2048 || h != 2048) return;
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    uint32_t ow = X1 - X0, oh = Y1 - Y0;
    uint8_t *px = malloc((size_t)ow * oh * 4);
    if (!px) return;
    for (uint32_t by = Y0 / 4; by < Y1 / 4; by++)
        for (uint32_t bx = X0 / 4; bx < X1 / 4; bx++) {
            const uint8_t *b = bc3 + (size_t)by * row + (size_t)bx * 16;
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
            for (int ch = 0; ch < 3; ch++)  // (BC3's colour block is always four colours)
                col[2][ch] = (uint8_t)((2 * col[0][ch] + col[1][ch]) / 3), col[3][ch] = (uint8_t)((col[0][ch] + 2 * col[1][ch]) / 3);
            uint32_t ci = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
            for (int i = 0; i < 16; i++) {
                uint8_t *o = px + (((size_t)(by * 4 + i / 4 - Y0)) * ow + (bx * 4 + i % 4 - X0)) * 4;
                const uint8_t *c = col[(ci >> (2 * i)) & 3];
                o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = al[(ai >> (3 * i)) & 7];
            }
        }
    mkdir("cache", 0755);
    if ((f = fopen(path, "wb"))) {
        uint32_t wh[2] = {ow, oh};
        fwrite(wh, 4, 2, f);
        fwrite(px, 1, (size_t)ow * oh * 4, f);
        fclose(f);
        g_d3_logo_ready = 1;
        LOG("[texpatch] Diablo III logo for the loading screens: %ux%u -> %s\n", ow, oh, path);
    }
    free(px);
}

// A logo carried inside the program file (the tester 2026-11-15: a private build with the logo in it, a shareable one
// without). The packing step appends to a copy of the program: the picture as raw RGBA (u32 width, u32 height,
// pixels), then a footer - u64 size of that block, then the 8 bytes "D3LOGO01". Windows runs such a file as usual.
uint8_t *texpatch_d3_embedded_logo(uint32_t *w, uint32_t *h) {
#ifdef _WIN32
    extern char *_pgmptr;
    const char *self = _pgmptr;
#else
    const char *self = NULL;
#endif
    if (!self || !*self) return NULL;
    FILE *f = fopen(self, "rb");
    if (!f) return NULL;
    uint8_t *px = NULL;
    uint64_t size = 0;
    char magic[8];
    if (fseek(f, -16, SEEK_END) == 0 && fread(&size, 8, 1, f) == 1 && fread(magic, 1, 8, f) == 8 &&
        !memcmp(magic, "D3LOGO01", 8) && size > 8 && size < (64u << 20) && fseek(f, -16 - (long)size, SEEK_END) == 0) {
        uint32_t wh[2];
        if (fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 4096 && wh[1] <= 4096 &&
            (uint64_t)wh[0] * wh[1] * 4 + 8 == size && (px = malloc((size_t)wh[0] * wh[1] * 4))) {
            if (fread(px, 1, (size_t)wh[0] * wh[1] * 4, f) == (size_t)wh[0] * wh[1] * 4) *w = wh[0], *h = wh[1];
            else free(px), px = NULL;
        }
    }
    fclose(f);
    return px;
}
