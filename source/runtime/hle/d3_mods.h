// SPDX-License-Identifier: GPL-2.0-only
// Diablo III mods: offline seasons, season buffs, cheats (runtime/hle/d3_mods.c; derived from d3hack, GPL-2.0)
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define D3_BUFF_COUNT 20
#define D3_BUFFS_THEME 0x80000000u  // (d3_buffs bit: the chosen season's own theme too)
// d3_cheats bits (the order of the Mods tab: d3_cheat_info)
enum {
    D3C_DIFFICULTY = 1u << 0, D3C_CUBE = 1u << 1, D3C_PORTAL = 1u << 2, D3C_COOLDOWNS = 1u << 3, D3C_CRAFT = 1u << 4,
    D3C_GEM100 = 1u << 5, D3C_GEMFAST = 1u << 6, D3C_GEM150 = 1u << 7, D3C_RESOURCE = 1u << 8, D3C_ORBS = 1u << 9,
    D3C_LEGENDARY = 1u << 10, D3C_EASYKILL = 1u << 11, D3C_REDDMG = 1u << 12, D3C_DROPANY = 1u << 13,
    D3C_LEGCHANCE = 1u << 14, D3C_ANYGEM = 1u << 15, D3C_AUTOPICKUP = 1u << 16, D3C_GOD = 1u << 17,
    D3C_DBGFLAGS = 1u << 18, D3C_EQUIPANY = 1u << 19, D3C_CHALLENGE = 1u << 20, D3C_RIFTCLOSE = 1u << 21,
    D3C_COSMETICS = 1u << 22, D3C_CRFARM = 1u << 23, D3C_KEEPSEASON = 1u << 24,
};
#define D3_CHEAT_COUNT 25
// d3_loot: legendary drops
enum { D3L_NORMAL, D3L_ANCIENT, D3L_PRIMAL, D3L_NO_PRIMAL, D3L_NO_ANCIENT, D3L_COUNT };

const char *d3_buff_label(int i);
uint32_t d3_season_theme(int season);
int d3_mods_season(void);
// cheat i (bit 1 << i): its Mods tab label and description
const char *d3_cheat_info(int i, const char **desc);
// the folder Challenge Rift offline reads its files from (d3hack's rift_data)
const char *d3_rift_dir(void);
// (GitHub #22) the multiplier / stash-size choices of the Mods tab
#define D3_MULT_COUNT 12
extern const int d3_mult_values[D3_MULT_COUNT];
#define D3_STASH_COUNT 21
#define D3_STASH_BASE 20  // (the stash a new account has; the game sells 20 more at a time up to 350)
#define D3_STASH_MAX (D3_STASH_BASE + 20 * (D3_STASH_COUNT - 1))
int d3_stash_choice(int i);  // i = 0 off, 1..20: the stash size in slots

#ifdef __cplusplus
}
#endif
