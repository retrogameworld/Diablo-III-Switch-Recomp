"""Small changes to the generated Diablo III game code (src/d3recomp/generated, made by tools/gen.py from the player's
own game files and never shared). Run again after the game code is generated anew; it backs up each file it changes
(once, as <file>.orig next to the backups folder), reports what it did, and leaves already-patched files alone.

  1. main+0x35bf90 (the main menu's "is this entry shown"): asks runtime/hle/d3_menu.c d3_menu_hidden first.
  8. main+0xa4d720 (FMOD ChannelGroup::setVolume): Music volume on the game's Music group (hle/d3_volume.c).
  9. main+0x3e72c0 (the map's button hints): Zoom In / Zoom Out / Recenter Map / Reset Zoom left out (d3_menu.c).
 11. main+0x7b7620 (the world step): counted for the 120 fps clock sync (hle/d3_clock.c).
"""
import os
import shutil
import sys

GEN = r'<project folder>\src\d3recomp\generated\main'
BACKUP = r'<project folder>\backups\generated'

# ---- 20. mods (runtime/hle/d3_mods.c, GPL-2.0: from d3hack, through its 2.7.7 port in Diablo III Mobile; GitHub #4).
# Read by tools/gen.py. Function entries that ask d3_mods_fn first (it may answer the whole function, or wrap it):
MOD_ENTRIES = [
    0x5ff00,   # IsSeasonsInitialized (offline season)
    0x56b10,   # IsSignedInOnline (offline season, Challenge Rift)
    0x1b0600,  # the mailbox's lobby check (offline season)
    0x1bf5f0,  # ValidateHeroForPartyMember (offline season)
    0x5282b0,  # PlayerCanPlayHandicap (all difficulties)
    0x4f9820,  # "is this item picked up by walking over it" (auto pickup: every item, GitHub #15)
    0x181060,  # the Game Settings difficulty lock (Torment VII+ "locked until level 70"; all difficulties, GitHub #15)
    0x8875d0,  # Kanai's Cube material use (free cube)
    0x0c8000,  # CPlayerGetMoveSpeedForStickInput (movement speed)
    0x6cc700,  # PowerGetFormulaValueAtLevel (attack speed)
    0x489280,  # ACDInventoryItemAllowedInSlot (equip in any slot)
    0x88dee0,  # LootRollForAncientLegendary (ancient / primal drops)
    0x8128a0,  # sInitializeWorld (god mode, debug flags)
    0x066b10,  # StorageGetPublisherFile (Challenge Rift offline: its two files)
    0x185f70,  # sOnGetChallengeRiftData (Challenge Rift: log)
    0x185aa0,  # the Challenge Rift screen's error (log)
    0x04edc0,  # ChallengeRiftRewardEarned_SaveToProfile (season tag)
    0x7764e0,  # ChallengeRiftGrantRewardToPlayerIfEligible (season tag)
    0x8db530,  # the scripts' DungeonFinderClosingTriggered(seconds) (no rift close timer, GitHub #15)
    0x8a4740,  # the scripts' Wait(seconds) (no rift close timer: the wait right after it)
    0x505d60,  # an item's flavor text (a cosmetic without an icon: "only in the PC version", d3_icon_placeholder)
    0xa2ad70,  # the game's error report (code, message): logged with a guest backtrace before it stops the game
    0x79ff40,  # ExperienceAdd (experience multiplier, GitHub #22)
    0x47ffb0,  # ActorCommonData::ModifyCurrencyAmount (gold multiplier, GitHub #22)
    0x775cd0,  # ChallengeRiftHandleChallengeRewards (unlimited Challenge Rift rewards, GitHub #22)
]
# Instruction variants, switched at run time (d3_mods_word: 0 = the game's own instruction): main offset ->
# (the original word, checked by gen.py; [the variant word]) or [(a number, C written by hand)]. d3_mods.c k_sites has
# the same sites with the setting that turns each on.
MOD_VARIANTS = {
    0x9e3350: (0x1e380018, [0x52800018]),   # portal: town portal / Book of Cain cast time: fcvtzs w24 -> mov w24,#0
    0x7397d0: (0x1e28cd20, [0x1e2703e0]),   # cooldowns: fcsel s0 -> fmov s0,wzr
    0x7f8a60: (0x14000002, [0x2a0003e0]),   #   b over "mov w0,wzr" -> into it (0 ticks)
    0x9bc63c: (0x1e20cd08, [0x1e2703e8]),   #   fcsel s8 -> fmov s8,wzr
    0x9bc928: (0x1e20cd08, [0x1e2703e8]),   #   fcsel s8 -> fmov s8,wzr
    0x20636c: (0x1e213800, [0xd503201f]),   # craft: identify progress 1 - t/T -> 1 (instant)
    0x40fa20: (0x11025908, [0x11001508]),   #   blacksmith: now + 150 ticks -> + 5
    0x1dcfa4: (0x1101e108, [0x11000508]),   #   enchant: now + 120 ticks -> + 1
    0x236d20: (0x321e0fe8, [0x52800008]),   #   Kanai's Cube: 60 / 180 / 120 / 240 ticks -> 0
    0x236d24: (0x52801689, [0x52800009]),
    0x236d28: (0x321d0fea, [0x5280000a]),
    0x236d30: (0x321c0fe8, [0x52800008]),
    0x6a265c: (0xbc685928, [0x1e2e1008]),   # gem100: legendary gem upgrade chance -> 1.0
    0x349ba8: (0xbd406662, [0x1e2e1002]),   # gemfast: gem upgrade progress step -> 1.0
    0x7dfa58: (0x2a0003f6, [(1, 'c->x[22] = (uint32_t)c->x[0]; c->x[0] = 150; '
                                'c->x[23] = (uint32_t)(150u - (uint32_t)c->x[22]);')]),
                                             # gem150: upgrades left = 150 - the gem's level (d3hack's code cave)
    0x47ad7c: (0x97ffff41, [0xd503201f]),   # resource: bl ResourceAttributeSetFloat (the spend) -> nop
    0x844ce4: (0x2a1f03e3, [0x52807ce3]),   # orbs: greater rift progress orbs on elite kills 0 -> 999
    0x4fbae4: (0x540002e0, [0x14000017]),   # legendary: the same legendary already equipped: b.eq -> b (allowed)
    0x97f65c: (0x528bf529, [0x528f78e9]),   # easykill: damage to an ACTOR_EASYKILL actor's target 1e15 -> 1.34e18
    0x97f660: (0x72ab0c69, [0x72abb289]),
    0x0bb550: (0xb9404789, [0x52801fe9]),   # reddmg: floating damage number colour -> red
    0x504d78: (0x2a1403e0, [0x12800000]),   # dropany: the "can this item drop" answer -> -1 (any; Staff of Herding ...)
    0x88f92c: (0x340000c8, [0xd503201f]),   # legendchance: Kadala / Kanai legendary roll: the checks -> always
    0x88f934: (0x340009c8, [0xd503201f]),
    0x88f93c: (0x54000c6c, [0x14000063]),   #   b.gt -> b
    0x88fac4: (0x5400010d, [0xd503201f]),
    0x88fae0: (0x54001a84, [0x140000d4]),   #   b.mi -> b
    0x4fa01c: (0x34000520, [0x528000b9]),   # anygem: socket check: w25 = 5 (allowed) ...
    0x4fa024: (0x7100091f, [0x14000028]),   #   ... and return it
    0x4f99dc: (0x340000a8, [0xd503201f]),   # autopickup: the pickup check's early "no" -> on to the item's flag
    0x4f99ec: (0x340000c0, [0x14000006]),   #   and the item's NO_AUTOPICKUP attribute (gear, gems) -> picked up too
    0x04ee18: (0x54000ee9, [0x14000001]),   # crfarm: ChallengeRiftRewardEarned_SaveToProfile's "this challenge's reward
    0x04ef28: (0x54fff7a8, [0x17ffffbd]),   #   was saved already" -> saved again (d3hack's save-gate bypass; GitHub #22)
    0x1bd07c: (0xb9409115, [(1, 'c->x[21] = LD32(c->x[8] + 0x90ull); if ((uint32_t)c->x[21]) '
                                '{ extern int d3_mods_season(void); c->x[21] = (uint32_t)d3_mods_season(); }')]),
                                             # keepseason: SetGameParamsForHero: a seasonal hero's season -> the current one
    0x358a88: (0x6b08001f, [(1, '{ uint32_t hs_ = (uint32_t)c->x[8]; if (hs_) hs_ = (uint32_t)c->x[0]; '
                                '(void)(subs32(c, (uint32_t)c->x[0], hs_)); }')]),
                                             #   hero select: a seasonal hero of another season -> Play (no Rebirth)
}
_applied = set()  # (the PATCHES / REPLACES that apply_text put in, by index: check_applied)

PATCHES = [
    # (file, anchor (must appear once), text inserted after it, declaration added after the first #include block)
    # 14. the game's event helpers (main+0x6900 signal / 0x6910 wait / 0x6920 timed wait): D3_CLOCK_LOG=4 logs the
    #     frame event's users (d3_clock.c)
    ('main_000.c',
     'void f_7000006900(Cpu *c) {\n    FN_ENTER(0x7000006900ull);\n',
     "    d3_lock_seen(c->x[0], 'S', c->x[30]);  /* d3_gen_patch.py */\n",
     'void d3_lock_seen(uint64_t m, int unlock, uint64_t lr);  /* d3_gen_patch.py */\n'),
    ('main_000.c',
     'void f_7000006910(Cpu *c) {\n    FN_ENTER(0x7000006910ull);\n',
     "    d3_lock_seen(c->x[0], 'W', c->x[30]);  /* d3_gen_patch.py */\n",
     'void d3_lock_seen(uint64_t m, int unlock, uint64_t lr);  /* d3_gen_patch.py */\n'),
    ('main_000.c',
     'void f_7000006920(Cpu *c) {\n    FN_ENTER(0x7000006920ull);\n',
     "    d3_lock_seen(c->x[1], 'T', c->x[30]);  /* d3_gen_patch.py */\n",
     'void d3_lock_seen(uint64_t m, int unlock, uint64_t lr);  /* d3_gen_patch.py */\n'),
    # 13. the game's lock / unlock helpers (main+0x68b0 / 0x68c0): D3_CLOCK_LOG=4 logs the world lock's users (d3_clock.c)
    ('main_000.c',
     'void f_70000068b0(Cpu *c) {\n    FN_ENTER(0x70000068b0ull);\n',
     '    d3_lock_seen(c->x[0], 0, c->x[30]);  /* d3_gen_patch.py */\n',
     'void d3_lock_seen(uint64_t m, int unlock, uint64_t lr);  /* d3_gen_patch.py */\n'),
    ('main_000.c',
     'void f_70000068c0(Cpu *c) {\n    FN_ENTER(0x70000068c0ull);\n',
     '    d3_lock_seen(c->x[0], 1, c->x[30]);  /* d3_gen_patch.py (unlock) */\n',
     'void d3_lock_seen(uint64_t m, int unlock, uint64_t lr);  /* d3_gen_patch.py */\n'),
    # 11. the game's world step (main+0x7b7620, entered once per 1/60 s step): counted (hle/d3_clock.c d3_tick_mark)
    ('main_229.c',
     'void f_70007b7620(Cpu *c) {\n    FN_ENTER(0x70007b7620ull);\n',
     '    d3_tick_mark();  /* d3_gen_patch.py */\n',
     'void d3_tick_mark(void);  /* d3_gen_patch.py */\n'),
    ('main_117.c',
     'void f_700035bf90(Cpu *c) {\n    FN_ENTER(0x700035bf90ull);\n',
     '    { int h_ = d3_menu_hidden((uint32_t)c->x[0], (uint32_t)c->x[1]); if (h_) { c->x[0] = h_ == 2; return; } }  /* d3_gen_patch.py */\n',
     'int d3_menu_hidden(uint32_t menu, uint32_t id);  /* d3_gen_patch.py */\n'),
    # 5. the main menu's button handler (main+0x361870): entry 12 opens our settings menu (d3_menu.c d3_pc_settings_entry)
    ('main_117.c',
     '    { uint64_t ea = c->x[9] + (c->x[8] << 2); uint64_t v = LD32(ea); c->x[8] = (uint64_t)(v); }\nL_700036190c:;\n',
     '    { int r_ = d3_pc_settings_entry((uint32_t)c->x[8]); if (r_ < 0) goto L_7000361894; c->x[8] = (uint32_t)r_; }  /* d3_gen_patch.py */\n',
     'int d3_pc_settings_entry(uint32_t id);  /* d3_gen_patch.py */\n'),
    # 10. the main menu's entry labels by id (main+0x35bbf0): d3_menu.c d3_menu_label_id (Exit Game: ids shift)
    ('main_117.c',
     'void f_700035bbf0(Cpu *c) {\n    FN_ENTER(0x700035bbf0ull);\n',
     '    c->x[0] = d3_menu_label_id((uint32_t)c->x[0]);  /* d3_gen_patch.py */\n',
     'uint32_t d3_menu_label_id(uint32_t id);  /* d3_gen_patch.py */\n'),
    # 6. the game's "UI sound by number" (main+0x4e5f50): D3_LOG_UI_SOUNDS logs each (d3_menu.c d3_ui_sound_seen)
    ('main_151.c',
     'void f_70004e5f50(Cpu *c) {\n    FN_ENTER(0x70004e5f50ull);\n',
     '    d3_ui_sound_seen((uint32_t)c->x[0]);  /* d3_gen_patch.py */\n',
     'void d3_ui_sound_seen(uint32_t id);  /* d3_gen_patch.py */\n'),
    # 7. "play this sound" (main+0x315e20): D3_LOG_UI_SOUNDS logs each (d3_menu.c d3_ui_play_seen)
    ('main_107.c',
     'void f_7000315e20(Cpu *c) {\n    FN_ENTER(0x7000315e20ull);\n',
     '    d3_ui_play_seen((uint32_t)c->x[0], c->x[30]);  /* d3_gen_patch.py */\n',
     'void d3_ui_play_seen(uint32_t snd, uint64_t lr);  /* d3_gen_patch.py */\n'),
    # 8. FMOD ChannelGroup::setVolume (main+0xa4d720): the game's "Music" group gets Music volume x the game's value
    #    (runtime/hle/d3_volume.c d3_group_volume)
    ('main_307.c',
     'void f_7000a4d720(Cpu *c) {\n    FN_ENTER(0x7000a4d720ull);\n',
     '    d3_group_volume(c);  /* d3_gen_patch.py */\n',
     'void d3_group_volume(Cpu *c);  /* d3_gen_patch.py */\n'),
    # 4. zlib 1.2.3's inner decode loop (inflate_fast, main+0x12590): the host's version (runtime/hle/d3_inflate.c)
    ('main_002.c',
     'void f_7000012590(Cpu *c) {\n    FN_ENTER(0x7000012590ull);\n',
     '    if (d3_inflate_fast(c)) return;  /* d3_gen_patch.py */\n',
     '_Bool d3_inflate_fast(Cpu *c); void d3_inflate_check(Cpu *c);  /* d3_gen_patch.py */\n'),
]
# 2. the "Disconnected from Diablo Network / Not Signed In" box: main menu (main+0x362884) and pause menu
#    (main+0x2212b4) show it with SetVisible(1); d3_online_status_shown() decides instead (0 unless D3_MENU_ALL)
REPLACES = [
    # 24. an item without an icon (main+0xbf3c4: the icon lookup failed, the game puts "ItemPlaceholderIcon" - a pink
    #     "i need an icon" box): a cosmetic the Switch version never sold gets its category's own emblem instead
    #     (hle/d3_mods.c d3_icon_placeholder; the tester 2026-11-27 "remove the pink thing its too ugly")
    ('main_035.c',
     '    c->x[1] = (uint64_t)(0x7000dd4000ull);\n    c->x[1] = (uint64_t)(c->x[1] + 0xffdull);\n'
     '    c->x[2] = (uint64_t)(0ull | c->x[19]);\n    c->x[30] = 0x70000bf3d4ull; f_70000f29c0(c);\n',
     '    if (!d3_icon_placeholder(c)) {  /* d3_gen_patch.py */\n'
     '    c->x[1] = (uint64_t)(0x7000dd4000ull);\n    c->x[1] = (uint64_t)(c->x[1] + 0xffdull);\n'
     '    c->x[2] = (uint64_t)(0ull | c->x[19]);\n    c->x[30] = 0x70000bf3d4ull; f_70000f29c0(c);\n    }\n',
     'int d3_icon_placeholder(Cpu *c);  /* d3_gen_patch.py */\n'),
    # 23. a string the game's text tables lack (main+0x6f1518: snprintf "%s - %d:%s" with "!!Missing!!", the id and its
    #     key): a readable name made from the key instead (hle/d3_mods.c d3_missing_text; the offline Anniversary buffs
    #     showed "!!Missing!! - 77545:Anniversary_Buff_EXPMF_11" under Temporary Effects, GitHub #15)
    ('main_211.c',
     '    c->x[30] = 0x70006f1530ull; f_70000084c0(c);\n',
     '    c->x[30] = 0x70006f1530ull; if (!d3_missing_text(c)) f_70000084c0(c);  /* d3_gen_patch.py */\n',
     'int d3_missing_text(Cpu *c);  /* d3_gen_patch.py */\n'),
    # 22. Lua's error catch (luaD_rawrunprotected, main+0xb20ef8 bl setjmp): a host __builtin_setjmp around the call, so the
    #     game's longjmp back to it lands here (runtime/dispatch.c d3_sj_slot / sj_return; a crash at the start of Act 4)
    ('main_339.c',
     '    c->x[30] = 0x7000b20efcull; f_7000c722f0(c);\n',
     '    { void **sjb_ = d3_sj_slot(c, 0x7000b20efcull); if (__builtin_setjmp(sjb_) == 0) { c->x[30] = 0x7000b20efcull; '
     'f_7000c722f0(c); } }  /* d3_gen_patch.py: Lua setjmp */\n',
     'void **d3_sj_slot(Cpu *c, uint64_t ret);  /* d3_gen_patch.py */\n'),
    # 21. mods: hero create's season confirmation (main+0x351ccc b.ne -> mov w1,#1) and SetGameParamsForHero's season of
    #     a hero without data (main+0x1bd140 ldr w21 -> our season) - hle/d3_mods.c
    ('main_117.c',
     '    (void)(subs32(c, (uint32_t)c->x[8], 0x1ull));\n    if (COND_NE(c)) { goto L_7000351cfc; }\n',
     '    (void)(subs32(c, (uint32_t)c->x[8], 0x1ull));\n    if (!d3_mods_season_confirm(c) && COND_NE(c)) { goto L_7000351cfc; }  /* d3_gen_patch.py mods */\n',
     'int d3_mods_season_confirm(Cpu *c);  /* d3_gen_patch.py */\n'),
    ('main_070.c',
     'L_70001bd130:;\n    c->x[8] = (uint64_t)(0x700114a000ull);\n'
     '    { uint64_t ea = c->x[8] + 0x9b8ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x18ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x90ull; uint64_t v = LD32(ea); c->x[21] = (uint64_t)(v); }\n',
     'L_70001bd130:;\n    c->x[8] = (uint64_t)(0x700114a000ull);\n'
     '    { uint64_t ea = c->x[8] + 0x9b8ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x18ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x90ull; uint64_t v = LD32(ea); c->x[21] = (uint64_t)(d3_mods_season_created((uint32_t)v)); }  /* d3_gen_patch.py mods */\n',
     'uint32_t d3_mods_season_created(uint32_t v);  /* d3_gen_patch.py */\n'),
    # 16. the world (game simulation) thread's sleep between its steps (main+0x7c5cdc): above 60 fps it waits for the
    #     next frame in which the game counts a step (hle/d3_clock.c d3_sim_wait)
    ('main_230.c',
     '    c->x[0] = (uint32_t)((uint32_t)c->x[21] - (uint32_t)c->x[8]);\n    c->x[30] = 0x70007c5ce0ull; f_7000006fb0(c);\nL_70007c5ce0:;\n',
     '    c->x[0] = (uint32_t)((uint32_t)c->x[21] - (uint32_t)c->x[8]);\n    c->x[30] = 0x70007c5ce0ull; if (!d3_sim_wait((uint32_t)c->x[0])) f_7000006fb0(c);  /* d3_gen_patch.py */\nL_70007c5ce0:;\n',
     'int d3_sim_wait(uint32_t ms);  /* d3_gen_patch.py */\n'),
    # 15. THE 120 FPS FIX: the time the game hands its frame function (main+0x4c8890 -> main+0x40810) is ours above
    #     60 fps (hle/d3_clock.c d3_frame_time; the game's own smoothed frame clock can settle on every second frame)
    ('main_146.c',
     'L_70004c8890:;\n    c->x[0] = (uint32_t)(0u | (uint32_t)c->x[19]);\n    c->x[30] = 0x70004c8898ull; f_7000040810(c);\n',
     'L_70004c8890:;\n    { uint64_t t_ = c->x[19]; c->x[30] = 0x70004c8894ull; f_7000a49c30(c); c->x[19] = d3_frame_time((uint32_t)t_, (uint32_t)c->x[0], c->x[21]); }  /* d3_gen_patch.py */\n    c->x[0] = (uint32_t)(0u | (uint32_t)c->x[19]);\n    c->x[30] = 0x70004c8898ull; f_7000040810(c);\n',
     'uint32_t d3_frame_time(uint32_t game_t, uint32_t now, uint64_t game);  /* d3_gen_patch.py */\n'),
    # 12. the world thread's sleep between its steps (main+0x4c64ac): above 60 fps it waits for its place in the
    #     frame slots instead (hle/d3_clock.c d3_world_wait)
    ('main_146.c',
     'L_70004c64ac:;\n    c->x[30] = 0x70004c64b0ull; f_7000006fb0(c);\n',
     'L_70004c64ac:;\n    c->x[30] = 0x70004c64b0ull; if (!d3_world_wait((uint32_t)c->x[0])) f_7000006fb0(c);  /* d3_gen_patch.py */\n',
     'int d3_world_wait(uint32_t ms);  /* d3_gen_patch.py */\n'),
    ('main_117.c',
     '    c->x[1] = (uint32_t)(0u | 0x1ull);\n    c->pc = c->x[8]; c->x[30] = 0x7000362888ull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)(d3_online_status_shown());  /* d3_gen_patch.py */\n    c->pc = c->x[8]; c->x[30] = 0x7000362888ull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    ('main_077.c',
     '    c->x[1] = (uint32_t)(0u | 0x1ull);\n    c->pc = c->x[8]; c->x[30] = 0x70002212b8ull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)(d3_online_status_shown());  /* d3_gen_patch.py */\n    c->pc = c->x[8]; c->x[30] = 0x70002212b8ull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    # 2b. the season status line ("Not Signed In" with the leaf) of the main menu: SetVisible(1) at main+0x362a08 and
    #     main+0x362ce8 -> d3_online_status_shown()
    ('main_117.c',
     '    c->x[1] = (uint32_t)(0u | 0x1ull);\n    c->pc = c->x[8]; c->x[30] = 0x7000362a0cull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)(d3_online_status_shown());  /* d3_gen_patch.py */\n    c->pc = c->x[8]; c->x[30] = 0x7000362a0cull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    ('main_117.c',
     '    c->x[1] = (uint32_t)(0u | 0x1ull);\n    c->pc = c->x[8]; c->x[30] = 0x7000362cecull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)(d3_online_status_shown());  /* d3_gen_patch.py */\n    c->pc = c->x[8]; c->x[30] = 0x7000362cecull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    ('main_117.c',
     '    c->x[1] = (uint32_t)((uint32_t)c->x[19] & (uint32_t)c->x[8]);\n'
     '    { uint64_t ea = c->x[0] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x50ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    c->pc = c->x[8]; c->x[30] = 0x7000362d20ull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)((uint32_t)c->x[19] & (uint32_t)c->x[8] & (uint32_t)d3_online_status_shown());  /* d3_gen_patch.py */\n'
     '    { uint64_t ea = c->x[0] + 0x0ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    { uint64_t ea = c->x[8] + 0x50ull; uint64_t v = LD64(ea); c->x[8] = (uint64_t)(v); }\n'
     '    c->pc = c->x[8]; c->x[30] = 0x7000362d20ull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    # 2c. the season status box itself (SeasonStateNotification, handle main+0x1151210): SetVisible at main+0x3606a8
    ('main_117.c',
     '    c->x[1] = (uint32_t)(0u | (uint32_t)c->x[20]);\n    c->pc = c->x[8]; c->x[30] = 0x70003606acull; rt_dispatch(c);\n',
     '    c->x[1] = (uint32_t)(0u | ((uint32_t)c->x[20] & (uint32_t)d3_online_status_shown()));  /* d3_gen_patch.py */\n'
     '    c->pc = c->x[8]; c->x[30] = 0x70003606acull; rt_dispatch(c);\n',
     'int d3_online_status_shown(void);  /* d3_gen_patch.py */\n'),
    # 2d. pause menu (main+0x21f480 adds its entries with main+0x21f780, the entry's id at sp+0x18): Network Settings
    #     (2), Challenges (4), Leaderboards (5), Season Journey (6) and Connect to Diablo Servers (9) only when
    #     d3_pause_entry_shown(id) says so (d3_menu.c: Challenges yes, the online ones only with D3_MENU_ALL)
] + [
    ('main_077.c',
     '    c->x[30] = 0x%xull; f_700021f780(c);\n' % ret,
     '    if (d3_pause_entry_shown(%d)) { c->x[30] = 0x%xull; f_700021f780(c); }  /* d3_gen_patch.py */\n' % (id_, ret),
     'int d3_pause_entry_shown(int id);  /* d3_gen_patch.py */\n')
    for ret, id_ in ((0x700021f55c, 2), (0x700021f610, 4), (0x700021f65c, 5), (0x700021f6a4, 6), (0x700021f704, 9))
] + [
    # 2e. pause menu's button hint "View Online Friends" (main+0x1bb550, call returning to main+0x22120c)
    ('main_077.c',
     '    c->x[30] = 0x700022120cull; f_70001bb550(c);\n',
     '    if (d3_options_tab_shown(4)) { c->x[30] = 0x700022120cull; f_70001bb550(c); }  /* d3_gen_patch.py */\n',
     'int d3_options_tab_shown(int which);  /* d3_gen_patch.py */\n'),
    # 4b. the end of that loop: D3_INFLATE_CHECK=1 compares the host's result with the lifted one
    ('main_002.c',
     '    c->sp = (uint64_t)(c->sp + 0xa0ull);\n    return;\n    rt_udf(c, 0x7000012d48ull);\n',
     '    c->sp = (uint64_t)(c->sp + 0xa0ull);\n    d3_inflate_check(c);  /* d3_gen_patch.py */\n    return;\n'
     '    rt_udf(c, 0x7000012d48ull);\n',
     '_Bool d3_inflate_fast(Cpu *c); void d3_inflate_check(Cpu *c);  /* d3_gen_patch.py */\n'),
    # 3. Options tabs (main+0x20db60 adds Gameplay, Video, Sound, Multiplayer, Controller, Privacy with main+0x210450):
    #    Multiplayer (call returning to main+0x20e24c) and Privacy (main+0x20e26c) only when d3_options_tab_shown says so
    ('main_076.c',
     '    c->x[30] = 0x700020e24cull; f_7000210450(c);\n',
     '    if (d3_options_tab_shown(1)) { c->x[30] = 0x700020e24cull; f_7000210450(c); }  /* d3_gen_patch.py */\n',
     'int d3_options_tab_shown(int which);  /* d3_gen_patch.py */\n'),
    ('main_076.c',
     '    c->x[30] = 0x700020e26cull; f_7000210450(c);\n',
     '    if (d3_options_tab_shown(2)) { c->x[30] = 0x700020e26cull; f_7000210450(c); }  /* d3_gen_patch.py */\n',
     'int d3_options_tab_shown(int which);  /* d3_gen_patch.py */\n'),
]


# 9. the map's button hints (main+0x3e72c0): Zoom In, Zoom Out, Recenter Map, Reset Zoom don't work in this build
#    (none of the game's buttons changed the map in a test) - left out, d3_map_zoom_hints() (d3_menu.c) is 0 unless
#    D3_MAP_ZOOM_HINTS=1 (the tester 2026-11-15: "remove them if you can't find what makes them work")
for _ret in ('3e747c', '3e74b0', '3e74e4', '3e7518'):
    REPLACES.append(('main_131.c',
        f'    c->x[30] = 0x7000{_ret}ull; f_70001bb550(c);\n',
        f'    c->x[30] = 0x7000{_ret}ull; if (d3_map_zoom_hints()) f_70001bb550(c);  /* d3_gen_patch.py */\n',
        'int d3_map_zoom_hints(void);  /* d3_gen_patch.py */\n'))

# Earlier versions of a patch's text, brought up to date first (a file patched before keeps working)
UPGRADES = [
    ('main_117.c',
     '    if (d3_pc_settings_entry((uint32_t)c->x[8])) goto L_7000361894;  /* d3_gen_patch.py */\n',
     '    { int r_ = d3_pc_settings_entry((uint32_t)c->x[8]); if (r_ < 0) goto L_7000361894; c->x[8] = (uint32_t)r_; }  /* d3_gen_patch.py */\n'),
    ('main_117.c',
     '    if (d3_menu_hidden((uint32_t)c->x[0], (uint32_t)c->x[1])) { c->x[0] = 0; return; }  /* d3_gen_patch.py */\n',
     '    { int h_ = d3_menu_hidden((uint32_t)c->x[0], (uint32_t)c->x[1]); if (h_) { c->x[0] = h_ == 2; return; } }  /* d3_gen_patch.py */\n'),
] + [
    # (2d before v1.2.6: every one of those pause menu entries behind d3_options_tab_shown(3))
    ('main_077.c',
     '    if (d3_options_tab_shown(3)) { c->x[30] = 0x%xull; f_700021f780(c); }  /* d3_gen_patch.py */\n' % ret,
     '    if (d3_pause_entry_shown(%d)) { c->x[30] = 0x%xull; f_700021f780(c); }  /* d3_gen_patch.py */\n' % (id_, ret))
    for ret, id_ in ((0x700021f55c, 2), (0x700021f610, 4), (0x700021f65c, 5), (0x700021f6a4, 6), (0x700021f704, 9))
]


def main():
    os.makedirs(BACKUP, exist_ok=True)
    results = []
    for fn, old, new in UPGRADES:
        p = os.path.join(GEN, fn)
        s = open(p, encoding='utf-8').read()
        if old in s:
            b = os.path.join(BACKUP, fn + '.orig')
            if not os.path.exists(b):
                shutil.copyfile(p, b)
            open(p, 'w', encoding='utf-8', newline='\n').write(s.replace(old, new))
            results.append((fn, 'upgraded'))
    for fn, anchor, insert, decl in PATCHES:
        p = os.path.join(GEN, fn)
        s = open(p, encoding='utf-8').read()
        if insert in s:
            results.append((fn, 'already patched'))
            continue
        if s.count(anchor) != 1:
            results.append((fn, 'NOT patched: anchor not found once'))
            continue
        b = os.path.join(BACKUP, fn + '.orig')
        if not os.path.exists(b):
            shutil.copyfile(p, b)
        s = s.replace(anchor, anchor + insert, 1)
        i = s.index('\n', s.index('#include "simd.h"')) + 1
        s = s[:i] + decl + s[i:]
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        results.append((fn, 'patched'))
    for fn, old, new, decl in REPLACES:
        p = os.path.join(GEN, fn)
        s = open(p, encoding='utf-8').read()
        if new in s:
            if decl not in s:  # (an upgraded change may call a function its old text didn't)
                i = s.index('\n', s.index('#include "simd.h"')) + 1
                open(p, 'w', encoding='utf-8', newline='\n').write(s[:i] + decl + s[i:])
            results.append((fn, 'already patched (status box)'))
            continue
        if s.count(old) != 1:
            results.append((fn, 'NOT patched (status box): anchor not found once'))
            continue
        b = os.path.join(BACKUP, fn + '.orig')
        if not os.path.exists(b):
            shutil.copyfile(p, b)
        s = s.replace(old, new, 1)
        if decl not in s:
            i = s.index('\n', s.index('#include "simd.h"')) + 1
            s = s[:i] + decl + s[i:]
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        results.append((fn, 'patched (status box)'))
    for fn, r in results:
        print(f'{fn}: {r}')
    if sys.stdin and sys.stdin.isatty():
        try:
            input('Press Enter to close...')
        except EOFError:
            pass


if __name__ == '__main__':
    main()


# ---- used by tools/gen.py (and so by the game code builder on the player's PC: runtime/gamebuild.c): each generated
# chunk of Diablo III's main program is patched as it is written, before it is compiled. Same rules as main() above.
def apply_text(fn, s):
    """The patched text of the generated chunk file `fn` (a base name, e.g. main_117.c). A change goes in wherever its
    anchor is (its file name is where it was when written: a change to the generator can move a function to another
    chunk); check_applied() then says whether every one found its place."""
    import re

    def add_decl(s, decl):
        if decl in s:
            return s
        i = s.index('\n', s.index('#include "simd.h"')) + 1
        return s[:i] + decl + s[i:]

    def here(f, anchor):
        return f == fn or re.search(r'70[0-9a-f]{8}', anchor) is not None
    for f, old, new in UPGRADES:
        if f == fn and old in s:
            s = s.replace(old, new)
    for k, (f, anchor, insert, decl) in enumerate(PATCHES):
        if not here(f, anchor):
            continue
        if insert in s:
            _applied.add(('P', k))
            continue
        n = s.count(anchor)
        if n == 0:
            continue
        if n != 1:
            raise SystemExit(f'd3_gen_patch: {fn}: anchor of change P{k} found {n} times')
        s = add_decl(s.replace(anchor, anchor + insert, 1), decl)
        _applied.add(('P', k))
    for k, (f, old, new, decl) in enumerate(REPLACES):
        if not here(f, old):
            continue
        if new in s:
            s = add_decl(s, decl)  # (an upgraded change may call a function its old text didn't)
            _applied.add(('R', k))
            continue
        n = s.count(old)
        if n == 0:
            continue
        if n != 1:
            raise SystemExit(f'd3_gen_patch: {fn}: anchor of change R{k} found {n} times')
        s = add_decl(s.replace(old, new, 1), decl)
        _applied.add(('R', k))
    return s


def check_applied():
    """After all chunks (tools/gen.py): every change found its place."""
    miss = [f'P{k} ({p[0]})' for k, p in enumerate(PATCHES) if ('P', k) not in _applied]
    miss += [f'R{k} ({r[0]})' for k, r in enumerate(REPLACES) if ('R', k) not in _applied]
    if miss:
        raise SystemExit('d3_gen_patch: not found in the generated code: ' + ', '.join(miss))
