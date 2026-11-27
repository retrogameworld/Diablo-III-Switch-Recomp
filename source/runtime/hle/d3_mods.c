// SPDX-License-Identifier: GPL-2.0-only
// Diablo III mods (PC Settings > Mods, GitHub #4): offline seasons, season buffs / community events and a few cheats.
//
// Derived from d3hack (https://github.com/god-jester/d3hack, Copyright (C) god-jester and the d3hack contributors,
// GNU GPL version 2) through its port to the game's update 2.7.7 in the Android port "Diablo III Mobile"
// (https://github.com/Desm0nt/diablo3-port, dm/host/hle_d3_mods.c, GPL-2.0): the addresses (2.7.7, main build
// 2607a74f), the offline-season and community-event XVars and how they are set, and the code sites. Licence text:
// d3_mods.LICENSE next to this file. Because of this file the program as a whole is distributed under GPL-2.0.
//
// How it works here (a static recompilation, not the game's own ARM code): the code sites are compiled into the
// generated game code in both forms, the game's instruction and d3hack's (tools/gen.py with d3_gen_patch.py
// MOD_VARIANTS), and d3_mods_word picks one at run time; d3hack's hooks are function entries that ask d3_mods_fn first
// (MOD_ENTRIES), which may answer the function or run the game's own through call_orig; the XVars (the game's named
// settings: Season.Num / State / OverrideEnabled, CommunityEvent*) are set through the game's own setters (XVarUint32::Set
// main+0xa273c0, XVarBool::Set main+0xa26fc0, dirty flags 3) on the game's main thread at its controller poll
// (input.c fill_state -> d3_mods_pump), checked first (vtable, registered name), and set again every ~5 s: offline the
// game never loads the seasons / config files, so nothing else writes them.
//
// Settings (larecomp.ini): d3_season = 0 (off) or 14..40, d3_buffs = bit mask of k_buffs (bit 31: the season's own theme),
// d3_cheats = bit mask of D3C_*, d3_loot = D3L_*, d3_move / d3_atk = speed in percent. Everything is off by default.
// Changes apply after a restart.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../hle.h"
#include "../paths.h"
#include "../settings.h"
#include "d3_mods.h"
#include <time.h>

extern bool game_is_d3(void);
extern void guest_call(Cpu *c, uint64_t addr);

#define MAIN 0x7000000000ull
#define M8(off) (*(volatile uint8_t *)(uintptr_t)(MAIN + (off)))
#define M32(off) (*(volatile uint32_t *)(uintptr_t)(MAIN + (off)))
#define M64(off) (*(volatile uint64_t *)(uintptr_t)(MAIN + (off)))

// ---- XVars (main bss; vtable = *(GOT) + 0x10; name at +8; value at +0xb0)
enum { GOT_VT_BOOL = 0x115a388, GOT_VT_U32 = 0x115a398, F_BOOL_SET = 0xa26fc0, F_U32_SET = 0xa273c0,
       V_SEASON_NUM = 0x1a1bd78, V_SEASON_STATE = 0x1a1be30, V_SEASON_OVR = 0x1a1c1c8,
       V_EV_ENABLED = 0x18ffbe0, V_EV_SEASONONLY = 0x18ffc98, V_BUFF_START = 0x1771758, V_BUFF_END = 0x1771760,
       V_EGG_FLAG = 0x1771820 };
typedef struct { const char *key, *label; uint32_t var; const char *name; int community; } Buff;
static const Buff k_buffs[D3_BUFF_COUNT] = {
    {"goblins", "Double Treasure Goblins", 0x1900138, "CommunityEventDoubleTreasureGoblins", 1},
    {"bags", "Double Bounty Bags", 0x19001f0, "CommunityEventDoubleBountyBags", 1},
    {"grandeur", "Royal Grandeur (2 items fewer)", 0x19002a8, "CommunityEventRoyalGrandeur", 1},
    {"nightmares", "Legacy of Nightmares (2 items fewer)", 0x1900360, "CommunityEventLegacyOfNightmares", 1},
    {"triune", "Triune's Will", 0x1900418, "CommunityEventTriunesWill", 1},
    {"pandemonium", "Pandemonium", 0x19004d0, "CommunityEventPandemonium", 1},
    {"kanai", "Kanai's Powers", 0x1900588, "CommunityEventKanaiPowers", 1},
    {"tempests", "Trials of Tempests", 0x1900640, "CommunityEventTrialsOfTempests", 1},
    {"clones", "Shadow Clones", 0x19006f8, "CommunityEventShadowClones", 1},
    {"cube4", "Fourth Kanai's Cube Slot", 0x19007b0, "CommunityEventFourthKanaisCubeSlot", 1},
    {"ethereal", "Ethereal Items", 0x1900868, "CommunityEventEthrealItems", 1},  // (sic, the game's spelling)
    {"shards", "Soul Shards", 0x1900920, "CommunityEventSoulShards", 1},
    {"swarm", "Swarm Rifts", 0x19009d8, "CommunityEventSwarmRifts", 1},
    {"sanctified", "Sanctified Items", 0x1900a90, "CommunityEventSanctifiedItems", 1},
    {"alchemy", "Dark Alchemy", 0x1900b48, "CommunityEventDarkAlchemy", 1},
    {"nesting", "Nesting Portals", 0x18ff848, "CommunityEventNestingPortals", 1},
    {"keystones", "Double Rift Keystones", 0x18fffc8, "CommunityEventDoubleRiftKeystones", 1},
    {"bloodshards", "Double Blood Shards", 0x1900080, "CommunityEventDoubleBloodShards", 1},
    {"anniversary", "Anniversary Event", 0x18ffa70, "AnniversaryEnabled", 0},
    {"eggworld", "Whimsyshire / Cow Level Event", 0x18ffb28, "EasterEggWorldEnabled", 0},
};
// the theme each season had (public season history; seasons without a theme buff are not listed)
static const struct { int season; uint32_t mask; } k_themes[] = {
    {14, 1u << 0}, {15, 1u << 1}, {16, 1u << 2}, {17, 1u << 3}, {18, 1u << 4}, {19, 1u << 5}, {20, 1u << 6},
    {21, 1u << 7}, {22, 1u << 8 | 1u << 9}, {24, 1u << 10}, {25, 1u << 11}, {26, 1u << 12}, {27, 1u << 13}, {28, 1u << 14},
    {29, 1u << 15}, {30, 1u << 11}, {31, 1u << 6}, {32, 1u << 10}, {33, 1u << 8 | 1u << 9}, {34, 1u << 13},
    {35, 1u << 5}, {36, 1u << 11}, {37, 1u << 6},
};

const char *d3_buff_label(int i) { return i >= 0 && i < D3_BUFF_COUNT ? k_buffs[i].label : ""; }
uint32_t d3_season_theme(int season) {
    for (unsigned i = 0; i < sizeof k_themes / sizeof *k_themes; i++)
        if (k_themes[i].season == season) return k_themes[i].mask;
    return 0;
}

// ---- cheats: the Mods tab's rows (bit i of d3_cheats), d3hack's [rare_cheats] / [challenge_rifts]
static const struct { const char *label, *desc; } k_cheats[D3_CHEAT_COUNT] = {
    {"All Difficulties", "Every difficulty unlocked."},
    {"Free Kanai's Cube", "Kanai's Cube recipes take no materials."},
    {"Instant Town Portal", "Town Portal and the Book of Cain without the wait."},
    {"No Cooldowns", "Skills are ready again at once."},
    {"Instant Crafting", "Identify, craft, enchant and Kanai's Cube recipes finish at once."},
    {"Gem Upgrades Always Work", "Legendary gem upgrades always succeed."},
    {"Fast Gem Upgrades", "The legendary gem upgrade takes less time."},
    {"Gems To Level 150", "One upgrade takes a legendary gem to level 150."},
    {"Infinite Resource", "Skills cost no Fury, Mana, Spirit or other resource."},
    {"Extra Rift Orbs", "Elites in greater rifts drop many progress orbs."},
    {"Equip Same Legendary Twice", "Wear two copies of a legendary that normally allows one."},
    {"Easy Kill Damage", "Huge damage against what you hit (works with God Mode)."},
    {"Red Damage Numbers", "Floating damage numbers in red."},
    {"Drop Any Item", "Items that normally can't drop can (Staff of Herding parts and others)."},
    {"Guaranteed Legendaries", "Kadala and Kanai's Cube always give a legendary."},
    {"Socket Any Gem Anywhere", "Any gem goes into any socket."},
    {"Auto Pickup", "Loot - gear and gems too - is picked up by walking near it."},
    {"God Mode", "Your hero can't be hurt or stopped (also infinite resource)."},
    {"Debug Flags", "The developers' flags: every hit critical and crushing, item details shown, and more."},
    {"Equip In Any Slot", "Any item goes into any equipment slot."},
    {"Challenge Rift Offline", "The weekly Challenge Rift without servers. Needs the rift files from d3hack in the "
                               "challenge_rifts folder (see the README)."},
};
const char *d3_cheat_info(int i, const char **desc) {
    if (i < 0 || i >= D3_CHEAT_COUNT) return "";
    if (desc) *desc = k_cheats[i].desc;
    return k_cheats[i].label;
}
// instruction variants (tools/d3_gen_patch.py MOD_VARIANTS, the same sites): main offset, the variant word, the cheat
static const struct { uint32_t off, word, cheat; } k_sites[] = {
    {0x9e3350, 0x52800018, D3C_PORTAL},
    {0x7397d0, 0x1e2703e0, D3C_COOLDOWNS}, {0x7f8a60, 0x2a0003e0, D3C_COOLDOWNS}, {0x9bc63c, 0x1e2703e8, D3C_COOLDOWNS},
    {0x9bc928, 0x1e2703e8, D3C_COOLDOWNS},
    {0x20636c, 0xd503201f, D3C_CRAFT}, {0x40fa20, 0x11001508, D3C_CRAFT}, {0x1dcfa4, 0x11000508, D3C_CRAFT},
    {0x236d20, 0x52800008, D3C_CRAFT}, {0x236d24, 0x52800009, D3C_CRAFT}, {0x236d28, 0x5280000a, D3C_CRAFT},
    {0x236d30, 0x52800008, D3C_CRAFT},
    {0x6a265c, 0x1e2e1008, D3C_GEM100}, {0x349ba8, 0x1e2e1002, D3C_GEMFAST}, {0x7dfa58, 1, D3C_GEM150},
    {0x47ad7c, 0xd503201f, D3C_RESOURCE}, {0x844ce4, 0x52807ce3, D3C_ORBS}, {0x4fbae4, 0x14000017, D3C_LEGENDARY},
    {0x97f65c, 0x528f78e9, D3C_EASYKILL}, {0x97f660, 0x72abb289, D3C_EASYKILL}, {0x0bb550, 0x52801fe9, D3C_REDDMG},
    {0x504d78, 0x12800000, D3C_DROPANY},
    {0x88f92c, 0xd503201f, D3C_LEGCHANCE}, {0x88f934, 0xd503201f, D3C_LEGCHANCE}, {0x88f93c, 0x14000063, D3C_LEGCHANCE},
    {0x88fac4, 0xd503201f, D3C_LEGCHANCE}, {0x88fae0, 0x140000d4, D3C_LEGCHANCE},
    {0x4fa01c, 0x528000b9, D3C_ANYGEM}, {0x4fa024, 0x14000028, D3C_ANYGEM}, {0x4f99dc, 0xd503201f, D3C_AUTOPICKUP}, {0x4f99ec, 0x14000006, D3C_AUTOPICKUP},
};
enum { NSITES = sizeof k_sites / sizeof *k_sites };

// the settings as read at start (changes apply after a restart: the game builds its season state once)
static _Atomic int g_init;
static int g_season, g_loot, g_cr;
static uint32_t g_buffs, g_cheats;
static float g_move = 1, g_atk = 1;
static struct { uint64_t addr; uint32_t word; } g_words[NSITES];
static int g_nwords;
static void cr_load(void);
static void init(void) {
    if (atomic_load_explicit(&g_init, memory_order_acquire)) return;
    static atomic_flag busy = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&busy)) {
        while (!atomic_load_explicit(&g_init, memory_order_acquire)) {}
        return;
    }
    if (game_is_d3()) {
        g_season = g_settings.d3_season >= 1 && g_settings.d3_season <= 99 ? g_settings.d3_season : 0;
        g_buffs = g_settings.d3_buffs & ((1u << D3_BUFF_COUNT) - 1);
        if (g_settings.d3_buffs & D3_BUFFS_THEME) g_buffs |= d3_season_theme(g_season);
        g_cheats = g_settings.d3_cheats & ((1u << D3_CHEAT_COUNT) - 1);
        if (g_cheats & D3C_GOD) g_cheats |= D3C_RESOURCE;  // (d3hack: super god mode implies infinite MP)
        g_loot = g_settings.d3_loot > 0 && g_settings.d3_loot < D3L_COUNT ? g_settings.d3_loot : 0;
        if (g_settings.d3_move >= 10 && g_settings.d3_move <= 1000) g_move = g_settings.d3_move / 100.0f;
        if (g_settings.d3_atk >= 10 && g_settings.d3_atk <= 1000) g_atk = g_settings.d3_atk / 100.0f;
        for (int i = 0; i < NSITES; i++)
            if (g_cheats & k_sites[i].cheat) g_words[g_nwords].addr = MAIN + k_sites[i].off, g_words[g_nwords++].word = k_sites[i].word;
        if (g_cheats & D3C_CHALLENGE) cr_load();
        if (g_season || g_buffs || g_cheats || g_loot || g_move != 1 || g_atk != 1)
            LOG("[d3mods] season %d, buffs %#x, cheats %#x (%d code sites), legendary drops %d, speed x%.2f, attack x%.2f, "
                "Challenge Rift %s (GPL-2.0 code from d3hack / Diablo III Mobile)\n", g_season, g_buffs, g_cheats, g_nwords,
                g_loot, g_move, g_atk, g_cr ? "on" : "off");
    }
    atomic_store_explicit(&g_init, 1, memory_order_release);
}
int d3_mods_season(void) { init(); return g_season; }
static bool cheat(uint32_t bit) { init(); return (g_cheats & bit) != 0; }

// The generated code's instruction variants (tools/gen.py, d3_gen_patch.py MOD_VARIANTS): the word to run instead, 0 = the
// game's own instruction.
uint32_t d3_mods_word(uint64_t addr) {
    init();
    for (int i = 0; i < g_nwords; i++)
        if (g_words[i].addr == addr) return g_words[i].word;
    return 0;
}

// ---- hooks: a game function's entry asks d3_mods_fn first (tools/d3_gen_patch.py MOD_ENTRIES); a hook that wraps the
// game's function runs it through call_orig, whose next entry is let through
static __thread uint64_t t_pass;
static void call_orig(Cpu *c, uint64_t fn) { t_pass = fn; guest_call(c, fn); }
static float s_get(const Cpu *c, int r) { uint32_t u = (uint32_t)c->v[r]; float f; memcpy(&f, &u, 4); return f; }
static void s_set(Cpu *c, int r, float f) { uint32_t u; memcpy(&u, &f, 4); c->v[r] = u; }
static uint64_t gcall3(Cpu *c, uint32_t fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2;
    guest_call(c, MAIN + fn);
    return c->x[0];
}
// game functions (2.7.7; d3hack's 2.7.6 address in Diablo III Mobile's table)
enum { F_MOVESPEED = 0x0c8000, F_ATKSPEED = 0x6cc700, F_INSLOT = 0x489280, F_ANCIENT = 0x88dee0, F_INITWORLD = 0x8128a0,
       F_SRVLOCAL = 0x4c7e80, F_STARTUP = 0x4c7df0, F_PLFIRST = 0x51f8c0, F_PLNEXT = 0x51f970, F_ACDGET = 0x669cb0,
       F_ACTORGET = 0x86e070, F_ACTORFLAG = 0x86f940, F_ATTRSETI = 0x46fc00, F_ATTRSETF = 0x46fb80,
       O_APPGLOBALS = 0x1905610, GOT_APPGLOBALS = 0x114a840,
       F_PUBFILE = 0x066b10, F_PUBFILE_DONE = 0x070850, F_CR_DATA = 0x185f70, F_CR_FAILED = 0x185aa0, F_CR_SAVE = 0x04edc0,
       F_CR_GRANT = 0x7764e0, F_SEASON_NUM = 0x72e090, F_IS_SEASONAL = 0x4c8df0 };
// attribute ids (d3hack attributes.hpp, checked for 2.7.7 by Diablo III Mobile)
enum { A_INVULNERABLE = 0x69, A_APS_TOTAL = 0xc9, A_IMMUNE_KNOCKBACK = 0x12d, A_STUN_IMMUNE = 0x134, A_FREEZE_IMMUNE = 0x13a,
       A_SLOWDOWN_IMMUNE = 0x14b, A_GETHIT_IMMUNE = 0x14c, A_FEAR_IMMUNE = 0x231, A_IMMUNE_CHARM = 0x2c7, A_IMMUNE_BLIND = 0x2c8,
       A_GOD = 0x44d, A_ITEMS_INDESTRUCTIBLE = 0x4c0, A_RECEIVED_SEASONAL_LEGENDARY = 0x519 };
// LootSpecifier as LootRollForAncientLegendary reads it (Diablo III Mobile: d3hack's struct is 4 bytes off here)
enum { LS_CRAFTER_CHANCE = 0x64, LS_NO_ANCIENT = 0x68, LS_NO_PRIMAL = 0x6c, LS_ANCIENT_RANK = 0xd8 };

// d3hack _lobby.hpp FlagsAreFun
static void flags_are_fun(void) {
    if (M64(GOT_APPGLOBALS) != MAIN + O_APPGLOBALS) return;
    volatile uint32_t *f = (volatile uint32_t *)(uintptr_t)(MAIN + O_APPGLOBALS + 0x40);  // dwDebugFlags 1..3
    f[0] |= 1u << 0x6 | 1u << 0xe | 1u << 0xf | 1u << 0x17 | 1u << 0x19;  // all hits crit, all skills, item affixes / attributes, get-hit
    f[1] |= 1u << 0xd | 1u << 0x3 | 1u << 0x14 | 1u << 0x16 | 1u << 0x15;  // overkill, crushing blow, floating numbers, class
    f[2] |= 1u << 0x5 | 1u << 0x4 | 1u << 0x0 | 1u << 0x1;  // ancient rolls succeed / primal, no low-health glow, collector's
}
// d3hack _lobby.hpp EnableGod: every player's actor
static void enable_god(Cpu *c) {
    static const uint32_t attrs[] = {A_GOD, A_INVULNERABLE, A_ITEMS_INDESTRUCTIBLE, A_IMMUNE_KNOCKBACK, A_IMMUNE_BLIND,
                                     A_IMMUNE_CHARM, A_STUN_IMMUNE, A_FEAR_IMMUNE, A_SLOWDOWN_IMMUNE, A_FREEZE_IMMUNE,
                                     A_GETHIT_IMMUNE};
    uint64_t pl = gcall3(c, F_PLFIRST, 0, 0, 0);
    for (int n = 0; pl && n < 16; n++, pl = gcall3(c, F_PLNEXT, pl, 0, 0)) {
        int32_t id = (int32_t)LD32(pl + 4);
        if (id == -1) continue;
        uint64_t acd = gcall3(c, F_ACDGET, (uint32_t)id, 0, 0);
        if (!acd) continue;
        uint64_t actor = gcall3(c, F_ACTORGET, LD32(acd + 0x14), 0, 0);
        if (!actor) continue;
        uint32_t aid = LD32(actor);
        for (int bit = 0; bit < 3; bit++) gcall3(c, F_ACTORFLAG, aid, bit == 2 ? 4 : bit, 1);  // god, easy kill, no collision
        for (unsigned i = 0; i < sizeof attrs / sizeof *attrs; i++) gcall3(c, F_ATTRSETI, acd, attrs[i] | 0xfffff000u, 1);
    }
}
static int cr_hook(Cpu *c, uint64_t addr);

// A whole function answered here: returns 1 = handled (the game function returns at once with x0 / s0 as set).
int d3_mods_fn(Cpu *c, uint64_t addr) {
    if (t_pass == addr) { t_pass = 0; return 0; }  // (call_orig: the game's own function)
    init();
    switch (addr) {
    case MAIN + F_MOVESPEED:  // d3hack MoveSpeed: float CPlayerGetMoveSpeedForStickInput(ACD *, Player *, SNO, float)
        if (g_move == 1) return 0;
        call_orig(c, addr);
        if (getenv("D3_MODS_LOG")) { static int n; if (n++ < 8) LOG("[d3mods] move speed %.3f -> x%.2f\n", s_get(c, 0), g_move); }
        s_set(c, 0, s_get(c, 0) * g_move);
        return 1;
    case MAIN + F_ATKSPEED: {  // d3hack AttackSpeed: PowerGetFormulaValueAtLevel(group, ACD *, SNO power, formula, size, level)
        if (g_atk == 1 || (uint32_t)c->x[2] != 0x50860) return 0;  // (the power whose formula is the attack speed)
        uint64_t acd = c->x[1];
        call_orig(c, addr);
        float r = s_get(c, 0);
        if (acd) { s_set(c, 0, 3.0f); gcall3(c, F_ATTRSETF, acd, A_APS_TOTAL, 0); }
        s_set(c, 0, r * g_atk);
        return 1;
    }
    case MAIN + F_INSLOT:  // d3hack EquipAny: ACDInventoryItemAllowedInSlot -> no error
        if (!cheat(D3C_EQUIPANY)) return 0;
        call_orig(c, addr);
        c->x[0] = 0;
        return 1;
    case MAIN + F_ANCIENT: {  // LootRollForAncientLegendary(LootSpecifier *, ACDID looter)
        if (!g_loot) return 0;
        uint64_t spec = c->x[0];
        if (g_loot == D3L_NO_PRIMAL || g_loot == D3L_NO_ANCIENT) {  // the game's own roll, with its "no ancient / primal" flag set
            ST32(spec + (g_loot == D3L_NO_ANCIENT ? LS_NO_ANCIENT : LS_NO_PRIMAL), 1);
            return 0;
        }
        uint32_t looter = (uint32_t)c->x[1];  // d3hack ForceAncient + AugmentSpecifier: the rank written, no roll
        ST32(spec + LS_CRAFTER_CHANCE, 0);
        ST32(spec + LS_NO_ANCIENT, 0);
        ST32(spec + LS_ANCIENT_RANK, g_loot == D3L_PRIMAL ? 2 : 1);
        uint64_t acd = gcall3(c, F_ACDGET, looter, 0, 0);
        if (acd) gcall3(c, F_ATTRSETI, acd, A_RECEIVED_SEASONAL_LEGENDARY, 0);
        return 1;
    }
    case MAIN + F_INITWORLD:  // d3hack sInitializeWorld hook: after the world is set up, on the local server
        if (!(g_cheats & (D3C_GOD | D3C_DBGFLAGS))) return 0;
        call_orig(c, addr);
        if (gcall3(c, F_SRVLOCAL, 0, 0, 0) & 1 && !(gcall3(c, F_STARTUP, 0, 0, 0) & 1)) {
            if (g_cheats & D3C_DBGFLAGS) flags_are_fun();
            if (g_cheats & D3C_GOD) enable_god(c);
            static int n;
            if (n++ < 4) LOG("[d3mods] world set up:%s%s\n", g_cheats & D3C_GOD ? " god mode" : "", g_cheats & D3C_DBGFLAGS ? " debug flags" : "");
        }
        return 1;
    case 0x7000056b10ull:  // IsSignedInOnline -> 1 (hero create refuses a seasonal hero offline otherwise)
        if (!g_season && !g_cr) return 0;
        c->x[0] = 1;
        return 1;
    case MAIN + F_PUBFILE: case MAIN + F_CR_DATA: case MAIN + F_CR_FAILED: case MAIN + F_CR_SAVE: case MAIN + F_CR_GRANT:
        return g_cr ? cr_hook(c, addr) : 0;
    case 0x700005ff00ull:  // IsSeasonsInitialized -> 1 (d3hack PatchDynamicSeasonal)
    case 0x70001b0600ull:  // the mailbox's lobby check -> 1 (a hero's items come by mail when its season is swapped)
        if (!g_season) return 0;
        c->x[0] = 1;
        return 1;
    case 0x70001bf5f0ull:  // ValidateHeroForPartyMember -> 0, no error
        if (!g_season) return 0;
        c->x[0] = 0;
        return 1;
    case 0x70005282b0ull:  // PlayerCanPlayHandicap -> 1: every difficulty unlocked
        if (!cheat(D3C_DIFFICULTY)) return 0;
        c->x[0] = 1;
        return 1;
    case 0x70004f9820ull:  // "is this ground item picked up by walking over it" (item ACD) -> 1. The instruction variants
        // at 0x4f99dc / 0x4f99ec only reached the end of it: gear and gems left earlier, at their item type's own "auto
        // pickup" flag (main+0x4f9944), so nothing but gold and materials was picked up (GitHub #15)
        if (!cheat(D3C_AUTOPICKUP)) return 0;
        if (getenv("D3_MODS_LOG")) { static int n; if (n++ < 20) LOG("[d3mods] auto pickup: item %#x\n", LD32(c->x[0] + 0x3c)); }
        c->x[0] = 1;
        return 1;
    case 0x7000181060ull:  // Game Settings' "is this difficulty locked" (difficulty, player) -> 0: the menu let Torment
        if (!cheat(D3C_DIFFICULTY)) return 0;  // VII-XVI stay "locked until your hero is level 70" (GitHub #15)
        c->x[0] = 0;
        return 1;
    case 0x70008875d0ull:  // Kanai's Cube material consumption -> nothing taken
        if (!cheat(D3C_CUBE)) return 0;
        return 1;
    }
    return 0;
}
// A text the game's string tables don't have (main+0x6f1518, tools/d3_gen_patch.py #23): the game prints
// "!!Missing!! - <id>:<key>". The offline Anniversary event's buffs have no names in this version (GitHub #15:
// "!!Missing!! - 77545:Anniversary_Buff_EXPMF_11" under Temporary Effects): a name made from the key instead -
// underscores to spaces, a trailing number dropped. Args as the snprintf call: x0 buffer, x1 size, x4 id, x5 key.
int d3_missing_text(Cpu *c) {
    char *buf = (char *)(uintptr_t)c->x[0];
    size_t size = (size_t)c->x[1];
    const char *key = (const char *)(uintptr_t)c->x[5];
    if (!buf || size < 2 || !key || c->x[5] < 0x10000 || getenv("D3_MISSING_TEXT")) return 0;
    static const struct { const char *prefix, *name; } k_names[] = {
        {"Anniversary_Buff_EXPMF", "Anniversary: Experience and Magic Find"},
        {"Anniversary_Buff", "Anniversary Buff"},
    };
    for (unsigned i = 0; i < sizeof k_names / sizeof *k_names; i++)
        if (!strncmp(key, k_names[i].prefix, strlen(k_names[i].prefix))) {
            c->x[0] = (uint64_t)snprintf(buf, size, "%s", k_names[i].name);
            return 1;
        }
    size_t n = 0;
    for (const char *p = key; *p && n + 1 < size; p++) buf[n++] = *p == '_' ? ' ' : *p;
    while (n && buf[n - 1] >= '0' && buf[n - 1] <= '9') n--;  // (a trailing level number: "..._11")
    while (n && buf[n - 1] == ' ') n--;
    if (!n) return 0;
    buf[n] = 0;
    c->x[0] = n;
    return 1;
}

// the hero-create season confirmation callback (main+0x351ccc b.ne -> mov w1,#1): 1 = take it as "seasonal: yes"
int d3_mods_season_confirm(Cpu *c) {
    init();
    if (!g_season) return 0;
    c->x[1] = 1;
    return 1;
}
// SetGameParamsForHero (main+0x1bd140): the season of a hero without data - ours when a season is on
uint32_t d3_mods_season_created(uint32_t v) { init(); return g_season ? (uint32_t)g_season : v; }

// ---- Challenge Rift offline (d3hack [challenge_rifts], as Diablo III Mobile does it for 2.7.7)
// The game asks StorageGetPublisherFile (main+0x66b10) for "challengerift_config.dat", then "challengerift_<n % 10>.dat",
// and parses them itself. Here both are answered at once through the storage layer's own "file arrived" entry
// (main+0x70850) from d3hack's dumps (examples/config/d3hack-nx/rift_data, not shipped with this program: the player puts
// them in the challenge_rifts folder). The config's challenge number is the week number (a new one every Thursday 00:00
// UTC), start 0, end 0x7fffffff, so the game's own rules give one reward cache a week; the rift is file week % count.
// The reward's season tag is set as d3hack does (the game sets it only with the lobby connected).
typedef struct { const uint8_t *p; uint64_t size, cap; uint8_t sso[16]; } BlzStr;  // blz::string
enum { CD_BITS = 0x10, CD_START = 0x18, CD_NUMBER = 0x28, CD_END = 0x30, RW_BITS = 0x10, RW_CHALLENGE = 0x30, RW_SEASON = 0x38,
       PL_REWARDS_END = 0xdd88, PL_REWARDS_HEAD = 0xdd90, NODE_NEXT = 0x8, NODE_SEASON = 0x48, V_CR_ENABLED = 0x1a1c4a8,
       GOT_CR_ENABLED = 0x1158fd8, MAX_RIFTS = 100 };
static struct { uint8_t *p; uint32_t n; } g_rift_cfg, g_rifts[MAX_RIFTS];
static int g_nrifts;
static uint8_t g_cr_cfg[64];
static BlzStr g_cr_cfg_str, g_cr_dat_str;  // (what the game's parsers read: never freed)
static int32_t g_cr_result;
static uint64_t g_cr_answer[2];
static uint64_t g_cr_week;

const char *d3_rift_dir(void) {
    static char dir[1200];
    const char *e = getenv("D3_RIFT_DIR");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else snprintf(dir, sizeof dir, "%s/challenge_rifts", rt_data_root());
    return dir;
}
static uint64_t pb_varint(const uint8_t *b, size_t n, size_t *i, int *ok) {  // d3hack debug.hpp ReadVarint
    uint64_t v = 0;
    for (unsigned sh = 0; *i < n && sh < 64; sh += 7) {
        uint8_t ch = b[(*i)++];
        v |= (uint64_t)(ch & 0x7f) << sh;
        if (!(ch & 0x80)) return v;
    }
    *ok = 0;
    return 0;
}
// a protobuf message's top-level fields (d3hack ParseWeeklyChallengeTopLevel, generalised): val[f] = a varint / fixed32
// value or a length; returns bit f for each field f < nf seen, bit 31 when well formed
static uint32_t pb_top(const uint8_t *b, size_t n, uint64_t *val, unsigned nf) {
    uint32_t seen = 0;
    size_t i = 0;
    int ok = 1;
    while (i < n) {
        uint64_t tag = pb_varint(b, n, &i, &ok), v = 0;
        if (!ok) return 0;
        unsigned f = (unsigned)(tag >> 3), w = (unsigned)(tag & 7);
        if (w == 0) { v = pb_varint(b, n, &i, &ok); if (!ok) return 0; }
        else if (w == 1) { if (n - i < 8) return 0; i += 8; }
        else if (w == 5) { if (n - i < 4) return 0; uint32_t x; memcpy(&x, b + i, 4); v = x; i += 4; }
        else if (w == 2) { v = pb_varint(b, n, &i, &ok); if (!ok || v > n - i) return 0; i += (size_t)v; }
        else return 0;
        if (f < nf && f < 31) { seen |= 1u << f; if (val) val[f] = v; }
    }
    return seen | 1u << 31;
}
static size_t pb_put(uint8_t *o, uint32_t field, uint64_t v) {  // a varint field
    size_t n = 0;
    o[n++] = (uint8_t)(field << 3);
    do { uint8_t ch = v & 0x7f; v >>= 7; o[n++] = (uint8_t)(ch | (v ? 0x80 : 0)); } while (v);
    return n;
}
static uint8_t *read_all(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *p = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        long sz = ftell(f);
        if (sz > 0 && sz < (1 << 20) && !fseek(f, 0, SEEK_SET) && (p = malloc((size_t)sz)) && fread(p, 1, (size_t)sz, f) != (size_t)sz)
            free(p), p = NULL;
        if (p) *n = (uint32_t)sz;
    }
    fclose(f);
    return p;
}
static void cr_load(void) {
    char path[1300];
    const char *dir = d3_rift_dir();
    snprintf(path, sizeof path, "%s/challengerift_config.dat", dir);
    uint64_t f[7] = {0};
    if (!(g_rift_cfg.p = read_all(path, &g_rift_cfg.n)) || (pb_top(g_rift_cfg.p, g_rift_cfg.n, f, 7) & 0x22) != 0x22) {
        LOG("[d3mods] Challenge Rift offline: no usable %s - off (the files come from d3hack's rift_data folder)\n", path);
        return;
    }
    for (g_nrifts = 0; g_nrifts < MAX_RIFTS; g_nrifts++) {
        snprintf(path, sizeof path, "%s/challengerift_%02d.dat", dir, g_nrifts);
        uint32_t n = 0;
        uint8_t *p = read_all(path, &n);
        if (!p) break;
        if ((pb_top(p, n, NULL, 5) & 0xe) != 0xe) {
            LOG("[d3mods] Challenge Rift offline: %s is not a rift file - the ones before it are used\n", path);
            free(p);
            break;
        }
        g_rifts[g_nrifts].p = p, g_rifts[g_nrifts].n = n;
    }
    if (!g_nrifts) { LOG("[d3mods] Challenge Rift offline: no challengerift_00.dat in %s - off\n", dir); return; }
    g_cr = 1;
}
static unsigned cr_pick(void) { return (unsigned)(g_cr_week % (uint64_t)g_nrifts); }
static int cr_hook(Cpu *c, uint64_t addr) {
    switch (addr) {
    case MAIN + F_PUBFILE: {  // StorageGetPublisherFile(int user, const char *name, callback **)
        const char *name = (const char *)(uintptr_t)c->x[1];
        uint64_t cb = c->x[2];
        int cfg = name && strstr(name, "challengerift_config.dat"), dat = !cfg && name && strstr(name, "challengerift_");
        if ((!cfg && !dat) || !cb || !LD64(cb)) return 0;  // (other files: the game's own way)
        if (cfg || !g_cr_week) g_cr_week = (uint64_t)time(NULL) / 604800u;
        BlzStr *s;
        if (cfg) {
            uint64_t f[7] = {0};
            uint32_t seen = pb_top(g_rift_cfg.p, g_rift_cfg.n, f, 7);
            size_t n = pb_put(g_cr_cfg, 1, g_cr_week);  // challenge_number
            n += pb_put(g_cr_cfg + n, 2, 0);            // challenge_start_unix_time
            n += pb_put(g_cr_cfg + n, 3, 0);            // challenge_last_broadcast_unix_time
            n += pb_put(g_cr_cfg + n, 4, 0x7fffffff);   // challenge_end_unix_time_console
            n += pb_put(g_cr_cfg + n, 5, f[5]);         // challenge_hash
            if (seen >> 6 & 1) { g_cr_cfg[n++] = 6 << 3 | 5; uint32_t x = (uint32_t)f[6]; memcpy(g_cr_cfg + n, &x, 4); n += 4; }  // orb multiplier
            g_cr_cfg_str.p = g_cr_cfg, g_cr_cfg_str.size = g_cr_cfg_str.cap = n, s = &g_cr_cfg_str;
            LOG("[d3mods] challenge rift: config answered: challenge %llu (this week), rift file %02u of %d\n",
                (unsigned long long)g_cr_week, cr_pick(), g_nrifts);
        } else {
            unsigned k = cr_pick();
            g_cr_dat_str.p = g_rifts[k].p, g_cr_dat_str.size = g_cr_dat_str.cap = g_rifts[k].n, s = &g_cr_dat_str;
            LOG("[d3mods] challenge rift: \"%s\" answered with challengerift_%02u.dat (%u bytes)\n", name, k, g_rifts[k].n);
        }
        g_cr_answer[0] = (uint64_t)(uintptr_t)s, g_cr_answer[1] = 0, g_cr_result = 0;  // (STORAGE_SUCCESS; a static string)
        gcall3(c, F_PUBFILE_DONE, cb, (uint64_t)(uintptr_t)&g_cr_result, (uint64_t)(uintptr_t)g_cr_answer);
        return 1;
    }
    case MAIN + F_CR_DATA: {  // sOnGetChallengeRiftData(bind, StorageResult *, ChallengeData *, WeeklyChallengeData *): log
        uint64_t res = c->x[1], conf = c->x[2], data = c->x[3];
        int32_t r = res ? (int32_t)LD32(res) : -1;
        if (r || !conf || !data) LOG("[d3mods] challenge rift: the data callback got result %d\n", r);
        else LOG("[d3mods] challenge rift: data ok: challenge %u, config bits %#x, rift bits %#x\n", LD32(conf + CD_NUMBER),
                 LD32(conf + CD_BITS), LD32(data + CD_BITS));
        return 0;
    }
    case MAIN + F_CR_FAILED: {  // the Challenge Rift screen's error text: log
        const char *e = (const char *)(uintptr_t)c->x[0];
        LOG("[d3mods] challenge rift: the game refused it: %.120s\n", e && c->x[0] > 0x10000 ? e : "(none)");
        return 0;
    }
    case MAIN + F_CR_SAVE: {  // ChallengeRiftRewardEarned_SaveToProfile(int user, reward *): the season tag
        uint64_t reward = c->x[1];
        if (!reward) return 0;
        uint32_t user = (uint32_t)c->x[0];
        uint32_t season = (uint32_t)gcall3(c, F_SEASON_NUM, 0, 0, 0);
        if (season) { ST32(reward + RW_BITS, LD32(reward + RW_BITS) | 8u); ST32(reward + RW_SEASON, season); }
        c->x[0] = user, c->x[1] = reward;
        LOG("[d3mods] challenge rift: reward of challenge %u saved, season %u\n", LD32(reward + RW_CHALLENGE), season);
        return 0;
    }
    case MAIN + F_CR_GRANT: {  // ChallengeRiftGrantRewardToPlayerIfEligible(Player *): an untagged reward gets the season
        uint64_t player = c->x[0];
        if (!player) return 0;
        uint32_t season = (uint32_t)gcall3(c, F_SEASON_NUM, 0, 0, 0);
        uint64_t end = player + PL_REWARDS_END, node = LD64(player + PL_REWARDS_HEAD), untagged = 0;
        int n = 0, current = 0;
        for (; node && node != end && n < 4096; node = LD64(node + NODE_NEXT), n++) {
            uint32_t earned = LD32(node + NODE_SEASON);
            if (season && earned == season) { current = 1; break; }
            if (!earned && !untagged) untagged = node;
        }
        if (season && n < 4096 && !current && untagged) ST32(untagged + NODE_SEASON, season);
        c->x[0] = player;
        return 0;
    }
    }
    return 0;
}

// ---- XVars, on the game's main thread (input.c fill_state)
static bool xvar_ok(uint32_t var, uint32_t vt_got, const char *name) {
    uint64_t vt = M64(var), want = M64(vt_got) + 0x10;
    if (vt != want) return false;  // (not built yet, or not this class)
    return !strncmp((const char *)(uintptr_t)(MAIN + var + 8), name, 0x40);
}
static void call_set(Cpu *c, uint64_t fn, uint32_t var, uint32_t v) {
    Cpu save = *c;
    c->x[0] = MAIN + var, c->x[1] = v, c->x[2] = 3;
    guest_call(c, MAIN + fn);
    *c = save;
}
static void set_bool(Cpu *c, uint32_t var, int v) { if ((M8(var + 0xb0) & 1) != (v & 1)) call_set(c, F_BOOL_SET, var, (uint32_t)v); }
static void set_u32(Cpu *c, uint32_t var, uint32_t v) { if (M32(var + 0xb0) != v) call_set(c, F_U32_SET, var, v); }

void d3_mods_pump(Cpu *c) {
    init();
    if (!g_season && !g_buffs && !g_cr) return;
    static int state;  // 0 not checked yet, 1 ok, -1 refused
    static unsigned polls;
    if (state < 0) return;
    if (state == 0) {
        if (++polls < 30) return;  // (the game's static constructors build the XVars early; a moment after the first polls)
        bool ok = M32(F_BOOL_SET) == 0xf81d0ff5u && M32(F_U32_SET) == 0xf81d0ff5u;
        if (ok && g_season)
            ok = xvar_ok(V_SEASON_NUM, GOT_VT_U32, "Num") && xvar_ok(V_SEASON_STATE, GOT_VT_U32, "State") &&
                 xvar_ok(V_SEASON_OVR, GOT_VT_BOOL, "OverrideEnabled");
        for (int i = 0; ok && i < D3_BUFF_COUNT; i++)
            if (g_buffs >> i & 1) ok = xvar_ok(k_buffs[i].var, GOT_VT_BOOL, k_buffs[i].name);
        if (ok && g_buffs)
            ok = xvar_ok(V_EV_ENABLED, GOT_VT_BOOL, "CommunityEventEnabled") &&
                 xvar_ok(V_EV_SEASONONLY, GOT_VT_BOOL, "CommunityEventSeasonOnly");
        if (ok && g_cr) ok = M64(GOT_CR_ENABLED) == MAIN + V_CR_ENABLED && xvar_ok(V_CR_ENABLED, GOT_VT_BOOL, "ChallengeRiftEnabled");
        if (!ok) {
            if (polls < 600) return;  // (try for ~10 s)
            LOG("[d3mods] the game's season XVars are not where they were expected: season / buffs off\n");
            state = -1;
            return;
        }
        state = 1, polls = 0;
        LOG("[d3mods] XVars found: season %d, buffs %#x, Challenge Rift %d on\n", g_season, g_buffs, g_cr);
    } else if (++polls % 300) {
        return;  // (~5 s at 60 polls a second)
    }
    if (g_cr) set_bool(c, V_CR_ENABLED, 1);  // (d3hack PatchBase: XVarBool_Set(&g_varChallengeRiftEnabled, true, 3))
    if (g_season) {
        set_u32(c, V_SEASON_NUM, (uint32_t)g_season);
        set_u32(c, V_SEASON_STATE, 1);  // (in progress)
        set_bool(c, V_SEASON_OVR, 1);
    }
    if (g_buffs) {
        bool community = false;
        for (int i = 0; i < D3_BUFF_COUNT; i++)
            if (g_buffs >> i & 1) {
                set_bool(c, k_buffs[i].var, 1);
                community |= k_buffs[i].community != 0;
                if (!strcmp(k_buffs[i].key, "eggworld")) M32(V_EGG_FLAG) = 1;
            }
        if (community) {
            volatile int64_t *t0 = (volatile int64_t *)(uintptr_t)(MAIN + V_BUFF_START);
            volatile int64_t *t1 = (volatile int64_t *)(uintptr_t)(MAIN + V_BUFF_END);
            if (!*t0 || !*t1 || *t1 <= *t0) *t0 = 1, *t1 = INT64_MAX;  // (the buff window, unset offline: always)
            set_bool(c, V_EV_SEASONONLY, 0);
            set_bool(c, V_EV_ENABLED, 1);
        }
    }
}
