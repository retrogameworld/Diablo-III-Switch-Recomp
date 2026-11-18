// Diablo III: an offline game without the online-only menu entries. The main menu (and its More page) asks the game's
// "is this entry shown" function for each of its 30 entries (main+0x35bf90, called with the menu and the entry's id);
// the generated code of that function first asks d3_menu_hidden (put there by src/analysis/d3_gen_patch.py, which is
// run again after the game code is generated). Ids from the game's label table (main+0x35bbf0): 1 buy the game (demo),
// 3 Network Settings, 5 matchmaking, 6 find a game, 10 / 11 store pages, 12 Connect to Diablo Servers, 17 challenges,
// 18 leaderboards, 21 / 22 save import / export (online), 23 quit demo, 26 what's new (news from the servers).
// the tester 2026-11-15. D3_MENU_ALL=1: every entry the game itself would show.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// The "Disconnected from Diablo Network / Not Signed In" box of the main and pause menus: shown only with D3_MENU_ALL
int d3_online_status_shown(void) { return getenv("D3_MENU_ALL") != NULL; }

// Options tabs (main+0x20db60): 1 Multiplayer, 2 Privacy - left out (online settings; local co-op needs neither)
int d3_options_tab_shown(int which) { (void)which; return getenv("D3_MENU_ALL") != NULL; }

// "Exit Game" at the end of the main menu (the tester 2026-11-16). The menu lists its entries in id order and the spare ids
// are all before More (13), so three shift by one: id 11 shows "PC Settings" (12's label), 12 shows "More" (13's) and
// acts as it, 13 shows "Exit Game" (23's label, "Quit Demo", renamed by d3_text.c) and closes the game.
// D3_NO_EXIT_ENTRY=1: as before (12 = PC Settings, no Exit Game).
static bool exit_entry(void) {
    static int on = -1;
    if (on < 0) on = !getenv("D3_NO_EXIT_ENTRY") && !getenv("D3_NO_PC_SETTINGS_ENTRY") && !getenv("D3_MENU_ALL");
    return on;
}
// main+0x35bbf0, the label of an entry by id (d3_gen_patch.py #10): the id whose label is shown
uint32_t d3_menu_label_id(uint32_t id) {
    if (!exit_entry()) return id;
    return id == 11 ? 12 : id == 12 ? 13 : id == 13 ? 23 : id;
}

// 0 as the game decides, 1 hidden, 2 shown (12: our "PC Settings", right after Options - d3_pc_settings_entry)
int d3_menu_hidden(uint32_t menu, uint32_t id) {
    static int all = -1;
    if (all < 0) all = getenv("D3_MENU_ALL") != NULL;
    if (all) return 0;
    // (menu: the page - 0 the main menu (the game's ids up to 13), 1 More (above 13). Forced shown on the main page only:
    // they were on More too, the first test showed "PC Settings" and "More" at its top)
    if (exit_entry() && (id == 11 || id == 12)) return menu == 0 ? 2 : 1;
    if (id == 12 && !getenv("D3_NO_PC_SETTINGS_ENTRY")) return menu == 0 ? 2 : 1;
    switch (id) {
    case 1: case 3: case 5: case 6: case 10: case 11: case 12: case 17: case 18: case 21: case 22: case 23: case 26:
        return 1;
    default:
        return 0;
    }
}

// The main menu's button handler (main+0x361870), before it acts on the selected entry's id: our settings menu (id
// 12, or 11 with Exit Game) opens instead; with Exit Game, 12 goes on as 13 (More) and 13 closes the game.
// Returns -1: handled (the game does nothing), else the id the game acts on.
extern void menu_request_open(void);  // runtime/ui/menu.mm
#include <SDL3/SDL.h>
int d3_pc_settings_entry(uint32_t id) {
    if (getenv("D3_NO_PC_SETTINGS_ENTRY") || getenv("D3_MENU_ALL")) return (int)id;
    if (id == (exit_entry() ? 11u : 12u)) { menu_request_open(); return -1; }
    if (!exit_entry()) return (int)id;
    if (id == 12) return 13;
    if (id == 13) {
        extern void settings_save(void);
        settings_save();
        SDL_Event q;
        SDL_zero(q);
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
        return -1;
    }
    return (int)id;
}

// The game's UI sounds go through main+0x4e5f50 (a sound by number) and main+0x315e20 (play it): our settings menu
// asks for the same clicks the game's own menus make, so they sound and sit in the mix exactly as the Switch version's
// (the tester 2026-11-15). D3_LOG_UI_SOUNDS=1: log each number the game plays and from which thread (bring-up).
#include "../hle.h"
extern uint64_t kernel_current_thread_id(void);
void d3_ui_sound_seen(uint32_t id) {
    static int on = -1;
    if (on < 0) on = getenv("D3_LOG_UI_SOUNDS") != NULL;
    if (on > 1) LOG("[uisound] game plays %#x (thread %llu)\n", id, (unsigned long long)kernel_current_thread_id());
}
// (D3_LOG_UI_SOUNDS) main+0x315e20, "play this sound" (x0: the sound's id, x30: who asked): what the menus play
void d3_ui_play_seen(uint32_t snd, uint64_t lr) {
    static int on = -1;
    if (on < 0) on = getenv("D3_LOG_UI_SOUNDS") != NULL;
    extern uint64_t renderer_present_count(void);
    if (on && snd != 0xffffffffu) LOG("[uisound] play %u from %llx (thread %llu, frame %llu)\n", snd, (unsigned long long)lr,
                                      (unsigned long long)kernel_current_thread_id(), (unsigned long long)renderer_present_count());
}

// ---- our settings menu's clicks, played by the game itself (the Switch version's own sounds at its own volume)
// The game's Options screen plays (logged with D3_LOG_UI_SOUNDS, 2026-11-15): 293897 moving and changing a value,
// 293898 accepting, 291719 going back. ui_sounds.c queues one from the menu (render thread); the game's main thread runs
// it at its next controller poll (input.c fill_state -> d3_ui_sound_run), as its own menus do from that thread.
#include <stdatomic.h>
#include <string.h>
#include "../cpu.h"
extern void guest_call(Cpu *c, uint64_t addr);
static _Atomic uint32_t g_snd_pending;
void d3_ui_sound_queue(uint32_t snd) { atomic_store(&g_snd_pending, snd); }
void d3_ui_sound_run(Cpu *c) {
    uint32_t snd = atomic_exchange(&g_snd_pending, 0);
    if (!snd) return;
    static Cpu save;  // (the game's own registers, as they were: this runs inside its controller poll)
    save = *c;
    c->x[0] = snd;
    c->x[1] = *(volatile uint64_t *)(uintptr_t)0x700114b760ull;  // (what the game's own menus pass with it)
    guest_call(c, 0x7000315e20ull);
    *c = save;
    static int logged;
    if (!logged++) LOG("[uisound] our menu plays the game's sounds (first: %u, thread %llu)\n", snd,
                       (unsigned long long)kernel_current_thread_id());
}

// The map's button hints Zoom In / Zoom Out / Recenter Map / Reset Zoom (main+0x3e72c0, d3_gen_patch.py #9). Left out
// for a night (2026-11-15: "nothing works") - they're the D-PAD (up / down / left / right; verified 2026-11-16), whose
// icons were drawn as face buttons (texpatch.c d3_sideways, now d-pad crosses). Shown again; D3_MAP_ZOOM_HINTS=0 hides.
#include <stdlib.h>
#include <string.h>
int d3_map_zoom_hints(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("D3_MAP_ZOOM_HINTS") && !strcmp(getenv("D3_MAP_ZOOM_HINTS"), "0"));
    return on;
}
