// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

extern "C" {
#include "../cosmetic_colour.h"
}
#include "option_rows.h"
#include "../ui_sounds.h"
#include "osk.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)
// (Diablo III's look: the description pane is under the rows, headed by the row's own label)
static std::string g_title_hover, g_title_focus, g_title_shown, g_title_first;
static const float kD3DescH = 190;  // (its height at 1080p)

static ImU32 kGold = IM_COL32(236, 194, 84, 255);
static bool g_d3;  // (opt_theme) Diablo III's look
static ImFont *g_section_font, *g_desc_font;
static ImTextureID g_art_streak, g_art_smoke, g_art_flare, g_art_glow;
static int g_art_glow_frames;
void opt_highlight_glow(unsigned long long tex, int frames) { g_art_glow = (ImTextureID)tex, g_art_glow_frames = frames; }
static float g_art_fw = 1, g_art_fh = 1;
void opt_highlight_art(unsigned long long streak, float sw, float sh, unsigned long long smoke, float kw, float kh,
                       unsigned long long flare, float fw, float fh) {
    (void)sw, (void)sh, (void)kw, (void)kh;
    g_art_streak = (ImTextureID)streak, g_art_smoke = (ImTextureID)smoke, g_art_flare = (ImTextureID)flare;
    g_art_fw = fw > 0 ? fw : 1, g_art_fh = fh > 0 ? fh : 1;
}
void opt_theme(bool d3, ImFont *section, ImFont *desc) {
    g_d3 = d3, g_section_font = section, g_desc_font = desc;
    kGold = d3 ? IM_COL32(226, 178, 96, 255) : IM_COL32(236, 194, 84, 255);
}
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static bool g_next_mark, g_next_warn;
void opt_mark_next(void) { g_next_mark = true; }
void opt_warn_next(void) { g_next_warn = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
// (the page itself asks: its first row takes the cursor next frame - the Quit page swapping its rows for "Yes / Cancel"
// lost the cursor, two presses to get onto them; the tester 2026-11-15)
static bool g_first_next;
void opt_focus_first_next(void) { g_first_next = true; }
static int g_focus_row = -1;  // (opt_focus_row: this page's row with that index takes the cursor)
void opt_focus_row(int i) { g_focus_row = i; }
bool opt_focus_first_left(void) {
    bool left = g_focus_req || g_take_first;
    g_focus_req = g_take_first = false;
    g_focus_row = -1;
    return left;
}
// this frame's rows in order (focus checks): labels, the first enabled one, the focused one
static std::vector<std::string> g_rows;
static std::vector<int> g_row_pane;  // (each row's pane: 0 the left one, 1 opt_split's right one)
static int g_row_first = -1, g_row_focus = -1, g_pane;
int opt_row_count(void) { return (int)g_rows.size(); }
const char *opt_row_label(int i) { return i >= 0 && i < (int)g_rows.size() ? g_rows[i].c_str() : ""; }
int opt_first_row(void) { return g_row_first; }
int opt_focused_row(void) { return g_row_focus; }
// (a tab switch plays its own sound; the cursor then landing on the new page's first row must not add a second one -
// the tester 2026-11-15: "a double scrolling sound when moving between tabs")
static int g_move_quiet;
void opt_quiet_move_sound(void) { g_move_quiet = 4; }
void opt_frame_reset(void) {
    // (the menu's move sound: the selected row changed since the frame before - ui_sounds.h)
    // (also from the tab bar down into the list and back up: the cursor left the rows - the tester 2026-11-15, the first Down
    // from a tab made no sound)
    static std::string last;
    static bool on_tabs;
    std::string cur = g_row_focus >= 0 && g_row_focus < (int)g_rows.size() ? g_rows[g_row_focus] : std::string();
    static double last_snd;
    auto move_sound = [&]() {  // (one click a scroll: the cursor can settle in two steps a frame apart)
        double now = ImGui::GetTime();
        if (now - last_snd > 0.07 && !g_move_quiet) ui_sound(UI_SND_MOVE);
        last_snd = now;
    };
    if (!cur.empty()) {
        if ((!last.empty() || on_tabs) && cur != last) move_sound();
        last = cur, on_tabs = false;
    } else if (!last.empty() && !g_rows.empty()) {  // (the rows are there but none has the cursor: it went up to the tabs)
        move_sound();
        last.clear(), on_tabs = true;
    } else on_tabs = true;
    g_rows.clear(), g_row_pane.clear(), g_row_first = g_row_focus = -1;
    if (g_move_quiet) g_move_quiet--;
}
// Up / Down between option rows go by the rows' order, not ImGui's spatial pick: that skipped indented sub-rows
// (Rumble -> Controller light past Intensity), and past the last row went up to the tab bar
bool opt_nav_step(int dir) {
    int from = g_row_focus, n = (int)g_row_pane.size();  // (the frame before's rows)
    if (from < 0 || from >= n) return false;  // (the cursor isn't on an option row)
    int to = from + dir;
    while (to >= 0 && to < n && g_row_pane[to] != g_row_pane[from]) to += dir;  // (the same pane)
    if (to < 0 || to >= n) {
        if (dir < 0) return false;  // Up from the top row: out of the page (ImGui: the tab bar / controls above)
        to = from;                  // Down from the last row: stays
    }
    g_focus_row = to;
    return true;
}
static std::unordered_map<ImGuiID, int> g_ids;  // this page's rows so far, by id (two rows with one label)
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req || g_first_next;
    g_focus_req = g_first_next = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    if (g_d3) g_title_hover.clear(), g_title_focus.clear(), g_title_first.clear();
    ImGui::BeginChild("##rows", g_d3 ? ImVec2(avail.x * 0.40f, avail.y - kD3DescH * scale - ImGui::GetStyle().ItemSpacing.y)
                                     : ImVec2(avail.x * 0.48f, avail.y),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * scale));  // (rows close)
    g_desc_first.clear();
    g_ids.clear();
    g_pane = 0;
}

void opt_section(const char *title) {
    std::string t = title;
    for (char &c : t) c = (char)toupper((unsigned char)c);
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 10 * g_scale));
    if (g_d3) {  // (Diablo III: the title in its face, gold, with a rule fading out to the right)
        if (g_section_font) ImGui::PushFont(g_section_font);
        ImGui::TextColored(ImVec4(0.80f, 0.63f, 0.36f, 1), "%s", t.c_str());
        if (g_section_font) ImGui::PopFont();
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float x1 = a.x + ImGui::GetContentRegionAvail().x, th = 1.2f * g_scale > 1 ? 1.2f * g_scale : 1;
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(a.x, b.y + 1), ImVec2(x1, b.y + 1 + th),
            IM_COL32(190, 146, 76, 200), IM_COL32(190, 146, 76, 0), IM_COL32(190, 146, 76, 0), IM_COL32(190, 146, 76, 200));
        ImGui::Dummy(ImVec2(0, 4 * g_scale));
        return;
    }
    ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.52f, 1), "%s", t.c_str());
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// chevron: a ">" on the right (opt_link)
// Diablo III's menus write each word of a name with a capital ("Display Player Health Bars"); descriptions stay as
// sentences (the tester 2026-11-15). Small joining words stay small unless first; words with a capital or a digit already
// ("FPS", "4K", "720p") are left as they are.
static std::string d3_title(const std::string &s) {
    static const char *const small_words[] = {"a", "an", "and", "as", "at", "by", "for", "in", "of", "on", "or", "the", "to", "with"};
    std::string o = s;
    size_t i = 0;
    bool first = true;
    while (i < o.size()) {
        while (i < o.size() && !isalpha((unsigned char)o[i]) && !isdigit((unsigned char)o[i])) i++;
        size_t j = i;
        while (j < o.size() && (isalnum((unsigned char)o[j]) || o[j] == '\'' || o[j] == '-')) j++;
        if (j > i) {
            std::string w = o.substr(i, j - i);
            bool keep = false, small = false;
            for (char c : w) if (isupper((unsigned char)c) || isdigit((unsigned char)c)) keep = true;
            for (const char *sw : small_words) if (w == sw) small = true;
            if (!keep && !(small && !first)) o[i] = (char)toupper((unsigned char)o[i]);
            first = false;
        }
        i = j;
    }
    return o;
}

static int row(const char *label, const std::string &value_in, bool changed, const char *desc, bool upper = true,
               bool arrows = false, bool chevron = false) {
    const std::string value = g_d3 ? d3_title(value_in) : value_in;
    // a label a row above already has (Save Editor: Bombs, Magic Powder under Equipment and Ammo) gets an id of its
    // own: two rows with one id share the controller's cursor - ImGui kept the later one's place, so Up / Down from
    // the first went on from the second (a row far below, or the edge of the view)
    int dup = g_ids[ImGui::GetID(label)]++;
    ImGui::PushID(label);
    if (dup) ImGui::PushID(dup);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    if (g_take_first && !off) {  // (opt_focus_first: the first row that isn't greyed out)
        g_take_first = false;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        // the page from its top when the row is in its first screen (its section title shows), else the row at the top
        if (ImGui::GetCursorPosY() < ImGui::GetWindowHeight()) ImGui::SetScrollY(0);
        else ImGui::SetScrollHereY(0);
    }
    if (g_focus_row >= 0 && g_focus_row == (int)g_rows.size()) {  // (opt_focus_row / opt_nav_step)
        g_focus_row = -1;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    }
    if (!off && g_row_first < 0) g_row_first = (int)g_rows.size();
    if (ImGui::IsItemFocused()) g_row_focus = (int)g_rows.size();
    g_rows.push_back(dup ? std::string(label) + " #" + std::to_string(dup + 1) : std::string(label));
    g_row_pane.push_back(g_pane);
    bool mark = g_next_mark, warn = g_next_warn;
    g_next_disabled = g_next_mark = g_next_warn = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (foc) g_desc_focus = desc ? desc : "", g_title_focus = label;
    if (hov) g_desc_hover = desc ? desc : "", g_title_hover = label;
    if (g_desc_first.empty() && desc) g_desc_first = desc, g_title_first = label;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 9 * g_scale, pad = 16 * g_scale, fs = ImGui::GetFontSize();
    if (g_d3) {  // (Diablo III's Options list: no boxes; the selected row has a soft cyan glow streak and a cyan flare)
        pad = (g_art_glow ? 84 : 40) * g_scale;  // (room for the flare: the list's own edge clips what is left of it)
        if ((hov || foc) && g_art_glow && g_art_glow_frames > 0 && g_art_flare) {
            // (as the game's main menu: the glow moving - its veins drift through the streak, one loop in 2.4 s - and
            // the big cross flare left of the label; the tester 2026-11-15 "isn't animated, isn't like the main menu")
            float cy = p.y + h * 0.5f, s = g_scale, lx = p.x + pad;
            ImU32 tint = off ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 255);
            // (not cut off by the list's left edge: the glow fades out by itself - a hard edge there looked like a box)
            dl->PushClipRect(ImVec2(lx - 130 * s, cy - 60 * s), ImVec2(p.x + w, cy + 60 * s), false);
            // (a 4 s loop; the two frames around now blended by how far between them it is: smooth at any frame rate)
            // (the tester's video of the main menu, 120 fps: the glow runs from just left of the cross to about the end of the
            // word - ~260 px at 1080p - brightest at the cross, its strands moving quickly: a 1.6 s loop)
            // (as long as the label: its bright part - the art's first ~400 of 512 px - ends at the last letter; the tester
            // 2026-11-15 "a bit wider so that it ends at the last letter")
            float gw = fmaxf(310 * s, (ImGui::CalcTextSize(label).x + 70 * s) * 1.28f);
            double pos = ImGui::GetTime() / 1.6 * g_art_glow_frames;
            int fr = (int)pos % g_art_glow_frames, fn = (fr + 1) % g_art_glow_frames;
            float f = (float)(pos - floor(pos));
            for (int pass = 0; pass < 2; pass++) {  // (the two frames around now, crossfaded; the art is light itself now:
                                                    // drawn once, not doubled - the tester 2026-11-15 "just beautiful")
                int frm = pass == 0 ? fr : fn;
                float wgt = pass == 0 ? 1 - f : f;
                // (dimmer than full: the tester 2026-11-15 "make the highlight less bright")
                ImU32 tw = IM_COL32(255, 255, 255, (int)((off ? 80 : 175) * fminf(1.0f, wgt * 2.0f)));
                float v0 = (float)frm / g_art_glow_frames, v1 = (float)(frm + 1) / g_art_glow_frames;
                for (int k = 0; k < 1; k++)
                    dl->AddImage(g_art_glow, ImVec2(lx - 70 * s, cy - 48 * s), ImVec2(lx - 70 * s + gw, cy + 48 * s),
                                 ImVec2(0, v0), ImVec2(1, v1), tw);
            }
            float fh = 50 * s, fw = fh * 1.45f, fx = lx - 44 * s;
            for (int i = 0; i < 4; i++)  // (the soft round glow behind the cross)
                dl->AddCircleFilled(ImVec2(fx, cy), (34 - i * 7) * s, IM_COL32(40, 200, 220, off ? 4 : 8 + i * 4));
            for (int k = 0; k < 1; k++)
                dl->AddImage(g_art_flare, ImVec2(fx - fw * 0.5f, cy - fh * 0.5f), ImVec2(fx + fw * 0.5f, cy + fh * 0.5f),
                             ImVec2(0, 0), ImVec2(1, 1), tint);
            dl->PopClipRect();
        } else if ((hov || foc) && g_art_streak && g_art_smoke && g_art_flare) {
            // (the game's own pieces, as its Options list draws them: soft smoke, the streak over it from the flare
            // out past the label, and the cross flare at the left - the tester 2026-11-15, screenshot of the real menu)
            float cy = p.y + h * 0.5f;
            ImU32 tint = off ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 255);
            dl->AddImage(g_art_smoke, ImVec2(p.x - 30 * g_scale, cy - h * 0.62f), ImVec2(p.x + w * 0.92f, cy + h * 0.62f),
                         ImVec2(0, 0), ImVec2(1, 1), tint);
            dl->AddImage(g_art_streak, ImVec2(p.x - 14 * g_scale, cy - h * 0.50f), ImVec2(p.x + w * 0.80f, cy + h * 0.50f),
                         ImVec2(0, 0), ImVec2(1, 1), tint);
            float fh = 26 * g_scale, fw = fh * g_art_fw / g_art_fh, fx = p.x + 20 * g_scale;
            dl->AddImage(g_art_flare, ImVec2(fx - fw * 0.42f, cy - fh * 0.5f), ImVec2(fx + fw * 0.58f, cy + fh * 0.5f),
                         ImVec2(0, 0), ImVec2(1, 1), tint);
        } else if (hov || foc) {
            float cy = p.y + h * 0.5f, x0 = p.x, x1 = p.x + w * 0.70f;
            float k0 = off ? 0.4f : 1.0f;
            // the streak: thin stacked bands (soft top and bottom), fading out to the right
            for (int i = 0; i < 10; i++) {
                float hh = h * 0.31f * (1.0f - i * 0.085f);
                ImU32 c0 = IM_COL32(28, 150, 165, (int)(16 * k0)), z = IM_COL32(28, 150, 165, 0);
                dl->AddRectFilledMultiColor(ImVec2(x0, cy - hh), ImVec2(x1, cy + hh), c0, z, z, c0);
            }
            // brighter where the flare is, gone by a third of the row
            for (int i = 0; i < 6; i++) {
                float hh = h * 0.26f * (1.0f - i * 0.14f);
                ImU32 c0 = IM_COL32(60, 200, 215, (int)(20 * k0)), z = IM_COL32(60, 200, 215, 0);
                dl->AddRectFilledMultiColor(ImVec2(x0, cy - hh), ImVec2(p.x + w * 0.33f, cy + hh), c0, z, z, c0);
            }
            // the flare: a bright horizontal line through a small diamond, with a soft round glow
            ImVec2 c(p.x + 18 * g_scale, cy);
            float rr = 6.5f * g_scale;
            ImU32 line = IM_COL32(170, 245, 250, (int)(230 * k0)), lz = IM_COL32(170, 245, 250, 0);
            float th = fmaxf(1.0f, 1.3f * g_scale);
            dl->AddRectFilledMultiColor(ImVec2(c.x - 18 * g_scale, cy - th), ImVec2(c.x, cy + th), lz, line, line, lz);
            dl->AddRectFilledMultiColor(ImVec2(c.x, cy - th), ImVec2(c.x + 70 * g_scale, cy + th), line, lz, lz, line);
            for (int i = 0; i < 4; i++)
                dl->AddCircleFilled(c, rr * (3.2f - i * 0.6f), IM_COL32(60, 210, 225, (int)((10 + i * 8) * k0)));
            const ImVec2 dp[4] = {ImVec2(c.x, c.y - rr), ImVec2(c.x + rr, c.y), ImVec2(c.x, c.y + rr), ImVec2(c.x - rr, c.y)};
            dl->AddConvexPolyFilled(dp, 4, IM_COL32(20, 90, 100, (int)(235 * k0)));
            dl->AddPolyline(dp, 4, IM_COL32(180, 248, 252, (int)(255 * k0)), ImDrawFlags_Closed, 1.4f * g_scale);
            dl->AddCircleFilled(c, 1.8f * g_scale, IM_COL32(235, 255, 255, (int)(255 * k0)));
        } else if (mark)
            dl->AddRectFilled(ImVec2(p.x + 14 * g_scale, p.y + h * 0.3f), ImVec2(p.x + 17 * g_scale, p.y + h * 0.7f), kGold);
    } else {
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), hov || foc ? (off ? IM_COL32(236, 194, 84, 120) : kGold)
                                             : mark ? IM_COL32(236, 194, 84, 130) : IM_COL32(255, 255, 255, off ? 18 : 38),
                r, 0, hov || foc || mark ? 2.0f : 1.0f);
    }
    std::string t = g_d3 ? d3_title(label) : std::string(label);
    if (upper && !g_d3)  // (Diablo III: its menus use mixed case)
        for (char &c : t) c = (char)toupper((unsigned char)c);
    float ty = p.y + (h - fs) * 0.5f;
    ImU32 label_col = off ? IM_COL32(110, 110, 102, 255) : IM_COL32(240, 236, 224, 255);  // (greyed out: clearly dim)
    if (g_d3) label_col = off ? IM_COL32(118, 106, 86, 255) : hov || foc ? IM_COL32(252, 250, 244, 255) : IM_COL32(232, 200, 128, 255);
    if (g_d3) {  // (a soft shadow: the list sits on the game's own picture)
        ImU32 sh = IM_COL32(0, 0, 0, 200);
        if (hov || foc)  // (on the glow: a thin dark outline too, as the game's selected entry - it stays readable)
            for (int i = 0; i < 4; i++)
                dl->AddText(ImVec2(p.x + pad + (i & 1 ? 1.0f : -1.0f) * g_scale, ty + (i & 2 ? 1.0f : -1.0f) * g_scale),
                            IM_COL32(0, 20, 26, 150), t.c_str());
        dl->AddText(ImVec2(p.x + pad + 1.5f * g_scale, ty + 1.5f * g_scale), sh, t.c_str());
    }
    dl->AddText(ImVec2(p.x + pad, ty), label_col, t.c_str());
    float right = p.x + w - pad;  // (the value's right edge)
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        ImU32 ac = off ? IM_COL32(110, 110, 100, 255) : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        if (g_d3) {  // (the game's fat orange arrows: a head and a short tail)
            ac = off ? IM_COL32(120, 90, 50, 255) : IM_COL32(246, 158, 38, 255);
            float hw = aw * 0.95f, tl = aw * 0.55f, th = aw * 0.38f;
            dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + hw, cy - hw), ImVec2(lx + hw, cy + hw), ac);
            dl->AddRectFilled(ImVec2(lx + hw - 0.5f, cy - th), ImVec2(lx + hw + tl, cy + th), ac);
            dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx + aw - hw, cy + hw), ImVec2(rx + aw - hw, cy - hw), ac);
            dl->AddRectFilled(ImVec2(rx + aw - hw - tl, cy - th), ImVec2(rx + aw - hw + 0.5f, cy + th), ac);
        } else {
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        }
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x, hit = gap;
            if (mx >= lx - hit && mx <= lx + aw + hit) step = -1;
            else if (mx >= rx - hit && mx <= rx + aw + hit) step = 1;
        }
        right = rx - gap;
    }
    if (chevron) {  // ">", drawn (as the arrows above)
        float cw = fs * 0.2f, cy = ty + fs * 0.52f;
        ImU32 cc = off ? IM_COL32(110, 110, 100, 255) : hov || foc ? kGold : IM_COL32(205, 205, 195, 255);
        const ImVec2 pts[] = {ImVec2(right - cw, cy - cw * 1.1f), ImVec2(right, cy), ImVec2(right - cw, cy + cw * 1.1f)};
        dl->AddPolyline(pts, 3, cc, 0, 2.6f * g_scale);
        right -= cw + fs * 0.55f;
        if (step == 1) step = 2;  // (Right opens it too)
    }
    if (!value.empty()) {
        float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;
        ImU32 vc = off ? IM_COL32(110, 110, 102, 255) : warn ? IM_COL32(240, 110, 90, 255)
                 : changed ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 205, 195, 255);
        if (g_d3 && !off && !warn) vc = hov || foc ? IM_COL32(252, 250, 244, 255) : IM_COL32(232, 200, 128, 255);
        if (g_d3) dl->AddText(ImVec2(vx + 1.5f * g_scale, ty + 1.5f * g_scale), IM_COL32(0, 0, 0, 200), value.c_str());
        dl->AddText(ImVec2(vx, ty), vc, value.c_str());
    }
    if (dup) ImGui::PopID();
    ImGui::PopID();
    if (step && !off) ui_sound(step == 2 ? UI_SND_SELECT : UI_SND_CHANGE);  // (A / click, or left / right)
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (g_d3) {  // (Diablo III: a dark square box with a bronze edge, a gold tick when on)
        bool off = g_next_disabled;
        int step = row(label, "", *v != def, desc);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float fs = ImGui::GetFontSize(), s = fs * 1.05f, x1 = b.x - 16 * g_scale - fs * 0.42f * 2.2f, y0 = (a.y + b.y - s) * 0.5f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 q0(x1 - s, y0), q1(x1, y0 + s);
        dl->AddRectFilled(q0, q1, IM_COL32(8, 7, 6, 235), 2 * g_scale);
        dl->AddRect(q0, q1, off ? IM_COL32(80, 66, 46, 255) : IM_COL32(128, 100, 62, 255), 2 * g_scale, 0, 2.2f * g_scale);
        dl->AddRect(ImVec2(q0.x + 3 * g_scale, q0.y + 3 * g_scale), ImVec2(q1.x - 3 * g_scale, q1.y - 3 * g_scale),
                    IM_COL32(0, 0, 0, 200), 1 * g_scale, 0, 1.2f * g_scale);
        if (*v) {
            ImU32 tc = off ? IM_COL32(150, 120, 70, 255) : IM_COL32(246, 196, 92, 255);
            const ImVec2 tick[3] = {ImVec2(q0.x + s * 0.22f, q0.y + s * 0.52f), ImVec2(q0.x + s * 0.42f, q0.y + s * 0.72f),
                                    ImVec2(q0.x + s * 0.80f, q0.y + s * 0.28f)};
            dl->AddPolyline(tick, 3, tc, 0, 3.2f * g_scale);
        }
        if (!step) return false;
        *v = !*v;
        return true;
    }
    if (!row(label, *v ? "On" : "Off", *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = off ? IM_COL32(150, 150, 140, 255) : selected ? kGold : IM_COL32(205, 205, 195, 255);
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.0f);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, col);
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper, bool changed) {
    return row(label, value ? value : "", changed, desc, upper) == 2;
}

bool opt_colour(const char *label, CosColour *c, int slot, const char *desc) {
    int prev = c->pick;
    const char *names[N_COL];
    for (int i = 0; i < N_COL; i++) names[i] = g_col_names[i];
    char seed[48];
    int sp = col_pick(c, slot);  // (Default: the seed's own colour, shown as such)
    if (c->pick == COL_DEFAULT && sp != COL_DEFAULT) snprintf(seed, sizeof seed, "Seed: %s", g_col_names[sp]), names[0] = seed;
    bool changed = opt_choice(label, &c->pick, names, N_COL, COL_DEFAULT, desc);
    if (changed && c->pick == COL_CUSTOM && prev != COL_CUSTOM)  // (Custom starts from the colour it was on)
        c->rgb = prev > COL_DEFAULT && prev < COL_RANDOM ? g_col_rgb[prev] : (c->rgb ? c->rgb : 0xF2F0EA);
    if (c->pick != COL_CUSTOM) return changed;
    ImGui::PushID(label);
    char hex[16];
    snprintf(hex, sizeof hex, "#%06X", (unsigned)(c->rgb & 0xFFFFFF));
    opt_indent(1);
    bool open = opt_action("Custom color", hex, "Any color: pick its hue, saturation and brightness.", true, true);
    opt_indent(-1);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    {   // a swatch of it, left of the value
        float fs = ImGui::GetFontSize(), sw = fs * 1.1f, vw = ImGui::CalcTextSize(hex).x;
        float x1 = mx.x - 16 * g_scale - vw - fs * 0.5f, y0 = (mn.y + mx.y - sw) * 0.5f;
        ImU32 col = IM_COL32(c->rgb >> 16 & 255, c->rgb >> 8 & 255, c->rgb & 255, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x1 - sw, y0), ImVec2(x1, y0 + sw), col, 4 * g_scale);
        ImGui::GetWindowDrawList()->AddRect(ImVec2(x1 - sw, y0), ImVec2(x1, y0 + sw), IM_COL32(255, 255, 255, 90),
                                            4 * g_scale);
    }
    static int test_open = -1;  // LARECOMP_OPEN_PICKER=1 (test): the first Custom row's picker opens by itself
    if (test_open < 0) test_open = getenv("LARECOMP_OPEN_PICKER") != NULL;
    if (test_open) test_open = 0, open = true;
    if (open) ImGui::OpenPopup("##picker");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 14 * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 9 * g_scale);
    ImGui::PushStyleColor(ImGuiCol_Border, kGold);
    ImGui::SetNextWindowPos(ImVec2(mx.x + 12 * g_scale, mn.y), ImGuiCond_Appearing);  // (beside the row, over the description)
    if (ImGui::BeginPopup("##picker")) {
        float f[3] = {(c->rgb >> 16 & 255) / 255.0f, (c->rgb >> 8 & 255) / 255.0f, (c->rgb & 255) / 255.0f};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        if (ImGui::ColorPicker3("##colour", f, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                                   ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoAlpha)) {
            c->rgb = (uint32_t)lroundf(f[0] * 255) << 16 | (uint32_t)lroundf(f[1] * 255) << 8 | (uint32_t)lroundf(f[2] * 255);
            changed = true;
        }
        if (ImGui::Button("Done") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

bool opt_link(const char *label, const char *desc) { return row(label, "", false, desc, true, false, true) == 2; }

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id) {
        if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? desc : "";
    if (done || ui_input_text_cancelled() || (focus == 0 && !ImGui::IsItemActive() && !osk_is_open())) editing = 0;
    ImGui::PopID();
    return done;
}

// the left pane's end: its child window, and the description the right pane shows
static void left_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("LARECOMP_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (!g_desc_hover.empty()) g_desc_shown = g_desc_hover, g_title_shown = g_title_hover;
    else if (!g_desc_focus.empty()) g_desc_shown = g_desc_focus, g_title_shown = g_title_focus;
    else if (g_desc_shown.empty()) g_desc_shown = g_desc_first, g_title_shown = g_title_first;
}

// the right pane's child window, after a gold line between the two
static void right_begin(const char *id, ImGuiWindowFlags flags) {
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 24 * g_scale;
    if (g_d3) {  // (Diablo III: a gold line fading out at both ends)
        float x = p.x + gap * 0.5f, m = p.y + avail.y * 0.5f, th = g_scale > 1 ? g_scale : 1;
        ImU32 gc = IM_COL32(200, 156, 84, 170), z = IM_COL32(200, 156, 84, 0);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(x, p.y), ImVec2(x + th, m), z, z, gc, gc);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(x, m), ImVec2(x + th, p.y + avail.y), gc, gc, z, z);
    } else
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        IM_COL32(236, 194, 84, 110), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild(id, ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, flags);
    ImGui::PopStyleVar();
}

void opt_desc_text(void) {
    if (g_desc_font) {  // (a font made at that size: crisp, not a scaled one)
        ImGui::PushFont(g_desc_font);
        ImGui::PushTextWrapPos(0);
        if (g_d3) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.74f, 0.62f, 1));
        ImGui::TextUnformatted(g_desc_shown.c_str());
        if (g_d3) ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        return;
    }
    ImGui::SetWindowFontScale(1.3f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
}

void opt_end(void) {
    left_end();
    if (g_d3) {  // (Diablo III: the description under the list - the row's name, then what it does)
        ImGui::BeginChild("##desc", ImVec2(ImGui::GetContentRegionAvail().x * 0.46f, kD3DescH * g_scale), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
        ImGui::Dummy(ImVec2(0, 18 * g_scale));
        if (g_desc_font) ImGui::PushFont(g_desc_font);
        ImGui::Indent(44 * g_scale);
        ImGui::TextColored(ImVec4(0.98f, 0.97f, 0.94f, 1), "%s", d3_title(g_title_shown).c_str());
        // (always all of it: a long one is drawn smaller until it fits the box - the tester 2026-11-15, its last line was cut)
        float room = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetCursorScreenPos().y - 4 * g_scale;
        float wrap = ImGui::GetContentRegionAvail().x, fit = 1.0f;
        while (fit > 0.6f && ImGui::CalcTextSize(g_desc_shown.c_str(), NULL, false, wrap / fit).y * fit > room) fit -= 0.04f;
        ImGui::SetWindowFontScale(fit);
        ImGui::PushTextWrapPos(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.76f, 0.72f, 1));
        ImGui::TextUnformatted(g_desc_shown.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Unindent(44 * g_scale);
        if (g_desc_font) ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }
    right_begin("##desc", ImGuiWindowFlags_NoNav);
    opt_desc_text();
    ImGui::EndChild();
    ImGui::PopID();
}

void opt_split(void) {
    left_end();
    right_begin("##right", ImGuiWindowFlags_None);
    g_pane = 1;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * g_scale));  // (as the left)
    g_indent = 0;
}

void opt_split_end(void) {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopID();
}
