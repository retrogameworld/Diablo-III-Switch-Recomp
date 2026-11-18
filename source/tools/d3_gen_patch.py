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
    # 2d. pause menu (main+0x21f480 adds its entries with main+0x21f780): Network Settings, Challenges, Leaderboards,
    #     Season Journey and Connect to Diablo Servers only when d3_options_tab_shown(3) (D3_MENU_ALL)
] + [
    ('main_077.c',
     '    c->x[30] = 0x%xull; f_700021f780(c);\n' % ret,
     '    if (d3_options_tab_shown(3)) { c->x[30] = 0x%xull; f_700021f780(c); }  /* d3_gen_patch.py */\n' % ret,
     'int d3_options_tab_shown(int which);  /* d3_gen_patch.py */\n')
    for ret in (0x700021f55c, 0x700021f610, 0x700021f65c, 0x700021f6a4, 0x700021f704)
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
    """The patched text of the generated chunk file `fn` (a base name, e.g. main_117.c)."""
    def add_decl(s, decl):
        if decl in s:
            return s
        i = s.index('\n', s.index('#include "simd.h"')) + 1
        return s[:i] + decl + s[i:]
    for f, old, new in UPGRADES:
        if f == fn and old in s:
            s = s.replace(old, new)
    for f, anchor, insert, decl in PATCHES:
        if f != fn or insert in s:
            continue
        if s.count(anchor) != 1:
            raise SystemExit(f'd3_gen_patch: {fn}: anchor not found once')
        s = add_decl(s.replace(anchor, anchor + insert, 1), decl)
    for f, old, new, decl in REPLACES:
        if f != fn or new in s:
            continue
        if s.count(old) != 1:
            raise SystemExit(f'd3_gen_patch: {fn}: anchor not found once')
        s = add_decl(s.replace(old, new, 1), decl)
    return s
