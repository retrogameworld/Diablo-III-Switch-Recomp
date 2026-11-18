// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>
#include <thread>

#include "imgui.h"
#include "option_rows.h"
#ifndef __APPLE__
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // (nvn/vulkan/imgui_glue.cpp)
extern "C" int d3_prompts_xbox(void);  // (hle/input.c: Diablo III's button pictures in Xbox style)
extern "C" uint8_t *texpatch_d3_embedded_logo(uint32_t *w, uint32_t *h);  // (nvn/texpatch.c: a private build's logo)
extern "C" volatile int g_d3_ui_art_gen;  // (runtime/hle/d3_art.c: bumped when it writes a menu piece)
#endif
#include "../ui_sounds.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
// runtime/nvn/vulkan/device.c: the command buffer ImGui records into (a secondary one the recorder thread executes in
// the window image's commands, or that buffer itself after the recorder caught up), then handed back
extern "C" void *vk_overlay_begin(void *window_cmd);
extern "C" void vk_overlay_end(void *window_cmd, void *cmd);
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "menu.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_tab_draw(float scale);  // controls_ui.mm
bool controls_ui_took_back(void);
bool rando_tab_took_back(void);  // rando_tab.mm: B went back from the Triforce hunt's count
#include "../input_bind.h"
#include "../game_addresses.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
// (any thread) open the menu at the next render: Diablo III's main menu entry "PC Settings" (runtime/hle/d3_menu.c)
static std::atomic<bool> g_open_request{false};
// (the menu opened or closed: forget every key ImGui still thinks is held. The Circle that closed it had its release go
// to the game - the menu was shut - so ImGui kept Circle "down", and the next Circle only "released" it: Back needed
// two presses the next time the menu was open; the tester 2026-11-15, his log: one Back seen per close)
static std::atomic<bool> g_clear_keys{false};
static bool g_open_quiet;  // (opened from the game's own menu entry: the game already made its accept sound)
extern "C" void menu_request_open(void) { g_open_request.store(true); }
static void set_open(bool open) {
    if (open == g_open.load()) return;
    LOG("[menu] %s\n", open ? "opened" : "closed");
    g_clear_keys.store(true);
    g_open.store(open);
    if (!(open && g_open_quiet)) ui_sound(open ? UI_SND_OPEN : UI_SND_CLOSE);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        save_editor_on_open();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}


// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + confirm, back deletes, Start / + OK; buttons as in pad_nav.h) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game's name limit counts characters as the console's keyboard does, in UTF-16 units (a character past U+FFFF
// takes two); the text is UTF-8. Cuts s after max_units of them, only between characters (a character left incomplete
// - a cut inside it, or bytes that are no character - goes too, with the rest), and returns how many units are left.
static int kbd_cut(char *s, int max_units) {
    int units = 0;
    for (unsigned char *p = (unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
        int i = 1;
        while (i < n && (p[i] & 0xc0) == 0x80) i++;
        if (i == n && n > 2 && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] > 0x9f) || (*p == 0xf0 && p[1] < 0x90) ||
                                (*p == 0xf4 && p[1] > 0x8f)))
            n = 0;  // (overlong, a surrogate or past U+10FFFF: the game's UTF-8 conversion refuses those too)
        int u = n == 4 ? 2 : 1;
        if (!n || i < n || units + u > max_units) { *p = 0; break; }
        units += u, p += n;
    }
    return units;
}
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            static int last_full = DISPLAY_BORDERLESS;
            if (g_settings.display_mode != DISPLAY_WINDOWED) last_full = g_settings.display_mode;
            g_settings.display_mode = g_settings.display_mode == DISPLAY_WINDOWED ? last_full : DISPLAY_WINDOWED;
            g_settings.fullscreen = g_settings.display_mode != DISPLAY_WINDOWED;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        // (Diablo III: Create alone, and the PS button - the touchpad is the inventory there, as on PlayStation)
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE ||
                                (game_is_d3() && e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK))) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = ui_pad_key(e->gbutton.which, e->gbutton.button, down);  // (confirm / back: pad_nav.h)
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

// The window in the Display mode setting: windowed; borderless (full screen at the desktop's own mode, no mode change);
// exclusive (full screen with the display set to the desktop's resolution: SDL's exclusive mode). Test windows
// (LARECOMP_HIDDEN / LARECOMP_WINDOW) stay windows.
extern "C" void ui_apply_display_mode(SDL_Window *w) {
    if (getenv("LARECOMP_HIDDEN") || getenv("LARECOMP_WINDOW")) return;
    int m = g_settings.display_mode;
    if (m == DISPLAY_EXCLUSIVE) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w));
        if (!dm || !SDL_SetWindowFullscreenMode(w, dm)) LOG("[window] exclusive full screen: %s\n", SDL_GetError());
    } else SDL_SetWindowFullscreenMode(w, NULL);
    SDL_SetWindowFullscreen(w, m != DISPLAY_WINDOWED);
}

extern "C" uint32_t g_window_resize_request;  // (test scripts: runtime/hle/input.c "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        ui_apply_display_mode(w);
    }
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImFont *g_font_title;  // (Diablo III's title face for "Loading...", when present: init())
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
// (Diablo III, its own faces when present: tabs / section titles in Exocet, descriptions in D3 Serif; NULL otherwise)
static ImFont *g_menu_font_tab, *g_menu_font_desc, *g_menu_font_section;
static float g_font_density = 1;  // (fonts_add: the scale the fonts were rasterized for)
static void fonts_add(float density);
static void fonts_rebuild(float density);
// (Diablo III) the body text face, read once (megabytes); dropped when the player takes the fonts from their PC copy
static void *g_body;
static size_t g_body_n;
static std::atomic<int> g_fonts_reload{0};  // (set by the PC-fonts thread: fonts made again at the next frame)
extern "C" bool d3_pcfonts_extract(const char *dir, char *why, size_t why_n);  // (runtime/d3_pcfonts.c)
extern "C" bool d3_pcfonts_present(void);
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
static bool g_page_fresh_seen;  // (focus_check: the menu opened)
static bool g_page_fresh;  // the page shown hasn't been entered yet: the first Down goes to its first enabled row
static bool g_page_focus_next;  // (that page has no option rows: its first control takes the cursor next frame)
static int g_shown_tab = -1000;  // the page drawn last (its id)
static int g_dpad_tab_req = -2;  // (Diablo III: the tab the d-pad moved onto - applied as g_tab_request next frame)
static int g_no_fresh_tab = -1000;  // (the page L1 / R1 switched to, its first row already given the cursor)
void menu_tabs_focus(bool first_tab) {
    g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0, g_page_fresh = g_page_fresh_seen = true;
}
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    if (game_is_d3() && !getenv("D3_MENU_PLAIN")) {
        // Diablo III's own menus (the tester 2026-11-15): near-black warm panels, bronze and gold trim, ember-red highlights,
        // parchment-coloured text, square corners. (Our own colours and drawing; only the fonts come from the game.)
        s.WindowRounding = 3; s.FrameRounding = 2; s.GrabRounding = 2; s.TabRounding = 2; s.ChildRounding = 2;
        s.ScrollbarRounding = 2; s.TabBarOverlineSize = 2; s.ScrollbarSize = 7;
        ImVec4 d3gold(0.89f, 0.71f, 0.40f, 1), bronze(0.55f, 0.40f, 0.20f, 1), ember(0.36f, 0.12f, 0.05f, 1);
        c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);  // (the game's Options: the list over the game's own picture)
        c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.04f, 0.03f, 0.99f);
        c[ImGuiCol_Border] = bronze;
        c[ImGuiCol_Text] = ImVec4(0.91f, 0.79f, 0.52f, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.41f, 0.35f, 1);
        c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.075f, 0.05f, 1);
        c[ImGuiCol_FrameBgHovered] = ember;
        c[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.17f, 0.06f, 1);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.08f, 0.04f, 1);
        c[ImGuiCol_ButtonHovered] = ember;
        c[ImGuiCol_ButtonActive] = ImVec4(0.48f, 0.17f, 0.06f, 1);
        c[ImGuiCol_Header] = ember;
        c[ImGuiCol_HeaderHovered] = ImVec4(0.44f, 0.15f, 0.06f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.50f, 0.18f, 0.07f, 1);
        c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabHovered] = ImVec4(0, 0, 0, 0);  // (no box: the selected tab has its underline)
        c[ImGuiCol_TabSelected] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
        s.TabBarBorderSize = 0;
        c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
        c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
        c[ImGuiCol_CheckMark] = d3gold;
        c[ImGuiCol_SliderGrab] = bronze;
        c[ImGuiCol_SliderGrabActive] = d3gold;
        c[ImGuiCol_NavCursor] = d3gold;
        c[ImGuiCol_Separator] = ImVec4(0.42f, 0.31f, 0.16f, 1);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.3f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.22f, 0.12f, 1);
        c[ImGuiCol_ScrollbarGrabHovered] = bronze;
        c[ImGuiCol_ScrollbarGrabActive] = d3gold;
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    }
}

// (Diablo III's look) the window's frame: a bronze outer line, a thin gold inner one, and small gold diamonds at the
// corners and the top's middle; the title centred under it with a fading gold rule
static bool menu_d3_look(void) { return game_is_d3() && !getenv("D3_MENU_PLAIN"); }
static void d3_diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
    const ImVec2 p[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    dl->AddConvexPolyFilled(p, 4, fill);
    dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, 1.0f);
}
static void d3_window_frame(float scale) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize(), b(a.x + sz.x, a.y + sz.y);
    ImU32 bronze = IM_COL32(140, 102, 50, 255), gold = IM_COL32(226, 180, 100, 255), dark = IM_COL32(0, 0, 0, 200);
    float t = fmaxf(2.0f, 2.5f * scale), in = 7 * scale;
    // a soft dark band inside the edge (the panel's depth), then the two lines
    for (int i = 0; i < 6; i++) {
        float k = (i + 1) * 3 * scale;
        dl->AddRect(ImVec2(a.x + k, a.y + k), ImVec2(b.x - k, b.y - k), IM_COL32(0, 0, 0, 40 - i * 6), 2, 0, 3 * scale);
    }
    dl->AddRect(ImVec2(a.x + t * 0.5f, a.y + t * 0.5f), ImVec2(b.x - t * 0.5f, b.y - t * 0.5f), bronze, 3, 0, t);
    dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), IM_COL32(200, 156, 84, 170), 2, 0, fmaxf(1.0f, 1.2f * scale));
    float r = 7 * scale;
    const ImVec2 corners[4] = {ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, a.y + in), ImVec2(a.x + in, b.y - in), ImVec2(b.x - in, b.y - in)};
    for (const ImVec2 &c : corners) d3_diamond(dl, c, r, gold, dark);
    d3_diamond(dl, ImVec2((a.x + b.x) * 0.5f, a.y + in), r * 1.3f, gold, dark);
    d3_diamond(dl, ImVec2((a.x + b.x) * 0.5f, b.y - in), r * 1.3f, gold, dark);
}
// (Diablo III's look) the button hints along the bottom, centred, as on its own screens: the face button and a word,
// in the controller's own symbols (PlayStation or Xbox); "Back" closes the menu when clicked
extern "C" int input_pad_family(void);
static void set_open(bool open);
static float d3_hint(ImDrawList *dl, ImFont *f, float fs, float x, float cy, int sym, const char *word, float scale, bool xbox) {
    float r = fs * 0.48f;
    ImVec2 c(x + r, cy);
    dl->AddCircleFilled(c, r, IM_COL32(20, 20, 22, 230));
    dl->AddCircle(c, r, IM_COL32(200, 200, 205, 255), 0, 1.6f * scale);
    ImU32 sc = IM_COL32(235, 235, 240, 255);
    float k = r * 0.42f;
    if (xbox) {
        const char *l = sym == 0 ? "A" : sym == 1 ? "B" : sym == 2 ? "X" : "Y";
        ImVec2 ls = f->CalcTextSizeA(fs * 0.62f, FLT_MAX, 0, l);
        dl->AddText(f, fs * 0.62f, ImVec2(c.x - ls.x * 0.5f, c.y - ls.y * 0.5f), sc, l);
    } else if (sym == 0) {  // Cross
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), sc, 2.2f * scale);
        dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), sc, 2.2f * scale);
    } else if (sym == 1) {  // Circle
        dl->AddCircle(c, k * 1.05f, sc, 0, 2.2f * scale);
    } else {  // L1 / R1 (a small label in the circle's place)
        const char *l = sym == 2 ? (xbox ? "LB" : "L1") : (xbox ? "RB" : "R1");
        ImVec2 ls = f->CalcTextSizeA(fs * 0.5f, FLT_MAX, 0, l);
        dl->AddText(f, fs * 0.5f, ImVec2(c.x - ls.x * 0.5f, c.y - ls.y * 0.5f), sc, l);
    }
    float tx = x + 2 * r + 12 * scale;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, word);
    dl->AddText(f, fs, ImVec2(tx + 1.5f * scale, cy - ts.y * 0.5f + 1.5f * scale), IM_COL32(0, 0, 0, 200), word);
    dl->AddText(f, fs, ImVec2(tx, cy - ts.y * 0.5f), IM_COL32(222, 222, 228, 255), word);
    return tx + ts.x;
}
static void d3_button_hints(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *f = g_menu_font ? g_menu_font : ImGui::GetFont();
    float fs = 29 * scale, cy = io.DisplaySize.y - 52 * scale, gap = 40 * scale;
    bool xbox = d3_prompts_xbox();  // (the "Button icons" setting, else the controller)
    // (measure, then centre)
    float r2 = fs * 0.96f + 12 * scale;
    const char *w[3] = {"Accept", "Back", "Tabs"};
    float tot = 0;
    for (int i = 0; i < 3; i++) tot += r2 + f->CalcTextSizeA(fs, FLT_MAX, 0, w[i]).x + (i < 2 ? gap : 0);
    tot += r2;  // (the second shoulder before "Tabs")
    float x = (io.DisplaySize.x - tot) * 0.5f;
    x = d3_hint(dl, f, fs, x, cy, 0, "Accept", scale, xbox) + gap;
    float bx0 = x;
    x = d3_hint(dl, f, fs, x, cy, 1, "Back", scale, xbox);
    {   // (a click on Back closes the menu, as B does)
        ImVec2 m = io.MousePos;
        if (ImGui::IsMouseClicked(0) && m.x >= bx0 && m.x <= x && fabsf(m.y - cy) < fs) set_open(false);
    }
    x += gap;
    x = d3_hint(dl, f, fs, x, cy, 2, "", scale, xbox) + 6 * scale;
    d3_hint(dl, f, fs, x, cy, 3, "Tabs", scale, xbox);
}

// a horizontal gold rule fading out to both ends, centred at cx
static void d3_rule(ImDrawList *dl, float cx, float y, float half, float th) {
    ImU32 g = IM_COL32(214, 168, 92, 230), z = IM_COL32(214, 168, 92, 0);
    dl->AddRectFilledMultiColor(ImVec2(cx - half, y), ImVec2(cx, y + th), z, g, g, z);
    dl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + half, y + th), g, z, z, g);
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    fonts_add(1.0f);
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

// The fonts, rasterized at `density` times their size: the menu is laid out for 1080p and scaled to the window
// (FontGlobalScale), so at 4K glyphs made for 1080p were stretched 2x - soft text (the tester 2026-11-15). menu_render
// rebuilds them at the window's scale, so each glyph is drawn 1:1.
static void fonts_add(float density) {
    ImGuiIO &io = ImGui::GetIO();
    g_font = g_font_big = g_menu_font = g_menu_font_big = g_font_title = NULL;
    g_menu_font_tab = g_menu_font_desc = g_menu_font_section = NULL;
    g_font_density = density;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.RasterizerDensity = density;
    char dejavu[1024];  // none of the system's: the bundled DejaVu Sans (third_party/fonts/DejaVu, in every release)
    const char *bundled = rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", dejavu, sizeof dejavu) ? dejavu : NULL;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", bundled};
    for (const char *p : paths) {
        if (!p) continue;
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    {   // (Diablo III) the game's own title face for "Loading...", if the player put it in the build folder (made from their
        // own PC copy by src/analysis tools; never shipped): build/d3_title_font.otf. Else the font above.
        // Else (a shared build: the game's fonts can't ship) Marcellus SC, a free look-alike (OFL; the tester 2026-11-17 "use
        // option 3": Marcellus SC + EB Garamond).
        char tfb[1024] = "build/d3_title_font.otf";
        const char *tf = tfb;
        FILE *tff = fopen(tf, "rb");
        if (!tff && game_is_d3() &&
            rt_resource("MarcellusSC-Regular.ttf", "third_party/fonts/MarcellusSC/MarcellusSC-Regular.ttf", tfb, sizeof tfb))
            tff = fopen(tf, "rb");
        if (FILE *f = tff) {
            fclose(f);
            ImFontConfig t = cfg;
            t.FontDataOwnedByAtlas = true;
            g_font_title = io.Fonts->AddFontFromFileTTF(tf, 96, &t);
            // (the menu in Diablo III's look, menu_d3_look: its title, tabs and section titles in the same face)
            if (game_is_d3()) {
                g_menu_font_big = io.Fonts->AddFontFromFileTTF(tf, 54, &t);
                g_menu_font_tab = io.Fonts->AddFontFromFileTTF(tf, 31, &t);
                g_menu_font_section = io.Fonts->AddFontFromFileTTF(tf, 25, &t);
            }
        }
    }
    {   // (Diablo III) its body text face, from the player's PC copy (never shipped): the sans its console Options screen
        // uses (build/d3_sans_font.ttf, BlizzardGlobal; the tester 2026-11-15 "the same style like this menu"), else D3 Serif
        // (build/d3_body_font.ttf). Megabytes (every script): read once and shared by both sizes; only the Latin glyphs
        // are rasterized.
        if (game_is_d3() && !g_body) {
            FILE *f = fopen("build/d3_sans_font.ttf", "rb");
            if (!f) f = fopen("build/d3_body_font.ttf", "rb");
            char eb[1024];  // (else EB Garamond, a free look-alike: OFL)
            if (!f && rt_resource("EBGaramond.ttf", "third_party/fonts/EBGaramond/EBGaramond.ttf", eb, sizeof eb)) f = fopen(eb, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                g_body = n > 0 ? malloc((size_t)n) : NULL;
                if (g_body && fread(g_body, 1, (size_t)n, f) == (size_t)n) g_body_n = (size_t)n;
                else free(g_body), g_body = NULL;
                fclose(f);
            }
        }
        if (g_body && g_menu_font_big) {
            ImFontConfig b = cfg;
            b.FontDataOwnedByAtlas = false;
            g_menu_font = io.Fonts->AddFontFromMemoryTTF(g_body, (int)g_body_n, 31, &b);
            g_menu_font_desc = io.Fonts->AddFontFromMemoryTTF(g_body, (int)g_body_n, 29, &b);
        }
    }
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
}

#ifndef __APPLE__
extern "C" void vk_imgui_fonts_reupload(void);  // (nvn/vulkan/imgui_glue.cpp)
#endif
static void fonts_rebuild(float density) {
    ImGui::GetIO().Fonts->Clear();
    fonts_add(density);
#ifndef __APPLE__
    vk_imgui_fonts_reupload();
#endif
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    if (g_clear_keys.exchange(false)) io.ClearInputKeys();
    {   // (test) LARECOMP_MENU_DOWN=<n>: n presses of Down, one every 20 menu frames after it opens (a row selected
        // in hidden test shots)
        // LARECOMP_MENU_KEYS=<letters>: the same, any keys in turn (D U L R down / up / left / right, A Cross, B Circle,
        // E Esc, . nothing)
        static int downs = -1, tick, ki;
        static const char *keys;
        if (downs < 0) downs = getenv("LARECOMP_MENU_DOWN") ? atoi(getenv("LARECOMP_MENU_DOWN")) : 0, keys = getenv("LARECOMP_MENU_KEYS");
        if (downs > 0 && g_open.load()) {
            tick++;
            if (tick % 20 == 10) io.AddKeyEvent(ImGuiKey_DownArrow, true);
            if (tick % 20 == 12) { io.AddKeyEvent(ImGuiKey_DownArrow, false); downs--; }
        } else if (keys && keys[ki] && (g_open.load() || ki)) {
            static const char *names = "DULRABET";  // (T: R1, the next tab)
            static const ImGuiKey kk[] = {ImGuiKey_DownArrow, ImGuiKey_UpArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                                          ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_Escape,
                                          ImGuiKey_GamepadR1};
            const char *at = strchr(names, keys[ki]);
            tick++;
            if (at && tick % 20 == 10) { io.AddKeyEvent(kk[at - names], true); LOG("[menu] test key %c\n", keys[ki]); }
            if (tick % 20 == 12) { if (at) io.AddKeyEvent(kk[at - names], false); ki++; }
        }
    }
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               game_is_d3() ? "Unlocked: the view fills the window, so wider screens show more of the world, up to "
                              "ultrawide. Original 16:9: the game's own framing, with bars."
                            : "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
                              "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
    {   // (the tester 2026-11-15: exclusive full screen, borderless windowed, windowed)
        static const char *dm[] = {"Windowed", "Borderless windowed", "Exclusive fullscreen"};
        if (opt_choice("Display mode", &g_settings.display_mode, dm, 3, DISPLAY_BORDERLESS,
                       "Windowed: a movable window. Borderless windowed: the whole screen, quick to switch away from. "
                       "Exclusive fullscreen: the whole screen, can be a little smoother. F11 / Alt+Enter toggles.", false)) {
            g_settings.fullscreen = g_settings.display_mode != DISPLAY_WINDOWED;
            g_want_fullscreen_apply.store(true);
        }
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    // (Diablo III: the usual TV / monitor resolutions by name, the tester 2026-11-14)
    // (6K / 8K: supersampling - drawn larger than a 4K screen and scaled down, smoother edges; the tester 2026-11-15. In game
    // on an RTX 5080, unpaced: ~100 fps at 6K, ~135 at 8K)
    // (the tester 2026-11-15: just three, each drawn at twice its size and scaled down - 1080p draws 4K, 1440p 6K, 4K 8K)
    // (the tester 2026-11-15, lower-end PCs: "Native" first - the screen's own size, no supersampling, a quarter of 1080p's work
    // on a 1080p screen)
    // (named by the size actually drawn - the tester 2026-11-15 "don't fool people that 1080p is 1080p when it's not": modes 2,
    // 3, 4 draw the game's 900-line targets at 2160, 3240 and 4320 lines whatever the screen; [post] in the log)
    // ("Native" says what it is on this screen: on a 4K screen it is the same as 4K - the tester 2026-11-15 "native still
    // looks sharp")
    static char native[32];
    {
        int lines = (int)(ImGui::GetIO().DisplaySize.y + 0.5f);
        snprintf(native, sizeof native, lines >= 2100 && lines <= 2200 ? "Native (4K)" : "Native (%dp)", lines);
    }
    // (and smaller ones for weaker PCs - the tester 2026-11-15 "add options for lower end PCs like 1080p, 1440p": 720p, 1080p
    // (1x = 1080 lines), 1440p, each exactly that size)
    const char *res[] = {native, "720p", "1080p", "1440p", "4K", "6K", "8K"};
    static const int res_mode[] = {RES_MATCH, RES_P720, 1, RES_P1440, 2, 3, 4};
    enum { RES_MATCH_AT = 0 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT, rdef = RES_MATCH_AT;  // (rdef: the GPU's default - Dynamic on an integrated GPU)
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        (void)sx; (void)sy;
        snprintf(d, sizeof d, "The size the game is drawn at. Native: your screen's own resolution, the lightest, for "
                              "weaker PCs. 4K, 6K, 8K: drawn that large and scaled down to your screen, for smoother "
                              "edges. The bigger, the stronger the graphics card it needs.");
        if (opt_choice("Internal resolution", &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    // (Diablo III: no Upscaler / Sharpening / Game blur filter / Anti-aliasing rows - fixed at the values that looked
    // best, settings_apply_game_fixed - and frame rates 60 / 120; the tester 2026-11-15. D3_VIDEO_ALL=1: every row)
    bool d3fixed = game_is_d3() && !getenv("D3_VIDEO_ALL");
    if (!d3fixed) {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    {   // (Diablo III: sharpening, the tester 2026-11-14)
        static const char *sh[] = {"Off", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
        if (!d3fixed)
        opt_choice("Sharpening", &g_settings.sharpen, sh, 11, 5,
                   "Makes the image crisper (AMD FidelityFX CAS-style sharpening). Higher is sharper; too high can look "
                   "grainy. 5 is a good start.");
        {   // (Diablo III: 120 / 240 frames a second - the game keeps real time, it draws each frame anew; the tester
            // 2026-11-15)
            // (the tester 2026-11-17: 60 is the default; 120 stays as an option with a warning - the game steps its world 60
            // times a second and moving characters can show a double image at 120, hle/d3_clock.c, hle/d3_monitor.c)
            static const char *fr[] = {"60", "120", "240"};
            static const char *fr_d3[] = {"60", "120 (experimental)"};
            int fi = g_settings.frame_rate >= 240 ? 2 : g_settings.frame_rate >= 120 ? 1 : 0;
            if (opt_choice("Frame rate", &fi, d3fixed ? fr_d3 : fr, d3fixed ? 2 : 3, 0,
                           d3fixed ? "Frames a second. 60 is what the game was made for and the recommended setting.\n\n"
                                     "120 is smoother on a fast screen and needs a strong PC, but it is experimental: "
                                     "moving characters can show ghosting (a double image) now and then. The game "
                                     "corrects it by itself within about a second; if it bothers you, use 60."
                                   : "Frames a second. 120 and 240 are smoother on a fast screen and need a strong PC; pick one "
                                     "your screen shows (its refresh rate or half of it)."))
                g_settings.frame_rate = fi == 2 ? 240 : fi == 1 ? 120 : 60;
        }
        static const char *hd[] = {"Off", "On"};
        opt_choice("HD textures", &g_settings.hd_textures, hd, 2, 1,
                   "AI-upscaled textures from the hdtex folder. Changes show as new areas load.");
        if (game_is_d3()) {  // (the tester 2026-11-17: "give the player an option to extract the menu fonts from their pc copy")
            static std::atomic<int> state{0};  // 0 idle, 1 picking / reading, 2 done, 3 failed
            static char why[300];
            int st = state.load();
            const char *val = st == 1 ? "Reading..." : d3_pcfonts_present() ? "From your PC copy" : "Built in";
            if (opt_action("Menu fonts", val,
                           st == 3 ? why
                                   : "This menu's lettering. Built in: free fonts in the same style. If you also have "
                                     "Diablo III on PC, press to pick its install folder (for example C:\\Program Files "
                                     "(x86)\\Diablo III): Diablo's own fonts are copied from it, on this PC only.",
                           false, false) && st != 1) {
                state = 1;
                SDL_ShowOpenFolderDialog(
                    [](void *, const char *const *files, int) {
                        if (!files || !files[0]) { state = 0; return; }
                        std::string dir = files[0];
                        std::thread([dir] {
                            bool ok = d3_pcfonts_extract(dir.c_str(), why, sizeof why);
                            if (ok) g_fonts_reload = 1;
                            state = ok ? 2 : 3;
                        }).detach();
                    },
                    NULL, NULL, NULL, false);
            }
        }
        static const char *fx[] = {"Off", "On"};
        if (!d3fixed)
        opt_choice("Game blur filter", &g_settings.game_fxaa, fx, 2, 0,
                   "The console version's own edge blur (FXAA). Off is crisper, like the PC version; Anti-aliasing (SMAA) "
                   "or 6K smooth the edges instead. Takes effect after a restart.");
    }
    static const char *aa[] = {"Off", "SMAA"};
    if (!d3fixed)
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");

    if (game_is_d3()) {  // (the tester 2026-11-17: the name under P1 - the player, not the hero; services/acc.c)
        opt_section("Player");
        opt_text("Player name", g_settings.player_name, sizeof g_settings.player_name, "Your PC's user name",
                 "Your name in the game: shown under P1 on the main menu (on a Switch, your account's name - your heroes "
                 "keep their own names). Empty: this PC's user name. Shows from the next start of the game.");
        opt_end();
        return;
    }  // (the rows below are Link's Awakening's)
    opt_section("Game");
    opt_toggle("Remove blur", &g_settings.blur_removal, false,
               "Turns off the tilt-shift blur at the top and bottom of the screen.");
    opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
               "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    opt_end();
}

extern "C" bool cosmetics_seed_on(void);  // runtime/cosmetics_seed.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" fills the rows left on Default (shown as "Seed: <colour>")
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand};
    if (opt_button("Randomize all", "Every color below gets a random color."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Everything on this page back to the game's own look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
    }
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10); "Mixed" when
        // they match none
        static const char *const names[] = {"Spring", "Autumn", "Winter", "Cherry blossom", "Mixed"};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;  // (as shown: a row on Default shows the seed's colour, if it has its own)
        for (int i = 0; i < 4; i++)
            if (col_pick(&g_settings.leaves, COL_SLOT_LEAVES) == preset[i][0] &&
                col_pick(&g_settings.grass, COL_SLOT_GRASS) == preset[i][1] &&
                col_pick(&g_settings.sand, COL_SLOT_SAND) == preset[i][2])
                cur = i;
        int v = cur;
        if (opt_choice("Season", &v, names, 5, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            if (v == 4) v = cur == 0 ? 3 : 0;  // (Mixed is never chosen: past the ends, back round)
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and the sandy south.");
    opt_section("Enemies");
    {   // (Off in a seed with its own cosmetics: "Seed", its enemies are per seed)
        const char *modes[N_ENEMY_COLOUR_MODES];
        for (int i = 0; i < N_ENEMY_COLOUR_MODES; i++) modes[i] = g_enemy_colour_modes[i];
        if (cosmetics_seed_on()) modes[0] = "Seed";
        opt_choice("Enemy colors", &g_settings.enemy_colours, modes, N_ENEMY_COLOUR_MODES, 0,
                   "Each kind of enemy in another color. Enemies whose color shows their strength or type keep theirs.");
    }
    opt_section("Screen");
    opt_choice("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "A filter over the whole picture, the HUD included.");
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    // (Diablo III: "Extra" - the game has its own Music / Effects volumes in its Options; these sit on top of them.
    // the tester 2026-11-15: two places called the same thing was confusing; his levels that day are the 100%s)
    if (opt_slider(game_is_d3() ? "Extra volume" : "Volume", &pct, 0, 200, 5, 100, "%g%%",
                   game_is_d3() ? "All sound, on top of the game's own volumes in Options. 100% is the game's own level."
                                : "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    if (game_is_d3()) {  // (runtime/hle/d3_volume.c: the game's Music channel group, changed at once)
        float mp = roundf(g_settings.music_volume / D3_MUSIC_BASE * 100);
        if (opt_slider("Extra music volume", &mp, 0, 200, 5, 100, "%g%%",
                       "The music, on top of the game's own Music volume in Options. 100% is the PC balance; 0% is "
                       "no music."))
            g_settings.music_volume = mp / 100.0f * D3_MUSIC_BASE;
    }
    {   // (the tester 2026-11-15: "a way to set sound as mono or stereo" - services/audio.c mixes it down)
        const char *modes[] = {"Stereo", "Mono"};
        opt_choice("Sound", &g_settings.sound_mono, modes, 2, 0,
                   "Stereo: left and right speakers. Mono: the same sound from both, for one speaker or one ear.");
    }
    if (!game_is_d3())
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
void opt_focus_first_next(void);  // option_rows.mm
// (Circle / Esc on "Yes / Cancel" goes back to the two choices; it closed the whole menu too - the tester 2026-11-17)
static bool g_quit_took_back;
static bool quit_tab_took_back(void) { bool t = g_quit_took_back; g_quit_took_back = false; return t; }
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    int ask_was = ask;
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0, g_quit_took_back = true, ui_sound(UI_SND_CLOSE);  // (the back click)
    const char *lost = in_game ? "\n\nProgress since your last save is lost." : "";
    char d1[200], d2[200];
    bool d3 = game_is_d3();
    snprintf(d1, sizeof d1, d3 ? "Restarts the game at its title screen.%s" : "Leaves the game and goes back to the LAS Recomp start screen.%s", lost);
    snprintf(d2, sizeof d2, d3 ? "Closes the game.%s" : "Closes LAS Recomp.%s", lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action(d3 ? "Quit to Title Screen" : "Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? (d3 ? "Yes, quit to Title Screen" : "Yes, quit to Start Screen") : "Yes, quit to Desktop", "",
                       ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(d3 ? 1 : 2);  // (RT_REBOOT_GAME: boot again at once; RT_REBOOT_TITLE: the start screen)
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
    if (ask != ask_was && ImGui::GetCurrentContext()->NavCursorVisible) opt_focus_first_next();  // (the new rows' first)
}

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" bool tracker_dungeon_wanted(void);
extern "C" void tracker_dungeon_draw(ImVec2 display, ImFont *font);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool race_overlay_wanted(void);
extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool pickup_flyin_wanted(void);  // (silent pickups: pickup_flyin.mm)
extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool race_overlay_key(char *out, size_t cap);

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static char g_ov_key[96];
static int g_ov_same;  // frames built in a row with g_ov_key
static float g_fps_dt[60];
static int g_fps_n, g_fps_i;
static double g_fps_sum, g_fps_shown, g_fps_at;
static void fps_note(double now, float dt) {
    if (g_fps_n == 60) g_fps_sum -= g_fps_dt[g_fps_i];
    else g_fps_n++;
    g_fps_dt[g_fps_i] = dt;
    g_fps_sum += dt;
    g_fps_i = (g_fps_i + 1) % 60;
    if (now - g_fps_at >= 0.5 || now < g_fps_at) {
        g_fps_at = now;
        g_fps_shown = g_fps_sum > 0 ? g_fps_n / g_fps_sum : 0;
    }
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        int units = kbd_cut(g_kbd_text, g_kbd_max);
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", units, g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    kbd_cut(g_kbd_text, g_kbd_max);  // (whatever entered it: typed, on-screen keys, the test hook)
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// LARECOMP_MENU_FOCUS_LOG=1 (test; tools/menu_focus_sweep.sh): every Down / Up on a page of option rows is checked a
// few frames later against the rows' order and logged "[menu] focus check ... OK / FAIL": the first Down after a tab
// switch (however it came) or the menu opening, and Down from the tab bar, land on the page's first enabled row; Down
// goes to the next row (the last one stays), Up to the row before, and Up from the top row onto the selected tab.
static void focus_check(bool down, bool up, bool any_down, int focused_tab, int tab, const char *tab_name) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_MENU_FOCUS_LOG") != NULL;
    if (!on) return;
    static int shown = -1000, due_in, want, from, prev_focus = -1;  // (want: a row index, -1: the selected tab)
    static bool first_down_due;
    static const char *what;
    if (tab != shown || g_page_fresh_seen) shown = tab, first_down_due = true, g_page_fresh_seen = false, prev_focus = -1;
    int n = opt_row_count(), first = opt_first_row(), focus = opt_focused_row();
    if (any_down && !down) first_down_due = false;  // (a held Down's repeat: the first Down was checked or skipped)
    if ((down || up) && !due_in && n > 0) {
        from = prev_focus, due_in = 4;
        if (down && (first_down_due || from < 0)) want = first, what = first_down_due ? "first Down" : "Down from the tab bar";
        else if (down) want = from + 1 < n ? from + 1 : from, what = "Down";
        else if (from > 0) want = from - 1, what = "Up";
        else if (from == 0) want = -1, what = "Up from the top row";
        else due_in = 0;  // (Up on the tab bar: not a page move)
        if (down) first_down_due = false;
    } else if (down && n == 0 && first_down_due) {
        first_down_due = false;
        LOG("[menu] focus check: first Down on %s SKIP (no option rows)\n", tab_name);
    }
    prev_focus = focus;
    if (!due_in || --due_in) return;
    bool ok = want >= 0 ? focus == want : focused_tab == tab;
    char got[160];
    if (focus >= 0) snprintf(got, sizeof got, "\"%s\"", opt_row_label(focus));
    else snprintf(got, sizeof got, "tab %d", focused_tab);
    char exp[160];
    if (want >= 0) snprintf(exp, sizeof exp, "\"%s\"", opt_row_label(want));
    else snprintf(exp, sizeof exp, "tab %d", tab);
    LOG("[menu] focus check: %s on %s from %s -> %s (want %s) %s\n", what, tab_name,
        from >= 0 ? opt_row_label(from) : "the tab bar", got, exp, ok ? "OK" : "FAIL");
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    static const char *names[] = {"Video", "Audio", "Save Editor", "Randomizer", "Warp", "Cheats", "Gameplay",
                                  "Cosmetics", "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 6, 7, 3, 5, 4, 2, 9};  // Controls, Video, Audio, Gameplay, Cosmetics, Randomizer,
                                                                // Cheats, Warp, Save Editor, Quit (ids stay: test hooks)
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
        if ((i != 9 || !prelaunch) && !(game_is_d3() && i >= 2 && i <= 7))  // (Diablo III: Link's Awakening tabs out)
            tabs[ntabs++] = {names[i], i, in_game || (i != 2 && i != 4 && i != 5)};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    if (g_dpad_tab_req != -2) { g_tab_request = g_dpad_tab_req; g_dpad_tab_req = -2; }  // (g_tab_request is cleared
                                                                                       // after the tab bar each frame)
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            ui_sound(UI_SND_TAB);
            { extern void opt_quiet_move_sound(void); opt_quiet_move_sound(); }  // (one sound per switch)
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
        }
    }
    // (Diablo III: after L / R the cursor goes straight onto the new page's first row, as the game's own menus - not
    // onto the tab, needing a Down; the tester 2026-11-15. Next frame: the new page is drawn then)
    static int first_row_after_switch;
    bool auto_first = first_row_after_switch == 1 && !switching;
    if (first_row_after_switch) first_row_after_switch--;
    if (switching && game_is_d3()) first_row_after_switch = 1, g_no_fresh_tab = g_tab_request;  // (the new page shows
        // a frame or two later; the cursor is put on its first row meanwhile, so it isn't "fresh" when it does: the
        // next Down goes on to the second row - the tester 2026-11-15, Down needed twice after L1 / R1)
    bool down = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    bool up = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
    // The first Down on a page just switched to (L / R, a click or A on a tab, the menu opening), or Down with the
    // cursor on the tab bar: the page's first enabled row, top to bottom - never ImGui's pick from wherever its cursor
    // was (a row of the page before, the tab bar's x). Option rows take it (opt_focus_first); a page without them gets
    // its first control (g_page_focus_next).
    if (ImGui::GetIO().MouseClicked[0]) g_page_fresh = false;  // (the mouse went into the page)
    // (the tester 2026-11-15: on a tab, Down had to be pressed twice - with ImGui's cursor hidden (after the mouse moved or
    // clicked) its first Down only shows the cursor. Now Down on a tab, or with the cursor hidden and on no row, goes
    // straight to the first row)
    bool nav_shown = ImGui::GetCurrentContext()->NavCursorVisible;
    bool down_from_tab = (down && !switching &&
                          (focused_tab != -99 || g_page_fresh || (!nav_shown && opt_focused_row() < 0))) ||
                         auto_first;
    if (down && getenv("D3_LOG_UI_SOUNDS"))
        LOG("[menu] Down: tab %d, row %d, cursor %s, page fresh %d -> %s\n", focused_tab, opt_focused_row(),
            nav_shown ? "shown" : "hidden", (int)g_page_fresh, down_from_tab ? "first row" : "next row / ImGui");
    if (down_from_tab) {
        ImGui::NavMoveRequestCancel();  // (ImGui's own move this frame)
        g_page_fresh = false;
        // (after L1 / R1 the cursor is put on the first row while the new page shows for the first time: that page is
        // not "fresh" any more, or the next Down went to the first row again - the tester 2026-11-15, Down needed twice)
        g_focus_tab = -99, g_focus_frames = 0;  // (the cursor stops going back to the tab)
    }
    opt_focus_first(down_from_tab);
    // Up / Down on a page of option rows: the next / previous row in order (opt_nav_step), not ImGui's spatial pick
    if ((down || up) && !down_from_tab && !switching && opt_nav_step(down ? 1 : -1)) ImGui::NavMoveRequestCancel();
    opt_frame_reset();
    // The cursor moved out of the page (ImGui's pick, applied this frame for last frame's press - Up from the page's
    // top control): onto the selected tab. ImGui's pick was the tab nearest the control's x (Controls, from most
    // rows), or the close button
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *root = ImGui::GetCurrentWindow()->RootWindow;
    static bool nav_in_page;  // (the frame before: the cursor on a control of the page - its child windows)
    if (nav_in_page && g.NavJustMovedToId && g.NavWindow == root && !any_popup) g_focus_tab = -100, g_focus_frames = 0;
    // (Diablo III: Left / Right with the cursor on the tab bar - the tab it moves to opens, a few frames' window for
    // ImGui to move it. Only then: at the menu's opening the cursor sits on another tab than the page shown, and
    // following the cursor regardless switched tabs back and forth every frame)
    static int lr_window;
    {
        bool lr = !any_popup && !typing &&
                  (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
                   ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow));
        if (lr && focused_tab != -99) lr_window = 5;
        else if (lr_window) lr_window--;
    }
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s " : "  %s  ", t.name);  // (ten tabs: tighter, so none is cut short)
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImVec4 lc = menu_d3_look() ? ImVec4(0, 0, 0, 0) : ImVec4(0.19f, 0.27f, 0.22f, 1);
                ImGui::PushStyleColor(ImGuiCol_Tab, lc);
                ImGui::PushStyleColor(ImGuiCol_TabHovered, lc);
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool d3 = menu_d3_look();
            if (g_menu_font_tab && d3) ImGui::PushFont(g_menu_font_tab);  // (Diablo III: tabs in its face)
            if (d3) ImGui::PushStyleColor(ImGuiCol_Text, t.id == g_tab ? ImVec4(0.99f, 0.98f, 0.95f, 1) : ImVec4(0.91f, 0.79f, 0.52f, 1));
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            if (d3) ImGui::PopStyleColor();
            if (g_menu_font_tab && d3) ImGui::PopFont();
            if (d3 && open) {  // (the selected tab: a cyan streak under it, brightest in the middle)
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float cx = (a.x + b.x) * 0.5f, half = (b.x - a.x) * 0.6f, th = fmaxf(2.0f, 2.5f * scale), y = b.y - th;
                ImDrawList *tdl = ImGui::GetWindowDrawList();
                ImU32 cyan = IM_COL32(120, 235, 245, 255), z = IM_COL32(120, 235, 245, 0);
                tdl->AddRectFilledMultiColor(ImVec2(cx - half, y), ImVec2(cx, y + th), z, cyan, cyan, z);
                tdl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + half, y + th), cyan, z, z, cyan);
                ImU32 glow = IM_COL32(40, 190, 210, 60), gz = IM_COL32(40, 190, 210, 0);
                tdl->AddRectFilledMultiColor(ImVec2(cx - half, y - 10 * scale), ImVec2(cx, y + th), gz, gz, glow, gz);
                tdl->AddRectFilledMultiColor(ImVec2(cx, y - 10 * scale), ImVec2(cx + half, y + th), gz, gz, gz, glow);
            }
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit && !menu_d3_look()) {  // (Diablo III: the cursor on a tab shows as its white text, no bar)
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          menu_d3_look() ? IM_COL32(120, 235, 245, 255)
                                                                         : IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            // (Diablo III: the d-pad along the tab bar opens the tab the cursor goes to, as the game's own menus - it
            // only lit it before, the page below staying the old one; the tester 2026-11-15)
            if (game_is_d3() && !prelaunch && lr_window > 0 && t.on && focused_tab == t.id && t.id != g_tab &&
                !focus_here && !any_popup && !typing && ImGui::GetCurrentContext()->NavCursorVisible) {
                lr_window = 0;
                g_dpad_tab_req = t.id;
                ui_sound(UI_SND_TAB);
                extern void opt_quiet_move_sound(void);
                opt_quiet_move_sound();
            }
            ImGui::EndDisabled();
            if (open) {
                if (t.id != g_shown_tab) {
                    g_shown_tab = t.id, g_page_fresh = t.id != g_no_fresh_tab, g_page_focus_next = false;
                    g_no_fresh_tab = -1000;
                }
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                if (g_page_focus_next) {  // (a page without option rows: its first control)
                    g_page_focus_next = false;
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2:
                    if (game_in_play()) save_editor_draw(scale);
                    else ImGui::TextDisabled("Load a save file and get into the game to edit it.");
                    break;
                case 3: rando_tab_draw(scale); break;
                case 4: if (warp_tab_draw(scale)) set_open(false); break;
                case 5: cheats_draw(scale); break;
                case 6: difficulty_draw(scale); break;
                case 7: tab_cosmetics(); break;
                case 8: controls_tab_draw(scale); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                // no enabled option row took the cursor: the page's first control, next frame (SetKeyboardFocusHere:
                // the first item that takes the cursor, in order)
                if (opt_focus_first_left() && down_from_tab) g_page_focus_next = true;
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first_left();  // (no page drawn: nothing took it)
        ImGui::EndTabBar();
    }
    nav_in_page = g.NavWindow && g.NavWindow != g.NavWindow->RootWindow && g.NavWindow->RootWindow == root;
    // (checks: fresh presses only - a held key's repeats come faster than a check's few frames)
    bool down1 = down && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    bool up1 = up && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false));
    for (int k = 0; k < ntabs; k++)
        if (tabs[k].id == g_tab) focus_check(down1, up1, down, focused_tab, g_tab, tabs[k].name);
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card).
// (Diablo III: "Loading" in the window while the game boots - the first ~7 s showed a black window that looked frozen,
// the tester 2026-11-15; device.c ui_run draws one such frame before the game starts)
extern "C" bool g_menu_loading;
bool g_menu_loading;

// The loading mark (Diablo III): the game's logo in the middle of the screen with three gold diamonds lighting in turn
// under it - the tester 2026-11-15, "instead of LOADING put that in the middle". The logo is the player's own picture
// (build/d3_ui/logo.rgba, made by the local src/analysis/d3_logo.py; never shipped); without it, the word LOADING.
// `a`: how far faded in (0..1).
static void loading_mark(ImDrawList *dl, ImVec2 disp, float scale, float a, bool dots) {
    static ImTextureID logo;
    static int lw, lh;
#ifndef __APPLE__
    static bool tried;
    static float hk = 0.40f;  // (its height on screen, of the screen's)
    extern volatile int g_d3_logo_ready;  // (nvn/texpatch.c: the title screen's logo just saved - load it now)
    if (g_d3_logo_ready == 1) g_d3_logo_ready = 2, tried = false;
    if (!tried) {
        tried = true;
        // the game's own logo, saved from the title screen by texpatch.c (the player's copy), else a picture of it
        // the player put in build/d3_ui (local)
        {   // first: one carried inside the program (a private build, texpatch.c texpatch_d3_embedded_logo)
            uint32_t ew = 0, eh = 0;
            uint8_t *ep = texpatch_d3_embedded_logo(&ew, &eh);
            if (ep) {
                logo = (ImTextureID)vk_imgui_texture_rgba(ep, (int)ew, (int)eh), lw = (int)ew, lh = (int)eh, hk = 0.40f;
                free(ep);
                LOG("[menu] loading logo: inside the program\n");
                return loading_mark(dl, disp, scale, a, dots);
            }
        }
        bool own = true;
        FILE *f = fopen("cache/d3_title_logo.rgba", "rb");
        if (!f) f = fopen("build/d3_ui/logo.rgba", "rb"), own = false;
        uint32_t wh[2];
        if (f && fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 4096 && wh[1] <= 4096) {
            std::vector<uint8_t> px((size_t)wh[0] * wh[1] * 4);
            if (fread(px.data(), 1, px.size(), f) == px.size())
                logo = (ImTextureID)vk_imgui_texture_rgba(px.data(), (int)wh[0], (int)wh[1]), lw = (int)wh[0], lh = (int)wh[1],
                hk = own ? 0.60f : 0.40f;  // (the game's has more of its glow around it)
        }
        if (f) fclose(f);
        LOG("[menu] loading logo: %s\n", logo ? "loaded" : "not there (the word LOADING instead)");
    }
#endif
    float r = 7 * scale, gap = 22 * scale, cx = disp.x * 0.5f, dots_y;
    if (logo) {
        float h = disp.y * hk, w = h * lw / lh, y0 = disp.y * 0.5f - h * 0.58f;
        dl->AddImage(logo, ImVec2(cx - w * 0.5f, y0), ImVec2(cx + w * 0.5f, y0 + h), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, (int)(a * 255)));
        dots_y = y0 + h + 26 * scale;
    } else {
        ImFont *f = g_font_title ? g_font_title : g_font_big ? g_font_big : ImGui::GetFont();
        const char *txt = g_font_title ? "LOADING" : "Loading";
        float fs = 46 * scale;
        ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0, txt);
        dl->AddText(f, fs, ImVec2(cx - sz.x * 0.5f + 2 * scale, (disp.y - sz.y) * 0.5f + 2 * scale), IM_COL32(0, 0, 0, (int)(a * 220)), txt);
        dl->AddText(f, fs, ImVec2(cx - sz.x * 0.5f, (disp.y - sz.y) * 0.5f), IM_COL32(240, 202, 128, (int)(a * 255)), txt);
        dots_y = (disp.y + sz.y) * 0.5f + 30 * scale;
    }
    double t = SDL_GetTicks() / 1000.0;
    // (the diamonds on the black loading screens - the game starting, Quit to Title - not on the fade over the game
    // while it loads; the tester 2026-11-15. Without a logo, always: they show it isn't frozen)
    for (int i = 0; i < (dots || !logo ? 3 : 0); i++) {
        float ph = (float)fmod(t * 1.6 - i * 0.22, 1.0), lit = ph < 0.5f ? 1 - ph * 2 : 0;  // (each in turn)
        ImVec2 c(cx + (i - 1) * gap, dots_y);
        const ImVec2 p[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
        int g = (int)(130 + 125 * lit);
        dl->AddConvexPolyFilled(p, 4, IM_COL32(g, (int)(g * 0.75f), (int)(g * 0.42f), (int)(a * 255)));
        dl->AddPolyline(p, 4, IM_COL32(0, 0, 0, (int)(a * 160)), ImDrawFlags_Closed, 1.0f);
    }
}
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin) {
    extern float g_stall_fade, g_fade_in;  // (Diablo III's loading fade: nvn/vulkan/device.c d3_stall_watch)
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin || g_menu_loading ||
           g_stall_fade > 0 || g_fade_in > 0;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    if (g_open_request.exchange(false)) { g_open_quiet = true; set_open(true); g_open_quiet = false; }
    bool open = g_open.load();
    bool inv = !open && inventory_overlay_wanted();
    bool trk = !open && tracker_overlay_wanted();
    bool trkd = !open && tracker_dungeon_wanted();
    bool chaos = !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool race = race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    bool flyin = pickup_flyin_wanted() && !open;  // (a silent pickup's card)
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin)) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        g_ov_same = 0;  // (shown again: built afresh first)
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    char key[sizeof g_ov_key];
    key[0] = 0;
    if (g_ov_cache) {
        fps_note(now, io.DeltaTime);
        char rk[64];
        // (only the FPS counter and the run timer; input only reaches ImGui while the menu or the name entry is up)
        extern float g_stall_fade, g_fade_in;  // (the loading mark animates: never a cached picture of it)
        if (!open && !kbd && !inv && !trk && !trkd && !chaos && !flyin && !g_menu_loading && g_stall_fade <= 0 &&
            g_fade_in <= 0 && race_overlay_key(rk, sizeof rk)) {
            pthread_mutex_lock(&q_lock);
            bool events = !q_events.empty();
            pthread_mutex_unlock(&q_lock);
            if (!events) snprintf(key, sizeof key, "%.0fx%.0f fps %d %.0f | %s", tw, th, (int)g_settings.show_fps,
                                  g_settings.show_fps ? g_fps_shown : 0.0, rk);
        }
        ImDrawData *dd = ImGui::GetDrawData();
        if (key[0] && g_ov_same >= 3 && !strcmp(key, g_ov_key) && dd && dd->Valid) {
            g_text_busy = false;  // (the menu is closed)
#ifdef __APPLE__
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            ImGui_ImplMetal_NewFrame(rp);  // (the backend's target description only)
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
            [enc endEncoding];
#else
            void *oc = vk_overlay_begin(cmd);  // (as below)
            ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)oc);
            vk_overlay_end(cmd, oc);
#endif
            return;
        }
        if (key[0] && !strcmp(key, g_ov_key)) g_ov_same++;
        else { memcpy(g_ov_key, key, sizeof key); g_ov_same = key[0] ? 1 : 0; }
    }
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifndef __APPLE__
    {   // the fonts at the window's scale (fonts_add), in steps of 1/8 (a resized window doesn't rebuild every frame);
        // D3_MENU_SOFT_TEXT=1: as before (made for 1080p, stretched)
        float d = roundf(scale * 8) / 8;
        if (d < 0.5f) d = 0.5f;
        if (d > 4) d = 4;
        if (g_fonts_reload.exchange(0)) {  // (the PC copy's fonts were just written: build/d3_*_font)
            free(g_body), g_body = NULL, g_body_n = 0;
            fonts_rebuild(g_font_density > 0 ? g_font_density : d);
        }
        if (!getenv("D3_MENU_SOFT_TEXT") && fabsf(d - g_font_density) > 0.01f) fonts_rebuild(d);
    }
#endif
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    extern float g_stall_fade, g_fade_in;
    if (g_menu_loading) {  // (the window before the game's first frame: the loading mark in the middle of black)
        loading_mark(ImGui::GetBackgroundDrawList(), io.DisplaySize, scale, 1.0f, true);
    } else if (g_stall_fade > 0 || g_fade_in > 0) {
        // (the game busy loading, nvn/vulkan/device.c d3_stall_watch: its last picture as it is, and a small LOADING
        // with three gold diamonds lighting in turn in the bottom right corner; it fades in, and out when the game's
        // frames come back. The logo is only for the black loading screens - the tester 2026-11-15)
        float a = g_stall_fade > 0 ? g_stall_fade : g_fade_in;
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImFont *f = g_font_title ? g_font_title : g_font_big ? g_font_big : ImGui::GetFont();
        const char *txt = g_font_title ? "LOADING" : "Loading";
        float fs = 46 * scale, pad = 44 * scale;
        ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0, txt);
        float r = 7 * scale, gap = 22 * scale, dots_w = 2 * gap + 2 * r;
        float x = io.DisplaySize.x - pad - sz.x - 18 * scale - dots_w, y = io.DisplaySize.y - pad - sz.y;
        // (just the diamonds now - the word LOADING and its dark band are gone, the tester 2026-11-15 "remove the LOADING text
        // when starting a game"; they keep the word's place, the corner)
        double t = SDL_GetTicks() / 1000.0;
        for (int i = 0; i < 3; i++) {
            float ph = (float)fmod(t * 1.6 - i * 0.22, 1.0), lit = ph < 0.5f ? 1 - ph * 2 : 0;  // (each in turn)
            ImVec2 c(x + sz.x + 18 * scale + r + i * gap, y + sz.y * 0.52f);
            const ImVec2 p[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
            int g = (int)(130 + 125 * lit);
            dl->AddConvexPolyFilled(p, 4, IM_COL32(g, (int)(g * 0.75f), (int)(g * 0.42f), (int)(a * 255)));
            dl->AddPolyline(p, 4, IM_COL32(0, 0, 0, (int)(a * 160)), ImDrawFlags_Closed, 1.0f);
        }
    }
    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", g_ov_cache ? g_fps_shown : io.Framerate);
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
    if (flyin) pickup_flyin_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (trkd) tracker_dungeon_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open) {
        bool d3look = menu_d3_look();
        if (d3look) {
            // (Diablo III: as its own Options screen - the list straight on the game's picture, which darkens toward
            // the left, where the list is, and toward the bottom, where the button hints are)
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            ImVec2 D = io.DisplaySize;
            // (dark enough that the game's own menu text and hints behind - its main menu, when opened from there - don't
            // show through the list)
            bg->AddRectFilled(ImVec2(0, 0), D, IM_COL32(0, 0, 0, 90));
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(D.x * 0.44f, D.y), IM_COL32(0, 0, 0, 228));
            bg->AddRectFilledMultiColor(ImVec2(D.x * 0.44f, 0), ImVec2(D.x * 0.78f, D.y), IM_COL32(0, 0, 0, 228), IM_COL32(0, 0, 0, 0),
                                        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 228));
            bg->AddRectFilledMultiColor(ImVec2(0, D.y * 0.84f), ImVec2(D.x, D.y * 0.88f), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                        IM_COL32(0, 0, 0, 235), IM_COL32(0, 0, 0, 235));
            bg->AddRectFilled(ImVec2(0, D.y * 0.88f), D, IM_COL32(0, 0, 0, 235));
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(D.x, D.y - 96 * scale), ImGuiCond_Always);  // (the hints below it)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D.x * 0.06f, 50 * scale));
        } else {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        }
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         (d3look ? ImGuiWindowFlags_NoBackground : 0));
        if (d3look) ImGui::PopStyleVar();
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        if (d3look) {  // (Diablo III: the title in its face at the top left, a gold rule with a diamond under it)
            const char *title = "SETTINGS";
            ImVec2 tp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(tp.x + 2 * scale, tp.y + 2 * scale), IM_COL32(0, 0, 0, 200), title);
            ImGui::TextColored(ImVec4(0.92f, 0.78f, 0.50f, 1), "%s", title);
            ImVec2 ta = ImGui::GetItemRectMin(), tb = ImGui::GetItemRectMax();
            float cx = (ta.x + tb.x) * 0.5f, half = (tb.x - ta.x) * 0.75f;
            d3_rule(ImGui::GetWindowDrawList(), cx, tb.y + 6 * scale, half, fmaxf(1.0f, 1.5f * scale));
            d3_diamond(ImGui::GetWindowDrawList(), ImVec2(cx, tb.y + 6 * scale), 6 * scale, IM_COL32(226, 180, 100, 255),
                       IM_COL32(0, 0, 0, 200));
            ImGui::Dummy(ImVec2(0, 14 * scale));
        } else
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Settings");
        ImGui::PopFont();
        if (!d3look) {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        opt_theme(menu_d3_look(), g_menu_font_section, g_menu_font_desc);
#ifndef __APPLE__
        if (menu_d3_look()) {  // (the game's own highlight pieces, made from the player's game: src/analysis/d3_ui_art.py)
            struct Art { const char *name; uint64_t tex; int w, h; };
            static Art art[4] = {{"streak", 0, 0, 0}, {"smoke", 0, 0, 0}, {"flare", 0, 0, 0}, {"glow", 0, 0, 0}};
            static bool tried;
            static int gen;  // (runtime/hle/d3_art.c writes them the first time the game shows them: loaded again then)
            if (tried && gen != g_d3_ui_art_gen) tried = false;
            if (!tried) {
                tried = true;
                gen = g_d3_ui_art_gen;
                for (Art &a : art) {
                    if (a.tex) continue;
                    char path[96];
                    snprintf(path, sizeof path, "build/d3_ui/%s.rgba", a.name);
                    FILE *f = fopen(path, "rb");
                    uint32_t wh[2];
                    if (f && fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 4096 && wh[1] <= 8192) {
                        std::vector<uint8_t> px((size_t)wh[0] * wh[1] * 4);
                        if (fread(px.data(), 1, px.size(), f) == px.size()) {
                            a.tex = vk_imgui_texture_rgba(px.data(), (int)wh[0], (int)wh[1]);
                            a.w = (int)wh[0], a.h = (int)wh[1];
                        }
                    }
                    if (f) fclose(f);
                }
                LOG("[menu] the game's highlight pieces: %s\n", art[0].tex && art[1].tex && art[2].tex ? "loaded" : "not there (drawn instead)");
            }
            opt_highlight_art(art[0].tex, (float)art[0].w, (float)art[0].h, art[1].tex, (float)art[1].w, (float)art[1].h,
                              art[2].tex, (float)art[2].w, (float)art[2].h);
            opt_highlight_glow(art[3].tex, art[3].w ? art[3].h / (art[3].w / 4) : 0);  // (frames of 512 x 128)
        }
#endif
        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (back) {  // (the tester 2026-11-15: after changing the resolution Back sometimes needs two presses - which check held it)
            ImGuiContext &gc = *ImGui::GetCurrentContext();
            LOG("[menu] back: popup %d typing %d/%d took %d/%d focused %d (nav window %s, active %#x)\n", any_popup, typing,
                ui_text_busy(), controls_ui_took_back(), rando_tab_took_back(), focused,
                gc.NavWindow ? gc.NavWindow->Name : "none", gc.ActiveId);
        }
        bool quit_took = quit_tab_took_back();
        if (back && !any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && !rando_tab_took_back() &&
            !quit_took && focused)
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        if (d3look) d3_button_hints(scale);
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    // ImGui records its commands itself. Not into cmd, the window image's command buffer, while the recorder thread
    // (record.c) may still be recording it (one thread at a time on a command buffer and its pool: f054d8b, the device
    // loss entering Mabe Village), and without waiting for the recorder to catch up every frame either (a full drain:
    // 20-30 ms a frame on the Steam Deck): into a secondary command buffer of device.c's, which the recorder executes
    // in cmd in order - after the uploads queued above (overlay icons). Decided right here, where the draw data is
    // final, so an overlay turning on during this function can't miss it. (Recorder off: into cmd directly.)
    void *oc = vk_overlay_begin(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)oc);  // inside the window image's rendering
    vk_overlay_end(cmd, oc);
#endif
}
