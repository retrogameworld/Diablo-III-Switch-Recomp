// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. The game reads the answer as UTF-8.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";  // (UTF-16 -> UTF-8: a Russian / accented suggested name was all '?' - GitHub #5)
    size_t o = 0;
    for (uint32_t i = 0; wb && i < n && o + 4 < sizeof initial; i++) {
        uint32_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        if (ch >= 0xd800 && ch < 0xdc00 && i + 1 < n) {
            uint32_t lo = *(const uint16_t *)(uintptr_t)(wb + off + 2 * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00), i++;
        }
        if (ch < 0x80) initial[o++] = (char)ch;
        else if (ch < 0x800) initial[o++] = (char)(0xc0 | ch >> 6), initial[o++] = (char)(0x80 | (ch & 0x3f));
        else if (ch < 0x10000) initial[o++] = (char)(0xe0 | ch >> 12), initial[o++] = (char)(0x80 | ((ch >> 6) & 0x3f)), initial[o++] = (char)(0x80 | (ch & 0x3f));
        else initial[o++] = (char)(0xf0 | ch >> 18), initial[o++] = (char)(0x80 | ((ch >> 12) & 0x3f)), initial[o++] = (char)(0x80 | ((ch >> 6) & 0x3f)), initial[o++] = (char)(0x80 | (ch & 0x3f));
    }
    initial[o] = 0;
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    // (Diablo III: a cancel is reported as the SDK's own ShowKeyboard does - nn::swkbd's "cancelled", 0x29f (the
    // applet's answer code non-zero: sdk f_700438a390). Answered 0 with the suggested name, Cancel on the hero
    // name screen created the hero anyway; the tester 2026-11-16. Link's Awakening keeps the old answer.)
    extern bool game_is_d3(void);
    if (!ok && game_is_d3()) {
        c->x[0] = 0x29f;
        return;
    }
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        snprintf((char *)(uintptr_t)ptr, size, "%s", text);
    }
    c->x[0] = 0;
}
