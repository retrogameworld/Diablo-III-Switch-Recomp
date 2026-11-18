// Our menu's sounds (runtime/ui_sounds.c): Diablo III's own UI clicks, as WAV files the player made from their own PC
// copy (src/analysis/d3_ui_sounds.py -> build/d3_ui_sounds/; never shipped). Missing files: silent.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

enum { UI_SND_MOVE, UI_SND_SELECT, UI_SND_TAB, UI_SND_OPEN, UI_SND_CLOSE, UI_SND_CHANGE, UI_SND_N };
void ui_sound(int which);  // any thread; at the Audio volume; nothing in test runs (LARECOMP_NO_AUDIO)

#ifdef __cplusplus
}
#endif
