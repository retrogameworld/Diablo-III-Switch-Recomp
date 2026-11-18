// Our menu's sounds (ui_sounds.h): each WAV is loaded on first use and played through an SDL audio stream of its own
// (the game's audio goes through runtime/services/audio.c; this one only carries the menu's clicks). A new sound cuts
// the one before off, as menu clicks do.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "game_addresses.h"
#include "hle.h"
#include "settings.h"
#include "ui_sounds.h"

static const char *const k_files[UI_SND_N] = {"move", "select", "tab", "open", "close", "move"};
static SDL_AudioStream *g_stream;
static SDL_AudioSpec g_spec;
static Uint8 *g_buf[UI_SND_N];
static Uint32 g_len[UI_SND_N];
static int g_tried[UI_SND_N];
static SDL_Mutex *g_lock;
static int g_state;  // 0 not started, 1 ready, -1 off

static bool start(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    const char *na = getenv("LARECOMP_NO_AUDIO");
    if ((na && strcmp(na, "0")) || !game_is_d3() || getenv("D3_NO_MENU_SOUNDS")) return false;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    g_spec = (SDL_AudioSpec){SDL_AUDIO_S16, 1, 44100};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &g_spec, NULL, NULL);
    if (!g_stream) { LOG("[ui] menu sounds: no audio device (%s)\n", SDL_GetError()); return false; }
    SDL_ResumeAudioStreamDevice(g_stream);
    g_lock = SDL_CreateMutex();
    g_state = 1;
    return true;
}

static bool load(int which) {
    if (g_buf[which]) return true;
    if (g_tried[which]) return false;
    g_tried[which] = 1;
    char path[128];
    snprintf(path, sizeof path, "build/d3_ui_sounds/%s.wav", k_files[which]);
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;
    // (to the stream's format once: mono 16-bit 44.1 kHz)
    Uint8 *cvt = NULL;
    int clen = 0;
    if (!SDL_ConvertAudioSamples(&spec, buf, (int)len, &g_spec, &cvt, &clen)) { SDL_free(buf); return false; }
    SDL_free(buf);
    g_buf[which] = cvt;
    g_len[which] = (Uint32)clen;
    LOG("[ui] menu sound %s: %u ms\n", k_files[which], (unsigned)(clen / 2 * 1000 / 44100));
    return true;
}

void ui_sound(int which) {
    if (which < 0 || which >= UI_SND_N) return;
    if (getenv("D3_LOG_UI_SOUNDS")) {  // (the tester 2026-11-15: "each click sounds like a double scroll")
        extern uint64_t renderer_present_count(void);
        LOG("[uisound] menu asks for %s (frame %llu)\n", k_files[which], (unsigned long long)renderer_present_count());
    }
    if (game_is_d3() && !getenv("D3_MENU_SOUNDS_PC") && !getenv("D3_NO_MENU_SOUNDS")) {
        // (Diablo III: the game plays its own Options screen's sounds for us - runtime/hle/d3_menu.c; the tester 2026-11-15)
        // (moving / changing / tabs: the main menu's scroll sound, 293971 - the tester 2026-11-15 "the same scrolling sound as the
        // main menu"; the Options screen's own is 293897)
        // (the tester 2026-11-15, later: the main menu's scroll sound "sounds like each click is a double scroll" - one play
        // per press, checked in a log: the sound itself has two parts. Moving and changing a value now use the Options
        // screen's own single tick, 293897; tabs keep the main menu's. D3_MENU_MOVE_SOUND=<id> to try another)
        // (tabs too: the tester, "still a double sound moving between tabs")
        static uint32_t snd[UI_SND_N] = {293897, 293898, 293897, 293898, 291719, 293897};
        static int env_done;
        if (!env_done++ && getenv("D3_MENU_MOVE_SOUND")) snd[0] = snd[5] = (uint32_t)atoi(getenv("D3_MENU_MOVE_SOUND"));
        extern void d3_ui_sound_queue(uint32_t snd);
        d3_ui_sound_queue(snd[which]);
        return;
    }
    if (!start()) return;
    SDL_LockMutex(g_lock);
    if (load(which)) {
        float vol = g_settings.volume;
        if (vol < 0) vol = 0;
        if (vol > 1) vol = 1;
        int n = (int)(g_len[which] / 2);
        Sint16 *tmp = SDL_malloc((size_t)n * 2);
        if (tmp) {
            const Sint16 *src = (const Sint16 *)g_buf[which];
            // (the tester 2026-11-15: too loud. The game's own menu clicks are soft ticks under the music: ~1/5 of full
            // scale, the move tick quieter still)
            float k = vol * (which == UI_SND_MOVE ? 0.14f : 0.2f);
            for (int i = 0; i < n; i++) tmp[i] = (Sint16)(src[i] * k);
            SDL_ClearAudioStream(g_stream);
            SDL_PutAudioStreamData(g_stream, tmp, n * 2);
            SDL_free(tmp);
        }
    }
    SDL_UnlockMutex(g_lock);
}
