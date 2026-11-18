// Diablo III's sound balance on PC (the tester 2026-11-15, after trying it by ear: "the music finally feels proper at 150%
// volume, but the sound effects are too loud - keep 150% as the standard 100% and reduce the sound effects by 40%").
//
// The game mixes everything itself into one 5.1 stream (audout), so the sound effects can only be lowered inside the
// game. Its sound options - MasterVolume, EffectVolume, MusicVolume, NarrationVolume, AmbientVolume (1.0 / 0.8 by
// default, each capped at 1.0) - come from its settings file, prefs.dat (text: `EffectVolume "0.800000"`), when it
// starts; the console's menus never show them. Measured in town: all at 0 in the file is silence, so the file's values
// are the ones used.
//
// So: whenever the game reads prefs.dat, its EffectVolume line is rewritten in the read data (same length - the file
// is read by offset) to 0.8 x 0.6 x "Sound effects" (Audio tab, g_settings.effects_volume, 100% = this balance), and
// the whole output is played 1.5x (d3_volume_output_gain, audio.c), the old 150%. The effects setting applies from the
// game's next start (the game reads the file once). D3_MUSIC_GAME=1: the game's own balance and level.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

extern bool game_is_d3(void);

// (the tester 2026-11-15: the Sound effects setting made no difference he could hear - removed; he uses the general Volume.
// Both changes here are off unless D3_PC_BALANCE=1, kept for trying again.)
static bool pc_balance(void) {
    static int on = -1;
    if (on < 0) on = game_is_d3() && getenv("D3_PC_BALANCE") != NULL;
    return on;
}
float d3_volume_output_gain(void) { return pc_balance() ? 1.5f : 1.0f; }

// `Key "d.dddddd"` in [p, end): the value's 8 characters rewritten to v (only that exact form: same length)
static bool set_value(char *p, char *end, const char *key, float v) {
    size_t kl = strlen(key);
    for (char *s = p; s + kl + 11 <= end; s++) {
        if (memcmp(s, key, kl) || s[kl] != ' ' || s[kl + 1] != '"') continue;
        if (s != p && s[-1] != '\n') continue;  // (a whole key, at the start of its line)
        char *val = s + kl + 2;
        if (!(val[0] >= '0' && val[0] <= '9' && val[1] == '.' && val[8] == '"')) return false;
        char buf[16];
        snprintf(buf, sizeof buf, "%.6f", v < 0 ? 0.0f : v > 1.0f ? 1.0f : v);
        memcpy(val, buf, 8);
        return true;
    }
    return false;
}

void d3_volume_prefs_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (!pc_balance() || size <= 0) return;
    size_t pl = strlen(path);
    if (pl < 9 || strcmp(path + pl - 9, "prefs.dat")) return;
    (void)off;
    float e = 0.8f * 0.6f * g_settings.effects_volume;
    char *p = (char *)(uintptr_t)addr, *end = p + size;
    if (set_value(p, end, "EffectVolume", e))
        LOG("[audio] Diablo III's sound effects at %.2f (%.0f%% of the PC balance), output x1.5\n", e > 1 ? 1.0f : e,
            g_settings.effects_volume * 100);
}

// ---- Music volume (the tester 2026-11-15: "the music is too quiet against the effects, I just want a normal slider") ----
// The game's sound engine (FMOD) plays all music through a channel group the game makes at start: main+0x31c1a0 calls
// System::createChannelGroup("Music", &group) and keeps the group at 0x700185dce0. The game itself only ever sets that
// group to 1 (on) or 0 (off, main+0x31d3f0 in its sound update, last value kept at 0x700185dce8). FMOD plays a group
// louder than its sounds' own level when its volume is above 1. So:
//  - d3_group_volume (d3_gen_patch.py #8, start of ChannelGroup::setVolume main+0xa4d720): when the group is the Music
//    group, the game's value is multiplied by "Music volume" (g_settings.music_volume, Audio tab, 100% = the game's own).
//  - d3_music_tick (game thread, input.c fill_state, like our menu's clicks): when the setting changes, the group gets
//    the new volume at once (game's last 0/1 x setting) - no restart needed.
// D3_MUSIC_GROUP_OFF=1: neither.
#include "../cpu.h"
extern void guest_call(Cpu *c, uint64_t addr);
#define D3_MUSIC_GROUP_PTR 0x700185dce0ull
#define D3_MUSIC_GROUP_LAST 0x700185dce8ull
#define D3_CHANNELCONTROL_SETVOLUME 0x7000a4e050ull

static bool music_group_on(void) {
    static int on = -1;
    if (on < 0) on = game_is_d3() && getenv("D3_MUSIC_GROUP_OFF") == NULL;
    return on;
}
static float s0_get(Cpu *c) { uint32_t b = (uint32_t)c->v[0]; float f; memcpy(&f, &b, 4); return f; }
static void s0_set(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[0] = (__uint128_t)b; }
static float music_setting(void) {
    float m = g_settings.music_volume;
    return m < 0 ? 0 : m > 6 ? 6 : m;  // (the slider's 200% of D3_MUSIC_BASE 3)
}

void d3_group_volume(Cpu *c) {
    if (!music_group_on() || !c->x[0]) return;
    if (c->x[0] != *(volatile uint64_t *)(uintptr_t)D3_MUSIC_GROUP_PTR) return;
    float v = s0_get(c), m = music_setting();
    s0_set(c, v * m);
    LOG("[audio] music group: the game sets %.2f -> %.2f (Music volume %.0f%%)\n", v, v * m, m * 100);
}

// D3_MUSIC_TEST=<frame>:<volume>,... (test runs): Music volume set to <volume> at that frame, as the slider would
static void music_test_step(void) {
    static const char *next = (const char *)1;
    if (next == (const char *)1) next = getenv("D3_MUSIC_TEST");
    if (!next || !*next) return;
    extern uint64_t renderer_present_count(void);
    unsigned long long f = 0;
    float v = 1;
    if (sscanf(next, "%llu:%f", &f, &v) != 2) { next = NULL; return; }
    if (renderer_present_count() < f) return;
    g_settings.music_volume = v;
    LOG("[audio] test: Music volume -> %.0f%% at frame %llu\n", v * 100, (unsigned long long)renderer_present_count());
    next = strchr(next, ',');
    if (next) next++;
}

void d3_music_tick(Cpu *c) {
    if (!music_group_on()) return;
    music_test_step();
    static float applied = -1;
    static uint64_t applied_grp;
    uint64_t grp = *(volatile uint64_t *)(uintptr_t)D3_MUSIC_GROUP_PTR;
    float m = music_setting();
    if (!grp || (grp == applied_grp && m == applied)) return;
    float last = *(volatile float *)(uintptr_t)D3_MUSIC_GROUP_LAST;  // (the game's own 0 / 1; -1 = not set yet = 1)
    float base = last < 0 ? 1.0f : last;
    static Cpu save;  // (the game's registers, as they were: this runs inside its controller poll)
    save = *c;
    c->x[0] = grp;
    s0_set(c, base * m);
    guest_call(c, D3_CHANNELCONTROL_SETVOLUME);  // (FMOD ChannelControl::setVolume itself: not through our hook)
    uint32_t r = (uint32_t)c->x[0];
    *c = save;
    extern uint64_t renderer_present_count(void);
    LOG("[audio] Music volume %.0f%%: music group %llx at %.2f (result %u, frame %llu)\n", m * 100,
        (unsigned long long)grp, base * m, r, (unsigned long long)renderer_present_count());
    applied = m;
    applied_grp = grp;
}
