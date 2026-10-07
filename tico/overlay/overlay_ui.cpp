// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/overlay_ui.h"
#include "UsbStorage.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <switch.h>

#include <imgui.h>

namespace SwitchFrontend::OverlayUI {

namespace {
constexpr int kOverlaySlotCount = 6;
// Slots 1-5 are the player's; the sixth holds the state saved automatically
// when the game closes, listed first (as "Auto") in Load State only.
constexpr int kUserSlotCount = 5;
constexpr int kAutoSlotIndex = 5;
constexpr int kToastSlotCount = 4;
// rows shown at once before a list starts scrolling
constexpr int kMaxVisibleRows = 8;
constexpr float kMenuWidth = 480.0f;
// the settings panel: a category sidebar beside the options of the focused one
constexpr float kSettingsWidth = 1040.0f;
constexpr float kSettingsHeight = 500.0f;
constexpr float kSidebarWidth = 300.0f;
constexpr float kSettingsRowHeight = 56.0f;

constexpr float kAnimDuration = 0.4f;
constexpr float kToastDuration = 2.0f;
constexpr float kToastFadeDuration = 0.5f;

enum class MenuScreen {
    QuickMenu,
    SaveStates,
    LoadStates,
    Rewind,
    Discs,
    Cheats,
    Mods,
    SettingsCategories,
    SettingsOptions,
    ShaderBrowser,
    Library,
    FolderBrowser,
    FolderActions,
    FolderConfirm,
    // offered as the game starts when it has an auto save: Continue or Start Over
    Resume,
    // Delete/Cancel before a game's own settings are deleted
    GameSettingsConfirm,
    // ShowNotice's message and choices
    Notice,
    // Yes/Cancel before Restart or Exit Game
    GameConfirm,
};

enum class QuickItem {
    SaveState,
    LoadState,
    Rewind,
    ChangeDisc,
    Cheats,
    Mods,
    Settings,
    Reset,
    Restart,
    Exit,
};

// What GameConfirm asks about: QuickItem::Reset, Restart or Exit.
QuickItem s_confirm_item = QuickItem::Exit;

// The quick menu's sidebar, left of the list: Settings, Restart, Exit Game.
// Left moves focus onto it and right back to the list, as in tico's own menu.
constexpr QuickItem kSidebarItems[] = {QuickItem::Settings, QuickItem::Restart, QuickItem::Exit};
constexpr int kSidebarCount = 3;
bool s_side_focus = false;
int s_side_selected = 0;
// where GameConfirm returns to on Cancel
bool s_confirm_from_side = false;
std::array<unsigned long long, kSidebarCount> s_side_icons{};

// One line of whatever list is on screen. Every menu is drawn from these, so
// they all share scrolling, selection and layout.
struct MenuRow {
    std::string label;
    // shown right-aligned; empty for rows that only have a label
    std::string value;
    bool has_checkbox = false;
    bool checked = false;
    bool dimmed = false;
    // the value is information, not something left/right changes
    bool static_value = false;
};

std::string s_title;
bool s_hardcore = false;
std::string s_nickname;
NavInput s_nav{};
bool s_visible = false;
float s_anim_timer = 0.0f;
MenuScreen s_menu = MenuScreen::QuickMenu;
// the selected row on the current screen, and the ones to return to
int s_selected = 0;
int s_quick_selected = 0;
int s_category_selected = 0;
unsigned long long s_avatar_texture_id = 0;
unsigned long long s_border_texture_id = 0;
std::mutex s_toast_mutex;
std::array<std::string, kToastSlotCount> s_toast_messages{};
std::array<float, kToastSlotCount> s_toast_timers{};
SlotOccupiedFn s_slot_occupied_cb;
SlotPreviewFn s_slot_preview_cb;
CheatListFn s_cheat_list_cb;
CheatToggleFn s_cheat_toggle_cb;
RewindListFn s_rewind_list_cb;
DiscListFn s_disc_list_cb;
std::vector<CheatMenuEntry> s_cheat_entries;
ModListFn s_mod_list_cb;
ModStepFn s_mod_step_cb;
std::vector<ModMenuEntry> s_mod_entries;
std::vector<DiscMenuEntry> s_disc_entries;
std::array<bool, kOverlaySlotCount> s_slot_occupied{};
std::array<SlotPreview, kOverlaySlotCount> s_slot_preview{};
UndoStateFn s_undo_state_cb;
UndoStateInfo s_undo_state{};
std::vector<int> s_rewind_points;
ShaderCallbacks s_shader_cb;
LibraryCallbacks s_library_cb;
LibraryFolderCallbacks s_folder_cb;
bool s_library_mode = false;
std::vector<LibraryEntry> s_library_entries;
// the folder shown by the folder browser; empty while it lists the drives
std::string s_folder_dir;
std::vector<std::string> s_folder_subdirs;
// the drive list: each drive's name and root
std::vector<std::pair<std::string, std::string>> s_folder_drives;
// the folder being edited: its group, and its index (-1 while adding)
int s_folder_group = 0;
int s_folder_index = -1;
constexpr std::size_t kMaxLibraryFolders = 16;
std::string s_browse_dir;
std::vector<ShaderBrowseEntry> s_browse_entries;

// Results the emulation thread collects: the overlay renders on the
// presentation thread and never touches the emulator itself.
std::mutex s_pending_mutex;
bool s_settings_changed = false;
int s_rewind_index = -1;
int s_disc_index = -1;
const TicoConfig::OptionDef* s_text_edit_option = nullptr;

// HUD drawn over the game while the menu is closed
std::mutex s_hud_mutex;
HudStats s_hud_stats{};
int s_fps_position = 0;
int s_resolution_position = 0;

// Touch: the drawn rows are recorded as hit regions each frame, and the next
// frame's touch is tested against them. As in tico's own menus, a press
// selects a row, a tap on the row that was already selected activates it,
// dragging scrolls the list and a tap outside the menu goes back.
enum class HitKind {
    MenuRow,     // a row of the quick menu or one of its lists
    CategoryRow, // settings sidebar
    OptionRow,   // settings options pane
    StepLeft,    // the selected option's change arrows
    StepRight,
    Back,        // helpers bar
    Accept,
    Panel,       // the menu's background: taps there do nothing
    SideItem,    // the quick menu's sidebar icons
};
struct HitRegion {
    ImVec2 min;
    ImVec2 max;
    HitKind kind;
    int index;
};
std::vector<HitRegion> s_hits;
TouchInput s_touch{};
bool s_touch_was_down = false;
bool s_touch_ignore = false; // a finger already down when the menu opened
bool s_touch_tracking = false;
bool s_touch_moved = false;
bool s_touch_started_selected = false;
HitKind s_touch_kind = HitKind::Panel;
int s_touch_index = -1;
int s_touch_scrolled = 0; // rows scrolled by the current drag
float s_touch_row_height = 0.0f;
ImVec2 s_touch_start;
ImVec2 s_touch_last;

void AddHit(ImVec2 min, ImVec2 max, HitKind kind, int index = -1) {
    s_hits.push_back({min, max, kind, index});
}

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

std::string TrOr(const char* key, const char* fallback) {
    const std::string translated = OverlayTranslation::tr(key);
    return translated == key ? std::string(fallback) : translated;
}

// tico's light or dark theme, read each time the menu opens. The panels and
// what is drawn on them follow it; what sits on the dimmed game (title card,
// helpers bar, status bar, toasts) stays light on dark in both, as in tico.
bool s_dark = false;

ImU32 Themed(int dr, int dg, int db, int lr, int lg, int lb, int a) {
    return s_dark ? IM_COL32(dr, dg, db, a) : IM_COL32(lr, lg, lb, a);
}
ImU32 PanelColor(int a) { return Themed(45, 45, 45, 242, 245, 248, a); }
ImU32 PanelShadeColor(int a) { return Themed(34, 34, 34, 228, 232, 237, a); }
// a row's label and value: dimmed, normal or selected
ImU32 RowTextColor(bool dimmed, bool selected, int a) {
    if (dimmed) return Themed(150, 150, 150, 150, 150, 160, a);
    return selected ? Themed(255, 255, 255, 60, 60, 70, a) : Themed(200, 200, 200, 90, 90, 100, a);
}

void DrawTextWithShadow(ImDrawList* dl, ImFont* font, float font_size, ImVec2 pos, ImU32 color,
                        std::string_view text) {
    const ImU32 shadow_color = IM_COL32(0, 0, 0, 80);
    dl->AddText(font, font_size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), shadow_color, text.data(),
                text.data() + text.size());
    dl->AddText(font, font_size, pos, color, text.data(), text.data() + text.size());
}

void DrawSwitchButton(ImDrawList* dl, ImFont* font, float font_size, ImVec2 center, float size,
                      std::string_view symbol, float alpha) {
    const ImU32 fill_color = IM_COL32(220, 220, 220, static_cast<int>(255.0f * alpha));
    const ImU32 text_color = IM_COL32(40, 40, 40, static_cast<int>(255.0f * alpha));
    dl->AddCircleFilled(center, size * 0.5f, fill_color, 16);

    const float symbol_size = font_size * 0.75f;
    const ImVec2 text_size = font->CalcTextSizeA(symbol_size, FLT_MAX, 0.0f, symbol.data(),
                                                 symbol.data() + symbol.size());
    const ImVec2 text_pos(center.x - (text_size.x * 0.5f), center.y - (text_size.y * 0.5f));
    dl->AddText(font, symbol_size, text_pos, text_color, symbol.data(),
                symbol.data() + symbol.size());
}

std::string EllipsizeText(ImFont* font, float font_size, const std::string& text, float max_width) {
    if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x <= max_width) {
        return text;
    }

    constexpr std::string_view suffix = "...";
    std::string result = text;
    while (!result.empty()) {
        result.pop_back();
        const std::string candidate = result + "...";
        if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, candidate.c_str()).x <= max_width) {
            return candidate;
        }
    }
    return std::string(suffix);
}

void DrawCheckBox(ImDrawList* dl, ImVec2 center, float size, bool checked, float alpha) {
    const float half = size * 0.5f;
    const ImVec2 p0(center.x - half, center.y - half);
    const ImVec2 p1(center.x + half, center.y + half);
    const ImU32 border = Themed(220, 220, 220, 90, 90, 100, static_cast<int>(220.0f * alpha));
    const ImU32 fill = Themed(235, 235, 235, 60, 60, 70, static_cast<int>(checked ? 220.0f * alpha : 0.0f));
    dl->AddRect(p0, p1, border, 4.0f, 0, 2.0f);
    if (!checked) {
        return;
    }

    dl->AddRectFilled(ImVec2(p0.x + 3.0f, p0.y + 3.0f), ImVec2(p1.x - 3.0f, p1.y - 3.0f), fill,
                      3.0f);
    const ImU32 mark = PanelColor(static_cast<int>(255.0f * alpha));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.25f), center.y));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.44f), p0.y + (size * 0.68f)));
    dl->PathLineTo(ImVec2(p0.x + (size * 0.76f), p0.y + (size * 0.32f)));
    dl->PathStroke(mark, 0, 2.5f);
}

// Port of tico-nx's UIStyle::DrawAnimatedGradientBorder (textured path): the
// tint's gradient strip is wrapped around the rounded outline and scrolled
// along it over time.
void DrawAnimatedGradientBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float corner_radius,
                                float frame_width, float alpha, ImTextureID texture) {
    constexpr float kPi = 3.14159265f;
    constexpr int kCornerSegments = 12;
    const float phase = static_cast<float>(ImGui::GetTime()) * 0.5f;
    const float phase_mod = phase - std::floor(phase);
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    corner_radius = std::min(corner_radius, std::min(w, h) * 0.5f);
    const float perimeter =
        (2.0f * (w + h - (4.0f * corner_radius))) + (2.0f * kPi * corner_radius);

    struct BorderPoint {
        ImVec2 pos;
        ImVec2 normal;
        // distance along the outline, 0..1
        float dist;
    };
    std::array<BorderPoint, 4 * (kCornerSegments + 1)> points{};
    float travelled = 0.0f;
    const std::array<ImVec2, 4> centers = {{
        ImVec2(min.x + corner_radius, min.y + corner_radius),
        ImVec2(max.x - corner_radius, min.y + corner_radius),
        ImVec2(max.x - corner_radius, max.y - corner_radius),
        ImVec2(min.x + corner_radius, max.y - corner_radius),
    }};
    std::size_t count = 0;
    for (int c = 0; c < 4; ++c) {
        const float start = -kPi + (static_cast<float>(c) * kPi * 0.5f);
        for (int i = 0; i <= kCornerSegments; ++i) {
            const float angle = start + (kPi * 0.5f * static_cast<float>(i) / kCornerSegments);
            const ImVec2 normal(std::cos(angle), std::sin(angle));
            const ImVec2 pos(centers[c].x + (normal.x * corner_radius),
                             centers[c].y + (normal.y * corner_radius));
            if (count > 0) {
                const float dx = pos.x - points[count - 1].pos.x;
                const float dy = pos.y - points[count - 1].pos.y;
                travelled += std::sqrt((dx * dx) + (dy * dy));
            }
            points[count++] = {pos, normal, perimeter > 0.0f ? travelled / perimeter : 0.0f};
        }
    }

    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha));
    const float half = frame_width * 0.5f;
    const auto lerp = [](ImVec2 a, ImVec2 b, float t) {
        return ImVec2(a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t));
    };
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t next = (i + 1) % count;
        const BorderPoint& p1 = points[i];
        const BorderPoint& p2 = points[next];
        const ImVec2 v1o(p1.pos.x + (p1.normal.x * half), p1.pos.y + (p1.normal.y * half));
        const ImVec2 v1i(p1.pos.x - (p1.normal.x * half), p1.pos.y - (p1.normal.y * half));
        const ImVec2 v2o(p2.pos.x + (p2.normal.x * half), p2.pos.y + (p2.normal.y * half));
        const ImVec2 v2i(p2.pos.x - (p2.normal.x * half), p2.pos.y - (p2.normal.y * half));
        float u1 = p1.dist + phase_mod;
        float u2 = (next == 0 ? 1.0f : p2.dist) + phase_mod;
        const auto emit = [&](float t1, float t2, float u_start, float u_end) {
            dl->AddImageQuad(ImTextureRef(texture), lerp(v1o, v2o, t1), lerp(v1o, v2o, t2),
                             lerp(v1i, v2i, t2), lerp(v1i, v2i, t1), ImVec2(u_start, 0.0f),
                             ImVec2(u_end, 0.0f), ImVec2(u_end, 1.0f), ImVec2(u_start, 1.0f), tint);
        };
        // keep the UVs inside [0,1]: split a segment that crosses the wrap
        if (u1 < 1.0f && u2 > 1.0f) {
            const float split = (1.0f - u1) / (u2 - u1);
            emit(0.0f, split, u1, 1.0f);
            emit(split, 1.0f, 0.0f, u2 - 1.0f);
        } else {
            if (u1 >= 1.0f)
                u1 -= 1.0f;
            if (u2 >= 1.0f)
                u2 -= 1.0f;
            emit(0.0f, 1.0f, u1, u2);
        }
    }
}

// The focused row of any list: the animated tint border, or a plain
// highlight when the strip could not be loaded.
void DrawSelection(ImDrawList* dl, ImVec2 min, ImVec2 max, float corner_radius, float alpha) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    if (s_border_texture_id == 0) {
        dl->AddRectFilled(min, max, Themed(60, 60, 60, 220, 224, 230, static_cast<int>(255.0f * alpha)),
                          corner_radius);
        return;
    }
    DrawAnimatedGradientBorder(dl, min, max, corner_radius, 4.0f * scale, alpha,
                               static_cast<ImTextureID>(s_border_texture_id));
}

// A solid swatch of the tint (one column of the strip), for accents.
void DrawTintSwatch(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    const ImU32 color = IM_COL32(255, 255, 255, static_cast<int>(255.0f * alpha));
    if (s_border_texture_id == 0) {
        dl->AddRectFilled(min, max, IM_COL32(220, 220, 220, static_cast<int>(255.0f * alpha)),
                          rounding);
        return;
    }
    dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(s_border_texture_id)), min, max,
                        ImVec2(0.1f, 0.0f), ImVec2(0.1f, 1.0f), color, rounding);
}

std::string TrLabel(const char* key, const std::string& fallback) {
    if (!key) {
        return fallback;
    }
    const std::string translated = OverlayTranslation::tr(key);
    return translated == key ? fallback : translated;
}

std::vector<QuickItem> BuildQuickItems() {
    std::vector<QuickItem> items = {QuickItem::SaveState};
    if (!s_hardcore) {
        items.push_back(QuickItem::LoadState);
    }
    if (!s_hardcore && s_rewind_list_cb &&
        TicoConfig::GetConfigValue("enable_rewind", "false") == "true") {
        items.push_back(QuickItem::Rewind);
    }
    if (s_disc_entries.size() > 1) {
        items.push_back(QuickItem::ChangeDisc);
    }
    if (!s_hardcore && s_cheat_list_cb) {
        items.push_back(QuickItem::Cheats);
    }
    if (!s_hardcore && s_mod_list_cb) {
        items.push_back(QuickItem::Mods);
    }
    items.push_back(QuickItem::Reset);
    return items;
}

std::string QuickItemLabel(QuickItem item) {
    switch (item) {
    case QuickItem::SaveState:
        return TrOr("emulator_save_state", "Save State");
    case QuickItem::LoadState:
        return TrOr("emulator_load_state", "Load State");
    case QuickItem::Rewind:
        return TrOr("emulator_rewind", "Rewind");
    case QuickItem::ChangeDisc:
        return TrOr("emulator_select_disc", "Change Disc");
    case QuickItem::Cheats:
        return TrOr("emulator_cheats", "Cheats");
    case QuickItem::Mods:
        return TrOr("emulator_mods", "Mods");
    case QuickItem::Settings:
        return TrOr("emulator_settings", "Settings");
    case QuickItem::Reset:
        return TrOr("emulator_reset", "Reset");
    case QuickItem::Restart:
        return TrOr("emulator_restart", "Restart");
    case QuickItem::Exit:
    default:
        return TrOr("emulator_exit_game", "Exit Game");
    }
}

// Categories the frontend adds after the settings.json ones.
enum class ExtraCategory {
    None,
    Players,
    Game,
    Shaders,
    Library,
};

PlayerCallbacks s_player_cb;
std::string s_notice_message;
std::vector<std::string> s_notice_choices;
int s_notice_choice = -1;

std::vector<ExtraCategory> ExtraCategories() {
    std::vector<ExtraCategory> extra;
    if (s_player_cb.ports) {
        extra.push_back(ExtraCategory::Players);
    }
    if (TicoConfig::HasGame()) {
        extra.push_back(ExtraCategory::Game);
    }
    if (s_shader_cb.parameters) {
        extra.push_back(ExtraCategory::Shaders);
    }
    if (s_folder_cb.groups) {
        extra.push_back(ExtraCategory::Library);
    }
    return extra;
}

int CategoryCount() {
    return static_cast<int>(TicoConfig::GetCategories().size() + ExtraCategories().size());
}

ExtraCategory ExtraCategoryAt(int index) {
    const int extra_index = index - static_cast<int>(TicoConfig::GetCategories().size());
    const std::vector<ExtraCategory> extra = ExtraCategories();
    if (extra_index < 0 || extra_index >= static_cast<int>(extra.size())) {
        return ExtraCategory::None;
    }
    return extra[static_cast<std::size_t>(extra_index)];
}

bool ShaderCategoryActive() {
    return ExtraCategoryAt(s_category_selected) == ExtraCategory::Shaders;
}

bool LibraryCategoryActive() {
    return ExtraCategoryAt(s_category_selected) == ExtraCategory::Library;
}

bool GameCategoryActive() {
    return ExtraCategoryAt(s_category_selected) == ExtraCategory::Game;
}

bool PlayersCategoryActive() {
    return ExtraCategoryAt(s_category_selected) == ExtraCategory::Players;
}

// The Players category: each port and what it has, a note, then the row that
// opens the system's controller screen to change who is which player.
std::vector<MenuRow> BuildPlayerRows() {
    std::vector<MenuRow> rows;
    const std::vector<std::string> ports = s_player_cb.ports ? s_player_cb.ports() : std::vector<std::string>{};
    const std::string format = TrOr("emulator_player", "Player %d");
    for (std::size_t i = 0; i < ports.size(); ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), format.c_str(), static_cast<int>(i) + 1);
        MenuRow row{label};
        row.value = ports[i].empty() ? TrOr("emulator_not_connected", "Not connected") : ports[i];
        row.static_value = true;
        row.dimmed = ports[i].empty();
        rows.push_back(row);
    }
    const std::string note = s_player_cb.note ? s_player_cb.note() : std::string();
    if (!note.empty()) {
        MenuRow row{note};
        row.dimmed = true;
        rows.push_back(row);
    }
    rows.push_back({TrOr("emulator_change_controllers", "Change controller order")});
    return rows;
}

// The This Game category: save the current settings as the game's own, or,
// once it has them, delete them; then what that means.
std::vector<MenuRow> BuildGameRows() {
    const bool active = TicoConfig::GameSettingsActive();
    MenuRow action{active ? TrOr("emulator_game_settings_delete", "Delete this game's settings")
                          : TrOr("emulator_game_settings_save", "Save current settings for this game")};
    MenuRow hint{active ? TrOr("emulator_game_only_on", "Changes apply to this game only")
                        : TrOr("emulator_game_only_off", "Changes apply to every game")};
    hint.dimmed = true;
    return {action, hint};
}

std::string CategoryLabel(int index) {
    const auto& categories = TicoConfig::GetCategories();
    if (index >= 0 && index < static_cast<int>(categories.size())) {
        const auto& category = categories[static_cast<std::size_t>(index)];
        return TrLabel(category.label_key, category.fallback);
    }
    switch (ExtraCategoryAt(index)) {
    case ExtraCategory::Players:
        return TrOr("emulator_players", "Players");
    case ExtraCategory::Game:
        return TrOr("emulator_this_game", "This Game");
    case ExtraCategory::Library:
        return TrOr("emulator_library", "Library");
    default:
        return TrOr("emulator_shaders", "Shaders");
    }
}

MenuScreen RootScreen() {
    return s_library_mode ? MenuScreen::Library : MenuScreen::QuickMenu;
}

// One row of the Library category.
struct FolderEntry {
    enum Kind { Heading, Base, Folder, Add } kind;
    int group;
    int index;
};

std::vector<LibraryFolderGroup> FolderGroups() {
    return s_folder_cb.groups ? s_folder_cb.groups() : std::vector<LibraryFolderGroup>{};
}

std::vector<FolderEntry> FolderEntries(const std::vector<LibraryFolderGroup>& groups) {
    std::vector<FolderEntry> entries;
    for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
        entries.push_back({FolderEntry::Heading, g, -1});
        for (int i = 0; i < static_cast<int>(groups[g].bases.size()); ++i) {
            entries.push_back({FolderEntry::Base, g, i});
        }
        entries.push_back({FolderEntry::Add, g, -1});
        for (int i = 0; i < static_cast<int>(groups[g].folders.size()); ++i) {
            entries.push_back({FolderEntry::Folder, g, i});
        }
    }
    return entries;
}

// The Library category: per console, its heading, tico's bases, "Add folder"
// and its own folders.
std::vector<MenuRow> BuildFolderRows() {
    std::vector<MenuRow> rows;
    const std::vector<LibraryFolderGroup> groups = FolderGroups();
    for (const FolderEntry& entry : FolderEntries(groups)) {
        const LibraryFolderGroup& group = groups[static_cast<std::size_t>(entry.group)];
        switch (entry.kind) {
        case FolderEntry::Heading: {
            MenuRow row{group.label};
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        case FolderEntry::Base: {
            MenuRow row{UsbStorage::DisplayName(group.bases[static_cast<std::size_t>(entry.index)])};
            row.value = "tico";
            row.static_value = true;
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        case FolderEntry::Add: {
            MenuRow row{TrOr("emulator_add_folder", "Add folder")};
            if (group.folders.size() >= kMaxLibraryFolders) {
                row.value = "16/16";
                row.static_value = true;
                row.dimmed = true;
            }
            rows.push_back(row);
            break;
        }
        case FolderEntry::Folder: {
            MenuRow row{UsbStorage::DisplayName(group.folders[static_cast<std::size_t>(entry.index)])};
            rows.push_back(row);
            break;
        }
        }
    }
    return rows;
}

std::string FolderIdentity(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

// Stores `path` as the folder being added or changed. False for a duplicate.
bool StoreFolder(const std::string& picked) {
    // a folder on a USB drive is kept by the drive's id, not its umsN: mount
    const std::string path = UsbStorage::ToToken(picked);
    std::vector<LibraryFolderGroup> groups = FolderGroups();
    if (s_folder_group < 0 || s_folder_group >= static_cast<int>(groups.size()) || !s_folder_cb.set) {
        return false;
    }
    std::vector<std::string> folders = groups[static_cast<std::size_t>(s_folder_group)].folders;
    const std::string identity = FolderIdentity(path);
    for (int i = 0; i < static_cast<int>(folders.size()); ++i) {
        if (i != s_folder_index && FolderIdentity(folders[static_cast<std::size_t>(i)]) == identity) {
            return false;
        }
    }
    if (s_folder_index >= 0 && s_folder_index < static_cast<int>(folders.size())) {
        folders[static_cast<std::size_t>(s_folder_index)] = path;
    } else if (folders.size() < kMaxLibraryFolders) {
        folders.push_back(path);
    }
    s_folder_cb.set(s_folder_group, folders);
    return true;
}

// The row of the Library category showing the edited folder (or "Add folder").
int FolderRowFor(int group, int index) {
    const std::vector<FolderEntry> entries = FolderEntries(FolderGroups());
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const FolderEntry& entry = entries[static_cast<std::size_t>(i)];
        if (entry.group != group) {
            continue;
        }
        if ((index < 0 && entry.kind == FolderEntry::Add) ||
            (index >= 0 && entry.kind == FolderEntry::Folder && entry.index == index)) {
            return i;
        }
    }
    return 0;
}

std::string EditedFolder() {
    const std::vector<LibraryFolderGroup> groups = FolderGroups();
    if (s_folder_group < 0 || s_folder_group >= static_cast<int>(groups.size())) {
        return std::string();
    }
    const auto& folders = groups[static_cast<std::size_t>(s_folder_group)].folders;
    return s_folder_index >= 0 && s_folder_index < static_cast<int>(folders.size())
               ? folders[static_cast<std::size_t>(s_folder_index)]
               : std::string();
}

// The drives a folder can be on: the SD card and each USB drive.
void OpenDriveList() {
    s_folder_dir.clear();
    s_folder_subdirs.clear();
    s_folder_drives = {{TrOr("emulator_sd_card", "SD card"), "sdmc:/"}};
    for (const UsbStorage::Volume& volume : UsbStorage::Volumes()) {
        s_folder_drives.emplace_back(volume.label, volume.root);
    }
    s_menu = MenuScreen::FolderBrowser;
    s_selected = 0;
}

// sdmc:/, ums0:/ and the like
bool IsDriveRoot(const std::string& dir) {
    return dir.size() >= 2 && dir.compare(dir.size() - 2, 2, ":/") == 0;
}

void OpenFolderBrowser(std::string dir) {
    dir = UsbStorage::Resolve(dir);
    if (dir.empty()) {
        OpenDriveList(); // the folder's drive is not connected
        return;
    }
    if (dir.back() != '/') {
        dir += '/';
    }
    s_folder_dir = dir;
    s_folder_subdirs.clear();
    s_folder_drives.clear();
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.') {
                continue;
            }
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                struct stat st;
                is_dir = stat((dir + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (is_dir) {
                s_folder_subdirs.push_back(name);
            }
        }
        closedir(d);
    }
    std::sort(s_folder_subdirs.begin(), s_folder_subdirs.end(),
              [](const std::string& a, const std::string& b) {
                  return strcasecmp(a.c_str(), b.c_str()) < 0;
              });
    s_menu = MenuScreen::FolderBrowser;
    s_selected = 0;
}

// The parent of a folder path ending in '/'; the root (e.g. "sdmc:/") is its own.
std::string ParentFolder(const std::string& dir) {
    const std::string trimmed = dir.substr(0, dir.size() - 1);
    const std::size_t slash = trimmed.find_last_of('/');
    return slash == std::string::npos ? dir : trimmed.substr(0, slash + 1);
}

std::string FormatParameter(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    std::string out = text;
    while (out.size() > 1 && out.back() == '0') {
        out.pop_back();
    }
    if (out.back() == '.') {
        out.pop_back();
    }
    return out == "-0" ? "0" : out;
}

// The Shaders category: the preset, its parameters, and a reset row.
std::vector<MenuRow> BuildShaderRows() {
    std::vector<MenuRow> rows;
    MenuRow preset{TrOr("emulator_shader", "Shader")};
    preset.value = s_shader_cb.preset_label ? s_shader_cb.preset_label() : std::string();
    if (preset.value.empty()) {
        preset.value = TrOr("emulator_none", "None");
    }
    rows.push_back(preset);
    const std::vector<ShaderParameter> parameters =
        s_shader_cb.parameters ? s_shader_cb.parameters() : std::vector<ShaderParameter>{};
    for (const ShaderParameter& parameter : parameters) {
        MenuRow row{parameter.label.empty() ? parameter.id : parameter.label};
        row.value = FormatParameter(parameter.value);
        rows.push_back(row);
    }
    if (!parameters.empty()) {
        rows.push_back({TrOr("emulator_reset_parameters", "Reset Parameters")});
    }
    return rows;
}

// Moves the parameter on `row` (1-based after the preset row) by one step.
void StepShaderParameter(int row, int direction) {
    if (!s_shader_cb.parameters || !s_shader_cb.set_parameter || row < 1) {
        return;
    }
    const std::vector<ShaderParameter> parameters = s_shader_cb.parameters();
    if (row > static_cast<int>(parameters.size())) {
        return;
    }
    const ShaderParameter& p = parameters[static_cast<std::size_t>(row - 1)];
    const float step = p.step > 0.0f ? p.step : 0.01f;
    // Snap to the step grid so repeated presses don't accumulate float error.
    float value = p.minimum + std::round((p.value + direction * step - p.minimum) / step) * step;
    value = std::clamp(value, p.minimum, p.maximum);
    s_shader_cb.set_parameter(p.id, value);
}

void OpenShaderBrowser(const std::string& dir) {
    s_browse_dir = dir;
    s_browse_entries =
        s_shader_cb.browse ? s_shader_cb.browse(dir) : std::vector<ShaderBrowseEntry>{};
    s_menu = MenuScreen::ShaderBrowser;
    s_selected = 0;
}

const TicoConfig::OptionCategory& CurrentCategory() {
    const auto& categories = TicoConfig::GetCategories();
    const std::size_t index =
        std::min(static_cast<std::size_t>(std::max(s_category_selected, 0)), categories.size() - 1);
    return categories[index];
}

// The current category's options the menu lists right now; an option can
// depend on another's value (FSR sharpness only shows with the FSR filter).
std::vector<const TicoConfig::OptionDef*> VisibleOptions() {
    std::vector<const TicoConfig::OptionDef*> options;
    if (ExtraCategoryAt(s_category_selected) != ExtraCategory::None) {
        return options;
    }
    const TicoConfig::OptionCategory& category = CurrentCategory();
    for (std::size_t i = 0; i < category.option_count; ++i) {
        if (TicoConfig::IsOptionShown(category.options[i])) {
            options.push_back(&category.options[i]);
        }
    }
    return options;
}

const TicoConfig::OptionDef* SelectedOption() {
    const std::vector<const TicoConfig::OptionDef*> options = VisibleOptions();
    if (s_selected < 0 || s_selected >= static_cast<int>(options.size())) {
        return nullptr;
    }
    return options[static_cast<std::size_t>(s_selected)];
}

void RefreshCheats() {
    s_cheat_entries = s_cheat_list_cb ? s_cheat_list_cb() : std::vector<CheatMenuEntry>{};
}

void RefreshMods() {
    s_mod_entries = s_mod_list_cb ? s_mod_list_cb() : std::vector<ModMenuEntry>{};
}

// Steps the selected mod option's choice; it applies on the next start.
void StepMod(int direction) {
    if (!s_mod_step_cb || s_selected < 0 || s_selected >= static_cast<int>(s_mod_entries.size())) {
        return;
    }
    const ModMenuEntry& entry = s_mod_entries[static_cast<std::size_t>(s_selected)];
    if (entry.source_index >= 0 && s_mod_step_cb(entry.source_index, direction)) {
        RefreshMods();
        ShowToast(TrOr("emulator_applies_next_launch", "Use Restart to apply"));
    }
}

void RefreshSlots() {
    for (int i = 0; i < kOverlaySlotCount; ++i) {
        s_slot_preview[i] = s_slot_preview_cb ? s_slot_preview_cb(i + 1) : SlotPreview{};
        s_slot_occupied[i] = s_slot_occupied_cb ? s_slot_occupied_cb(i + 1) : false;
    }
    s_undo_state = s_undo_state_cb ? s_undo_state_cb() : UndoStateInfo{};
}

void RefreshRewindPoints() {
    s_rewind_points = s_rewind_list_cb ? s_rewind_list_cb() : std::vector<int>{};
}

void RefreshDiscs() {
    s_disc_entries = s_disc_list_cb ? s_disc_list_cb() : std::vector<DiscMenuEntry>{};
}

void ReloadHudPositions() {
    const auto& categories = TicoConfig::GetCategories();
    int fps = 0;
    int resolution = 0;
    for (const auto& category : categories) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            const TicoConfig::OptionDef& option = category.options[i];
            const std::string_view key = option.key;
            if (key != "fps_counter_position" && key != "rendered_ir_position") {
                continue;
            }
            const std::string value = TicoConfig::GetOptionValue(option);
            int position = 0;
            for (std::size_t c = 0; c < option.choice_count; ++c) {
                if (value == option.choices[c].value) {
                    position = static_cast<int>(c);
                }
            }
            (key == "fps_counter_position" ? fps : resolution) = position;
        }
    }
    std::lock_guard lock(s_hud_mutex);
    s_fps_position = fps;
    s_resolution_position = resolution;
}

// The focused category's options, as the settings panel lists them.
std::vector<MenuRow> BuildOptionRows() {
    if (ShaderCategoryActive()) {
        return BuildShaderRows();
    }
    if (LibraryCategoryActive()) {
        return BuildFolderRows();
    }
    if (GameCategoryActive()) {
        return BuildGameRows();
    }
    if (PlayersCategoryActive()) {
        return BuildPlayerRows();
    }
    std::vector<MenuRow> rows;
    for (const TicoConfig::OptionDef* shown : VisibleOptions()) {
        const TicoConfig::OptionDef& option = *shown;
        const TicoConfig::OptionValueLabel value = TicoConfig::GetOptionValueLabel(option);
        MenuRow row{TrLabel(option.label_key, option.fallback)};
        row.value = TrLabel(value.label_key, value.fallback);
        if (row.value.empty()) {
            row.value = "-";
        }
        rows.push_back(row);
    }
    return rows;
}

// The rows of the screen that is currently open.
// The slot (0-based) a row of the Save or Load State list stands for.
int SlotForRow(int row) {
    if (s_menu == MenuScreen::Resume) {
        return kAutoSlotIndex; // its picture and time, whichever row
    }
    if (s_menu == MenuScreen::LoadStates) {
        if (row == 0) {
            return kAutoSlotIndex;
        }
        // the undo rows after the slots stand for no slot
        return row <= kUserSlotCount ? row - 1 : -1;
    }
    return row;
}

std::vector<MenuRow> BuildRows() {
    std::vector<MenuRow> rows;
    switch (s_menu) {
    case MenuScreen::QuickMenu:
        for (const QuickItem item : BuildQuickItems()) {
            rows.push_back({QuickItemLabel(item)});
        }
        break;
    case MenuScreen::SaveStates:
    case MenuScreen::LoadStates: {
        const std::string slot_format = TrOr("emulator_slot", "Slot %d (%s)");
        const std::string in_use = TrOr("emulator_in_use", "In Use");
        const std::string empty = TrOr("emulator_empty", "Empty");
        const std::string auto_format = TrOr("emulator_auto_slot", "Auto (%s)");
        const int row_count = kUserSlotCount + (s_menu == MenuScreen::LoadStates ? 1 : 0);
        for (int r = 0; r < row_count; ++r) {
            const int i = SlotForRow(r);
            const char* state = s_slot_occupied[i] ? in_use.c_str() : empty.c_str();
            char label[128];
            if (i == kAutoSlotIndex) {
                std::snprintf(label, sizeof(label), auto_format.c_str(), state);
            } else {
                std::snprintf(label, sizeof(label), slot_format.c_str(), i + 1, state);
            }
            MenuRow row{label};
            row.dimmed = s_menu == MenuScreen::LoadStates && !s_slot_occupied[i];
            rows.push_back(row);
        }
        if (s_menu == MenuScreen::LoadStates && s_undo_state_cb) {
            MenuRow undo_load{TrOr("emulator_undo_load", "Undo Last Load")};
            undo_load.dimmed = !s_undo_state.can_undo_load;
            rows.push_back(undo_load);

            const bool overwritten = !s_undo_state.overwritten_saved_at.empty();
            char label[160];
            std::snprintf(label, sizeof(label), "%s (%s)",
                          TrOr("emulator_undo_save", "Load Overwritten State").c_str(),
                          overwritten ? s_undo_state.overwritten_saved_at.c_str()
                                      : TrOr("emulator_none", "None").c_str());
            MenuRow undo_save{label};
            undo_save.dimmed = !overwritten;
            rows.push_back(undo_save);
        }
        break;
    }
    case MenuScreen::Rewind: {
        if (s_rewind_points.empty()) {
            MenuRow row{TrOr("emulator_no_rewind_points", "Nothing to rewind to yet")};
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        const std::string format = TrOr("emulator_seconds_ago", "%d seconds ago");
        for (const int seconds : s_rewind_points) {
            char label[96];
            std::snprintf(label, sizeof(label), format.c_str(), seconds);
            rows.push_back({label});
        }
        break;
    }
    case MenuScreen::Discs:
        for (const DiscMenuEntry& entry : s_disc_entries) {
            MenuRow row{entry.name};
            row.has_checkbox = true;
            row.checked = entry.current;
            rows.push_back(row);
        }
        break;
    case MenuScreen::Cheats:
        if (s_cheat_entries.empty()) {
            MenuRow row{TrOr("emulator_no_cheats", "No Cheats")};
            row.dimmed = true;
            rows.push_back(row);
            break;
        }
        for (const CheatMenuEntry& entry : s_cheat_entries) {
            MenuRow row{entry.name};
            if (entry.is_add_row) {
                rows.push_back(row);
                continue;
            }
            row.has_checkbox = true;
            row.checked = entry.enabled;
            row.dimmed = !entry.toggleable;
            rows.push_back(row);
        }
        break;
    case MenuScreen::Mods:
        for (const ModMenuEntry& entry : s_mod_entries) {
            MenuRow row{entry.name, entry.value};
            if (entry.source_index < 0) {
                row.dimmed = true;
                row.static_value = true;
            }
            rows.push_back(row);
        }
        break;
    case MenuScreen::SettingsCategories:
        for (int i = 0; i < CategoryCount(); ++i) {
            rows.push_back({CategoryLabel(i)});
        }
        break;
    case MenuScreen::Library: {
        rows.push_back({TrOr("emulator_settings", "Settings")});
        for (const LibraryEntry& entry : s_library_entries) {
            MenuRow row{entry.title};
            row.value = entry.detail;
            row.static_value = true;
            rows.push_back(row);
        }
        if (s_library_entries.empty()) {
            MenuRow row{TrOr("emulator_no_games", "No games found. Add a folder in Settings > Library")};
            row.dimmed = true;
            rows.push_back(row);
        }
        rows.push_back({TrOr("emulator_exit", "Exit")});
        break;
    }
    case MenuScreen::FolderActions:
        rows.push_back({TrOr("emulator_change_folder", "Change folder")});
        rows.push_back({TrOr("emulator_move_up", "Move up")});
        rows.push_back({TrOr("emulator_move_down", "Move down")});
        rows.push_back({TrOr("emulator_remove", "Remove")});
        rows.push_back({TrOr("emulator_back", "Back")});
        break;
    case MenuScreen::GameConfirm:
        rows.push_back({QuickItemLabel(s_confirm_item)});
        rows.push_back({TrOr("emulator_cancel", "Cancel")});
        break;
    case MenuScreen::FolderConfirm: {
        MenuRow remove{TrOr("emulator_remove", "Remove")};
        remove.value = UsbStorage::DisplayName(EditedFolder());
        remove.static_value = true;
        rows.push_back(remove);
        rows.push_back({TrOr("emulator_cancel", "Cancel")});
        break;
    }
    case MenuScreen::FolderBrowser: {
        MenuRow use{TrOr("emulator_use_folder", "Use this folder")};
        use.value = UsbStorage::DisplayName(s_folder_dir);
        use.static_value = true;
        use.dimmed = s_folder_dir.empty(); // the drive list is not a folder
        rows.push_back(use);
        MenuRow up{".."};
        up.dimmed = s_folder_dir.empty();
        rows.push_back(up);
        for (const std::string& name : s_folder_subdirs) {
            rows.push_back({name + "/"});
        }
        for (const auto& drive : s_folder_drives) {
            rows.push_back({drive.first});
        }
        break;
    }
    case MenuScreen::ShaderBrowser:
        for (const ShaderBrowseEntry& entry : s_browse_entries) {
            rows.push_back({entry.label});
        }
        if (rows.empty()) {
            MenuRow row{TrOr("emulator_no_shaders", "No shaders found")};
            row.dimmed = true;
            rows.push_back(row);
        }
        break;
    case MenuScreen::SettingsOptions:
        rows = BuildOptionRows();
        break;
    case MenuScreen::Notice:
        for (const std::string& choice : s_notice_choices) {
            rows.push_back({choice});
        }
        break;
    case MenuScreen::GameSettingsConfirm:
        rows.push_back({TrOr("emulator_delete", "Delete")});
        rows.push_back({TrOr("emulator_cancel", "Cancel")});
        break;
    case MenuScreen::Resume:
        rows.push_back({TrOr("emulator_continue", "Continue")});
        rows.push_back({TrOr("emulator_start_over", "Start Over")});
        break;
    }
    return rows;
}

std::string BuildTitle() {
    std::string title;
    switch (s_menu) {
    case MenuScreen::SaveStates:
        title = TrOr("emulator_save_state", "Save State");
        break;
    case MenuScreen::LoadStates:
        title = TrOr("emulator_load_state", "Load State");
        break;
    case MenuScreen::Rewind:
        title = TrOr("emulator_rewind", "Rewind");
        break;
    case MenuScreen::Discs:
        title = TrOr("emulator_select_disc", "Change Disc");
        break;
    case MenuScreen::Cheats:
        title = TrOr("emulator_cheats", "Cheats");
        break;
    case MenuScreen::Mods:
        title = TrOr("emulator_mods", "Mods");
        break;
    case MenuScreen::ShaderBrowser:
        title = TrOr("emulator_shader", "Shader");
        break;
    case MenuScreen::FolderBrowser:
        title = TrOr(s_folder_index >= 0 ? "emulator_change_folder" : "emulator_add_folder",
                     s_folder_index >= 0 ? "Change folder" : "Add folder");
        break;
    case MenuScreen::FolderActions:
        title = EditedFolder();
        break;
    case MenuScreen::FolderConfirm:
        title = TrOr("emulator_remove_folder_confirm", "Remove this folder? No files will be deleted.");
        break;
    case MenuScreen::Resume:
        title = TrOr("emulator_resume_title", "Continue where you left off?");
        break;
    case MenuScreen::Notice:
        title = s_notice_message;
        break;
    case MenuScreen::GameSettingsConfirm:
        title = TrOr("emulator_game_settings_delete_confirm",
                     "Delete this game's settings? It will use the shared settings again.");
        break;
    case MenuScreen::GameConfirm:
        title = s_confirm_item == QuickItem::Restart
                    ? TrOr("emulator_restart_confirm",
                           "Restart the game? Progress since your last save will be lost.")
                : s_confirm_item == QuickItem::Reset
                    ? TrOr("emulator_reset_confirm",
                           "Reset the game? Progress since your last save will be lost.")
                    : TrOr("emulator_exit_confirm",
                           "Exit the game? Progress since your last save will be lost.");
        break;
    case MenuScreen::SettingsCategories:
    case MenuScreen::SettingsOptions:
        // the panel names the category itself
        title = TrOr("emulator_settings", "Settings");
        break;
    case MenuScreen::QuickMenu:
    default:
        title = s_title.empty() ? "Dolphin" : s_title;
        break;
    }

    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    std::replace(title.begin(), title.end(), '\t', ' ');
    const std::size_t first = title.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = title.find_last_not_of(' ');
    return title.substr(first, last - first + 1);
}

std::vector<std::string> WrapTitle(const std::string& title, float max_width) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t pos = 0;

    while (pos < title.size()) {
        while (pos < title.size() && title[pos] == ' ') {
            pos++;
        }
        std::size_t end = title.find(' ', pos);
        if (end == std::string::npos) {
            end = title.size();
        }

        std::string word = title.substr(pos, end - pos);
        pos = end;
        if (word.empty()) {
            continue;
        }

        const std::string candidate = current.empty() ? word : current + " " + word;
        if (current.empty() || ImGui::CalcTextSize(candidate.c_str()).x <= max_width) {
            current = candidate;
        } else {
            lines.push_back(current);
            current = word;
        }
    }

    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.empty()) {
        lines.push_back(title);
    }
    if (lines.size() > 3) {
        lines.resize(3);
        lines[2] += "...";
    }
    return lines;
}

Action MakeSaveActionForSlot(int slot_index) {
    return static_cast<Action>(static_cast<int>(Action::SaveStateSlot1) + slot_index);
}

Action MakeLoadActionForSlot(int slot_index) {
    return static_cast<Action>(static_cast<int>(Action::LoadStateSlot1) + slot_index);
}

void RenderOverlayBackground(ImDrawList* dl, ImVec2 display_size, float ease) {
    const int base_alpha = static_cast<int>(200.0f * ease);
    const int max_alpha = static_cast<int>(250.0f * ease);
    if (base_alpha <= 0)
        return;

    const float top_h = display_size.y * 0.20f;
    const float bottom_h = display_size.y * 0.20f;
    const float center_h = display_size.y - top_h - bottom_h;
    const ImU32 col_max = IM_COL32(0, 0, 0, max_alpha);
    const ImU32 col_base = IM_COL32(0, 0, 0, base_alpha);

    dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(display_size.x, top_h), col_max, col_max,
                                col_base, col_base);
    dl->AddRectFilled(ImVec2(0.0f, top_h), ImVec2(display_size.x, top_h + center_h), col_base);
    dl->AddRectFilledMultiColor(ImVec2(0.0f, display_size.y - bottom_h), display_size, col_base,
                                col_base, col_max, col_max);
}

void RenderTitleCard(ImDrawList* dl, ImVec2 display_size, float ease) {
    const std::string title = BuildTitle();
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float title_height = 72.0f * scale;
    const float available_top_space = 110.0f * scale;
    const float card_width = display_size.x * 0.7f;
    const float card_x = (display_size.x - card_width) * 0.5f;
    const float card_y = (available_top_space - title_height) * 0.5f;
    const float start_y = -150.0f * scale;
    const float current_y = start_y + ((card_y - start_y) * ease);
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));
    const std::vector<std::string> lines = WrapTitle(title.empty() ? std::string{"Dolphin"} : title,
                                                     card_width);
    const float line_height = ImGui::GetTextLineHeight();
    const float block_height = line_height * static_cast<float>(lines.size());
    float text_y = current_y + ((title_height - block_height) * 0.5f);

    for (const std::string& line : lines) {
        const ImVec2 text_size = ImGui::CalcTextSize(line.c_str());
        const float text_x = card_x + ((card_width - text_size.x) * 0.5f);
        DrawTextWithShadow(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_x, text_y),
                           text_color, line);
        text_y += line_height;
    }
}

// The first row to draw so that `selected` stays in view, centred while the
// list can scroll.
int FirstVisibleRow(int selected, int count, int visible) {
    if (count <= visible) {
        return 0;
    }
    return std::clamp(selected - (visible / 2), 0, count - visible);
}

// The label, and the value (with change arrows when selected) or checkbox, of
// one row inside [item_min, item_max].
void DrawRowContent(ImDrawList* dl, const MenuRow& row, ImVec2 item_min, ImVec2 item_max,
                    bool selected, float ease, float label_size, bool touch_arrows = false) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const int alpha = static_cast<int>(255.0f * ease);
    const float item_height = item_max.y - item_min.y;
    const ImU32 text_color = RowTextColor(row.dimmed, selected, alpha);
    const float text_x = item_min.x + (20.0f * scale);
    const float right_edge = item_max.x - (28.0f * scale);

    float label_max_width = right_edge - text_x;
    if (row.has_checkbox) {
        const float checkbox_size = 22.0f * scale;
        DrawCheckBox(dl, ImVec2(right_edge - (checkbox_size * 0.5f), item_min.y + (item_height * 0.5f)),
                     checkbox_size, row.checked, ease);
        label_max_width -= checkbox_size + (28.0f * scale);
    } else if (!row.value.empty()) {
        const float arrow_size = 12.0f * scale;
        const float arrow_gap = 12.0f * scale;
        // the value never takes more than half the row
        const std::string value =
            EllipsizeText(font, label_size, row.value,
                          ((item_max.x - item_min.x) * 0.5f) - (2.0f * arrow_size));
        const ImVec2 value_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, value.c_str());
        const float value_x = right_edge - arrow_size - arrow_gap - value_size.x;
        const float value_y = item_min.y + ((item_height - value_size.y) * 0.5f);
        dl->AddText(font, label_size, ImVec2(value_x, value_y), text_color, value.c_str());

        if (selected && !row.static_value) {
            const float arrow_y = item_min.y + ((item_height - arrow_size) * 0.5f);
            const float left_x = value_x - arrow_gap - arrow_size;
            dl->AddTriangleFilled(ImVec2(left_x, arrow_y + (arrow_size * 0.5f)),
                                  ImVec2(left_x + arrow_size, arrow_y),
                                  ImVec2(left_x + arrow_size, arrow_y + arrow_size), text_color);
            const float right_x = value_x + value_size.x + arrow_gap;
            dl->AddTriangleFilled(ImVec2(right_x + arrow_size, arrow_y + (arrow_size * 0.5f)),
                                  ImVec2(right_x, arrow_y), ImVec2(right_x, arrow_y + arrow_size),
                                  text_color);
            if (touch_arrows) {
                // finger-sized targets around each arrow, the value between them
                const float mid_x = value_x + (value_size.x * 0.5f);
                AddHit(ImVec2(left_x - (24.0f * scale), item_min.y), ImVec2(mid_x, item_max.y),
                       HitKind::StepLeft);
                AddHit(ImVec2(mid_x, item_min.y), item_max, HitKind::StepRight);
            }
        }
        label_max_width = value_x - (2.0f * arrow_gap) - arrow_size - text_x;
    }

    const std::string label = EllipsizeText(font, label_size, row.label, label_max_width);
    const ImVec2 text_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label.c_str());
    const float text_y = item_min.y + ((item_height - text_size.y) * 0.5f);
    dl->AddText(font, label_size, ImVec2(text_x, text_y), text_color, label.c_str());
}

// Scroll position for a list that does not fit, drawn as a thin bar at x.
void DrawScrollbar(ImDrawList* dl, float x, float top, float height, int first_visible,
                   int visible_count, int item_count, float ease) {
    if (item_count <= visible_count) {
        return;
    }
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float thumb_height =
        height * static_cast<float>(visible_count) / static_cast<float>(item_count);
    const float thumb_y = top + (height - thumb_height) * static_cast<float>(first_visible) /
                                    static_cast<float>(item_count - visible_count);
    dl->AddRectFilled(ImVec2(x, thumb_y), ImVec2(x + (3.0f * scale), thumb_y + thumb_height),
                      Themed(200, 200, 200, 90, 90, 100, static_cast<int>(120.0f * ease)), 2.0f);
}

// Draws the current screen's rows. Lists longer than kMaxVisibleRows scroll to
// keep the selection in view; rows with a value get change arrows when selected.
// A tooltip beside an item: its name and, when given, a line explaining it.
// The arrow points at the item, which is to the right of the tooltip when
// left_of is set and to its left otherwise.
void DrawTooltip(ImDrawList* dl, float anchor_x, float center_y, bool left_of,
                 const std::string& title, const std::string& hint, int alpha) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const float title_size = ImGui::GetFontSize() * 0.6f;
    const float hint_size = ImGui::GetFontSize() * 0.5f;
    const float pad_x = 12.0f * scale;
    const float pad_y = 8.0f * scale;
    const float wrap = 300.0f * scale;
    const ImVec2 title_text = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title.c_str());
    const ImVec2 hint_text = hint.empty() ? ImVec2(0.0f, 0.0f)
                                          : font->CalcTextSizeA(hint_size, FLT_MAX, wrap, hint.c_str());
    const float gap = hint.empty() ? 0.0f : 4.0f * scale;
    const float w = std::max(title_text.x, hint_text.x) + (2.0f * pad_x);
    const float h = title_text.y + gap + hint_text.y + (2.0f * pad_y);
    const float arrow = 6.0f * scale;
    const float x0 = left_of ? std::max(8.0f * scale, anchor_x - arrow - w) : anchor_x + arrow;
    const ImVec2 tip_min(x0, center_y - (h * 0.5f));
    const ImVec2 tip_max(x0 + w, center_y + (h * 0.5f));
    const ImU32 bg = Themed(36, 36, 36, 250, 252, 255, alpha);
    dl->AddRectFilled(tip_min, tip_max, bg, 8.0f * scale);
    if (left_of) {
        dl->AddTriangleFilled(ImVec2(tip_max.x, center_y - (5.0f * scale)),
                              ImVec2(tip_max.x, center_y + (5.0f * scale)),
                              ImVec2(tip_max.x + arrow, center_y), bg);
    } else {
        dl->AddTriangleFilled(ImVec2(tip_min.x, center_y - (5.0f * scale)),
                              ImVec2(tip_min.x, center_y + (5.0f * scale)),
                              ImVec2(tip_min.x - arrow, center_y), bg);
    }
    dl->AddText(font, title_size, ImVec2(tip_min.x + pad_x, tip_min.y + pad_y),
                Themed(255, 255, 255, 60, 60, 70, alpha), title.c_str());
    if (!hint.empty()) {
        dl->AddText(font, hint_size,
                    ImVec2(tip_min.x + pad_x, tip_min.y + pad_y + title_text.y + gap),
                    Themed(190, 190, 190, 100, 100, 110, alpha), hint.c_str(), nullptr, wrap);
    }
}

// What Reset and Restart do, shown beside them, since the two are easy to mix up.
std::string QuickItemHint(QuickItem item) {
    switch (item) {
    case QuickItem::Reset:
        return TrOr("emulator_reset_hint",
                    "Like pressing the console's reset button: the game starts over without closing.");
    case QuickItem::Restart:
        return TrOr("emulator_restart_hint",
                    "Closes and reopens the game, as if you launched it again from tico.");
    default:
        return std::string();
    }
}

// The quick menu's icon rail, left of the list and centred on it: Settings,
// Restart and Exit Game, the focused one with its name beside it.
void RenderSidebar(ImDrawList* dl, ImVec2 menu_pos, ImVec2 menu_size, float item_height, float ease) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float width = 76.0f * scale;
    const float gap = 12.0f * scale;
    const float corner_radius = 16.0f * scale;
    const int alpha = static_cast<int>(255.0f * ease);
    const float rail_h = item_height * kSidebarCount;
    const ImVec2 rail_min(std::max(24.0f * scale, menu_pos.x - gap - width),
                          menu_pos.y + ((menu_size.y - rail_h) * 0.5f));
    const ImVec2 rail_max(rail_min.x + width, rail_min.y + rail_h);
    dl->AddRectFilled(rail_min, rail_max, PanelColor(alpha), corner_radius);
    AddHit(rail_min, rail_max, HitKind::Panel);

    const float icon_size = 28.0f * scale;
    for (int i = 0; i < kSidebarCount; ++i) {
        const ImVec2 item_min(rail_min.x, rail_min.y + (static_cast<float>(i) * item_height));
        const ImVec2 item_max(rail_max.x, item_min.y + item_height);
        const bool selected = s_side_focus && s_side_selected == i;
        AddHit(item_min, item_max, HitKind::SideItem, i);
        if (selected) {
            DrawSelection(dl, item_min, item_max, corner_radius, ease);
        }
        const ImVec2 center((item_min.x + item_max.x) * 0.5f, (item_min.y + item_max.y) * 0.5f);
        if (s_side_icons[static_cast<std::size_t>(i)]) {
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(s_side_icons[static_cast<std::size_t>(i)])),
                         ImVec2(center.x - (icon_size * 0.5f), center.y - (icon_size * 0.5f)),
                         ImVec2(center.x + (icon_size * 0.5f), center.y + (icon_size * 0.5f)),
                         ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), RowTextColor(false, selected, alpha));
        }
        if (selected) {
            // the item's name (and what it does) in a tooltip left of the rail
            DrawTooltip(dl, item_min.x - (4.0f * scale), center.y, true,
                        QuickItemLabel(kSidebarItems[i]), QuickItemHint(kSidebarItems[i]), alpha);
        }
    }
}

void RenderMenu(ImDrawList* dl, ImVec2 display_size, float ease, const std::vector<MenuRow>& rows) {
    const bool wide = s_menu == MenuScreen::Cheats || s_menu == MenuScreen::Mods ||
                      s_menu == MenuScreen::ShaderBrowser ||
                      s_menu == MenuScreen::Library || s_menu == MenuScreen::FolderBrowser ||
                      s_menu == MenuScreen::FolderActions || s_menu == MenuScreen::FolderConfirm ||
                      s_menu == MenuScreen::GameConfirm || s_menu == MenuScreen::GameSettingsConfirm ||
                      s_menu == MenuScreen::Notice;
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float menu_width = kMenuWidth * (wide ? 1.5f : 1.0f) * scale;
    const float item_height = (wide ? 58.0f : 64.0f) * scale;
    const int item_count = static_cast<int>(rows.size());
    const int visible_count = std::min(item_count, kMaxVisibleRows);
    const ImVec2 menu_size(menu_width, static_cast<float>(visible_count) * item_height);
    const float target_y = (display_size.y - menu_size.y) * 0.5f;
    const float start_y = display_size.y + (100.0f * scale);
    const float current_y = start_y + ((target_y - start_y) * ease);
    const ImVec2 menu_pos((display_size.x - menu_size.x) * 0.5f, current_y);
    const float corner_radius = 16.0f * scale;
    const int alpha = static_cast<int>(255.0f * ease);

    dl->AddRectFilled(menu_pos, ImVec2(menu_pos.x + menu_size.x, menu_pos.y + menu_size.y),
                      PanelColor(alpha), corner_radius);
    AddHit(menu_pos, ImVec2(menu_pos.x + menu_size.x, menu_pos.y + menu_size.y), HitKind::Panel);

    const float label_size = ImGui::GetFontSize() * (wide ? 0.82f : 0.85f);
    const int first_visible = FirstVisibleRow(s_selected, item_count, visible_count);
    const bool quick = s_menu == MenuScreen::QuickMenu;
    if (quick) {
        RenderSidebar(dl, menu_pos, menu_size, item_height, ease);
    }
    for (int row_index = 0; row_index < visible_count; ++row_index) {
        const int i = first_visible + row_index;
        const bool selected = s_selected == i && !(quick && s_side_focus);
        const float item_y = menu_pos.y + (static_cast<float>(row_index) * item_height);
        const ImVec2 item_min(menu_pos.x, item_y);
        const ImVec2 item_max(menu_pos.x + menu_size.x, item_y + item_height);
        if (selected) {
            DrawSelection(dl, item_min, item_max, corner_radius, ease);
        }
        AddHit(item_min, item_max, HitKind::MenuRow, i);
        DrawRowContent(dl, rows[static_cast<std::size_t>(i)], item_min, item_max, selected, ease,
                       label_size);
        if (quick && selected) {
            const std::vector<QuickItem> items = BuildQuickItems();
            if (i < static_cast<int>(items.size())) {
                const std::string hint = QuickItemHint(items[static_cast<std::size_t>(i)]);
                if (!hint.empty()) {
                    DrawTooltip(dl, item_max.x + (10.0f * scale), (item_min.y + item_max.y) * 0.5f,
                                false, QuickItemLabel(items[static_cast<std::size_t>(i)]), hint, alpha);
                }
            }
        }
    }

    DrawScrollbar(dl, menu_pos.x + menu_size.x - (8.0f * scale), menu_pos.y + corner_radius,
                  menu_size.y - (2.0f * corner_radius), first_visible, visible_count, item_count,
                  ease);
}

// Save and Load State as one wide panel: the slots down the left and, beside
// them, the picture taken when the selected slot was saved and when that was.
void RenderStates(ImDrawList* dl, ImVec2 display_size, float ease, const std::vector<MenuRow>& rows) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const int alpha = static_cast<int>(255.0f * ease);
    const float corner_radius = 16.0f * scale;
    const float row_radius = 12.0f * scale;
    const float pad = 14.0f * scale;
    const float row_height = 64.0f * scale;
    const float label_size = ImGui::GetFontSize() * 0.85f;

    // between the title card and the helpers bar, like Settings
    const float top_space = 110.0f * scale;
    const float bottom_space = 96.0f * scale;
    const ImVec2 panel_size(std::min(kSettingsWidth * scale, display_size.x - (64.0f * scale)),
                            std::min(kSettingsHeight * scale,
                                     display_size.y - top_space - bottom_space));
    const float target_y =
        top_space + ((display_size.y - top_space - bottom_space - panel_size.y) * 0.5f);
    const float start_y = display_size.y + (100.0f * scale);
    const ImVec2 panel_min((display_size.x - panel_size.x) * 0.5f,
                           start_y + ((target_y - start_y) * ease));
    const ImVec2 panel_max(panel_min.x + panel_size.x, panel_min.y + panel_size.y);
    const float list_right = panel_min.x + (400.0f * scale);

    dl->AddRectFilled(panel_min, panel_max, PanelColor(alpha), corner_radius);
    dl->AddRectFilled(panel_min, ImVec2(list_right, panel_max.y), PanelShadeColor(alpha),
                      corner_radius, ImDrawFlags_RoundCornersLeft);
    AddHit(panel_min, panel_max, HitKind::Panel);

    // the slots, centred down the list
    const int count = static_cast<int>(rows.size());
    const float list_h = static_cast<float>(count) * row_height;
    const float list_top = panel_min.y + std::max(pad, (panel_size.y - list_h) * 0.5f);
    for (int i = 0; i < count; ++i) {
        const ImVec2 item_min(panel_min.x + pad, list_top + (static_cast<float>(i) * row_height));
        const ImVec2 item_max(list_right - pad, item_min.y + row_height);
        const bool selected = i == s_selected;
        if (selected) {
            DrawSelection(dl, item_min, item_max, row_radius, ease);
        }
        AddHit(item_min, item_max, HitKind::MenuRow, i);
        DrawRowContent(dl, rows[static_cast<std::size_t>(i)], item_min, item_max, selected, ease,
                       label_size);
    }

    // the selected slot's picture, as large as the pane allows
    const float pane_left = list_right + (2.0f * pad);
    const float pane_right = panel_max.x - (2.0f * pad);
    const float caption_h = 48.0f * scale;
    const float pane_top = panel_min.y + (2.0f * pad);
    const float pane_bottom = panel_max.y - (2.0f * pad) - caption_h;
    static const SlotPreview kNoPreview{};
    const int preview_slot = SlotForRow(s_selected);
    const SlotPreview& preview =
        preview_slot < 0
            ? kNoPreview
            : s_slot_preview[static_cast<std::size_t>(std::min(preview_slot, kOverlaySlotCount - 1))];
    const float aspect = preview.aspect > 0.1f ? preview.aspect : (4.0f / 3.0f);
    float pic_w = pane_right - pane_left;
    float pic_h = pic_w / aspect;
    if (pic_h > pane_bottom - pane_top) {
        pic_h = pane_bottom - pane_top;
        pic_w = pic_h * aspect;
    }
    const ImVec2 pic_min(pane_left + ((pane_right - pane_left - pic_w) * 0.5f),
                         pane_top + ((pane_bottom - pane_top - pic_h) * 0.5f));
    const ImVec2 pic_max(pic_min.x + pic_w, pic_min.y + pic_h);
    if (preview.texture) {
        dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(preview.texture)), pic_min, pic_max,
                            ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                            IM_COL32(255, 255, 255, alpha), row_radius);
    } else {
        dl->AddRectFilled(pic_min, pic_max, Themed(28, 28, 28, 225, 229, 234, alpha), row_radius);
        const std::string none = preview.saved_at.empty() ? TrOr("emulator_empty", "Empty")
                                                          : TrOr("emulator_no_preview", "No preview");
        const ImVec2 none_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, none.c_str());
        dl->AddText(font, label_size,
                    ImVec2(pic_min.x + ((pic_w - none_size.x) * 0.5f),
                           pic_min.y + ((pic_h - none_size.y) * 0.5f)),
                    Themed(150, 150, 150, 130, 130, 140, alpha), none.c_str());
    }
    if (!preview.saved_at.empty()) {
        const float caption_size = ImGui::GetFontSize() * 0.7f;
        const ImVec2 caption_text = font->CalcTextSizeA(caption_size, FLT_MAX, 0.0f, preview.saved_at.c_str());
        dl->AddText(font, caption_size,
                    ImVec2(pane_left + ((pane_right - pane_left - caption_text.x) * 0.5f),
                           pic_max.y + ((caption_h - caption_text.y) * 0.5f) + pad * 0.5f),
                    Themed(170, 170, 170, 90, 90, 100, alpha), preview.saved_at.c_str());
    }
}

// Settings as one wide panel: the categories down a sidebar on the left and the
// focused category's options on the right. Up and down move through the
// categories, previewing their options; A or right moves into the options,
// and B returns to the sidebar.
void RenderSettings(ImDrawList* dl, ImVec2 display_size, float ease) {
    const bool options_focused = s_menu == MenuScreen::SettingsOptions;
    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const int alpha = static_cast<int>(255.0f * ease);
    const float corner_radius = 16.0f * scale;
    const float row_radius = 12.0f * scale;
    const float pad = 14.0f * scale;
    const float row_height = kSettingsRowHeight * scale;
    const float label_size = ImGui::GetFontSize() * 0.8f;

    // between the title card and the helpers bar
    const float top_space = 110.0f * scale;
    const float bottom_space = 96.0f * scale;
    const ImVec2 panel_size(std::min(kSettingsWidth * scale, display_size.x - (64.0f * scale)),
                            std::min(kSettingsHeight * scale,
                                     display_size.y - top_space - bottom_space));
    const float target_y =
        top_space + ((display_size.y - top_space - bottom_space - panel_size.y) * 0.5f);
    const float start_y = display_size.y + (100.0f * scale);
    const ImVec2 panel_min((display_size.x - panel_size.x) * 0.5f,
                           start_y + ((target_y - start_y) * ease));
    const ImVec2 panel_max(panel_min.x + panel_size.x, panel_min.y + panel_size.y);
    const float sidebar_right = panel_min.x + (kSidebarWidth * scale);

    dl->AddRectFilled(panel_min, panel_max, PanelColor(alpha), corner_radius);
    AddHit(panel_min, panel_max, HitKind::Panel);
    dl->AddRectFilled(panel_min, ImVec2(sidebar_right, panel_max.y), PanelShadeColor(alpha),
                      corner_radius, ImDrawFlags_RoundCornersLeft);

    // sidebar
    const int category_count = CategoryCount();
    const int sidebar_visible = std::min(
        category_count, std::max(1, static_cast<int>((panel_size.y - (2.0f * pad)) / row_height)));
    const int first_category = FirstVisibleRow(s_category_selected, category_count, sidebar_visible);
    for (int row_index = 0; row_index < sidebar_visible; ++row_index) {
        const int i = first_category + row_index;
        const bool active = i == s_category_selected;
        const ImVec2 item_min(panel_min.x + pad,
                              panel_min.y + pad + (static_cast<float>(row_index) * row_height));
        const ImVec2 item_max(sidebar_right - pad, item_min.y + row_height);
        if (active && options_focused) {
            // where the options came from, while focus is on them
            dl->AddRectFilled(item_min, item_max, Themed(56, 56, 56, 214, 219, 226, alpha), row_radius);
            const float bar_height = row_height * 0.45f;
            const float bar_y = item_min.y + ((row_height - bar_height) * 0.5f);
            DrawTintSwatch(dl, ImVec2(item_min.x + (6.0f * scale), bar_y),
                           ImVec2(item_min.x + (10.0f * scale), bar_y + bar_height), 2.0f * scale,
                           ease);
        } else if (active) {
            DrawSelection(dl, item_min, item_max, row_radius, ease);
        }
        AddHit(item_min, item_max, HitKind::CategoryRow, i);
        MenuRow row{CategoryLabel(i)};
        DrawRowContent(dl, row, item_min, item_max, active, ease, label_size);
    }
    DrawScrollbar(dl, sidebar_right - (6.0f * scale), panel_min.y + corner_radius,
                  panel_size.y - (2.0f * corner_radius), first_category, sidebar_visible,
                  category_count, ease);

    // options pane: the category name over its options
    const float pane_left = sidebar_right + pad;
    const float pane_right = panel_max.x - pad;
    const std::string heading = CategoryLabel(s_category_selected);
    const float heading_size = ImGui::GetFontSize() * 0.95f;
    const float heading_height = 56.0f * scale;
    const ImVec2 heading_text_size =
        font->CalcTextSizeA(heading_size, FLT_MAX, 0.0f, heading.c_str());
    const float heading_x = pane_left + (20.0f * scale);
    DrawTextWithShadow(dl, font, heading_size,
                       ImVec2(heading_x, panel_min.y + pad +
                                             ((heading_height - heading_text_size.y) * 0.5f)),
                       Themed(235, 235, 235, 40, 40, 50, alpha),
                       EllipsizeText(font, heading_size, heading, pane_right - heading_x));
    const float rule_y = panel_min.y + pad + heading_height;
    dl->AddLine(ImVec2(heading_x, rule_y), ImVec2(pane_right - (20.0f * scale), rule_y),
                Themed(80, 80, 80, 205, 210, 216, alpha), 1.0f * scale);

    // a line under the list says when the focused option applies on the next start
    const float footer_height = 40.0f * scale;
    const float list_top = rule_y + (8.0f * scale);
    const float list_bottom = panel_max.y - pad - footer_height;
    const std::vector<MenuRow> rows = BuildOptionRows();
    const int option_count = static_cast<int>(rows.size());
    const int option_visible = std::min(
        option_count, std::max(1, static_cast<int>((list_bottom - list_top) / row_height)));
    const int focused = options_focused ? s_selected : -1;
    const int first_option = FirstVisibleRow(std::max(focused, 0), option_count, option_visible);
    for (int row_index = 0; row_index < option_visible; ++row_index) {
        const int i = first_option + row_index;
        const bool selected = i == focused;
        const ImVec2 item_min(pane_left, list_top + (static_cast<float>(row_index) * row_height));
        const ImVec2 item_max(pane_right, item_min.y + row_height);
        if (selected) {
            DrawSelection(dl, item_min, item_max, row_radius, ease);
        }
        // the arrows go first so they win over the row they sit on
        DrawRowContent(dl, rows[static_cast<std::size_t>(i)], item_min, item_max, selected, ease,
                       label_size, selected);
        AddHit(item_min, item_max, HitKind::OptionRow, i);
    }
    DrawScrollbar(dl, panel_max.x - (8.0f * scale), list_top, list_bottom - list_top, first_option,
                  option_visible, option_count, ease);

    const TicoConfig::OptionDef* option = options_focused ? SelectedOption() : nullptr;
    const bool restart_note = option && option->needs_restart;
    if (restart_note || (TicoConfig::GameSettingsActive() && !GameCategoryActive())) {
        const std::string note =
            restart_note ? TrOr("emulator_applies_next_launch", "Use Restart to apply")
                         : TrOr("emulator_game_only_note", "Saving for this game only");
        const float note_size = ImGui::GetFontSize() * 0.62f;
        const ImVec2 note_text_size = font->CalcTextSizeA(note_size, FLT_MAX, 0.0f, note.c_str());
        dl->AddText(font, note_size,
                    ImVec2(heading_x, list_bottom + ((footer_height - note_text_size.y) * 0.5f)),
                    Themed(160, 160, 160, 110, 110, 120, alpha),
                    EllipsizeText(font, note_size, note, pane_right - heading_x).c_str());
    }
}

void RenderHelpersBar(ImDrawList* dl, ImVec2 display_size, float ease) {
    struct Helper {
        const char* button;
        std::string_view description;
    };

    // on the resume prompt B starts the game over
    const std::string back = s_menu == MenuScreen::Resume ? TrOr("emulator_start_over", "Start Over")
                                                          : TrOr("emulator_back", "Back");
    std::string accept = TrOr("emulator_select", "Select");
    if (s_menu == MenuScreen::SaveStates)
        accept = TrOr("emulator_save_state", "Save State");
    else if (s_menu == MenuScreen::LoadStates)
        accept = TrOr("emulator_load_state", "Load State");
    else if (s_menu == MenuScreen::Rewind)
        accept = TrOr("emulator_rewind", "Rewind");
    else if (s_menu == MenuScreen::Discs)
        accept = TrOr("emulator_change", "Change");
    else if (s_menu == MenuScreen::Cheats && !s_cheat_entries.empty())
        accept = TrOr("emulator_toggle", "Toggle");
    else if (s_menu == MenuScreen::Mods)
        accept = TrOr("emulator_change", "Change");
    else if (s_menu == MenuScreen::SettingsOptions)
        accept = TrOr("emulator_change", "Change");
    else if (s_menu == MenuScreen::ShaderBrowser || s_menu == MenuScreen::FolderBrowser)
        accept = TrOr("emulator_select", "Select");

    const std::array<Helper, 2> helpers = {{
        {"B", back},
        {"A", accept},
    }};

    const float scale = ImGui::GetIO().FontGlobalScale;
    const float bar_height = 48.0f * scale;
    const float margin_bottom = 24.0f * scale;
    const float padding = 16.0f * scale;
    const float button_size = 22.0f * scale;
    const float item_spacing = 12.0f * scale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.78f;

    float total_width = padding * 2.0f;
    for (size_t i = 0; i < helpers.size(); ++i) {
        const char* const text_begin = helpers[i].description.data();
        const char* const text_end = text_begin + helpers[i].description.size();
        total_width += button_size + (8.0f * scale) +
                       font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end).x;
        if (i + 1 < helpers.size())
            total_width += item_spacing;
    }

    const float current_offset = (400.0f * scale) * (1.0f - ease);
    const float bar_x = display_size.x - total_width - 20.0f + current_offset;
    const float bar_y = display_size.y - margin_bottom - bar_height;
    float cursor_x = bar_x + padding;
    const float center_y = bar_y + (bar_height * 0.5f);
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

    for (const Helper& helper : helpers) {
        const float helper_x = cursor_x;
        DrawSwitchButton(dl, font, font_size, ImVec2(cursor_x + (button_size * 0.5f), center_y),
                         button_size, helper.button, ease);

        cursor_x += button_size + (8.0f * scale);
        const char* const text_begin = helper.description.data();
        const char* const text_end = text_begin + helper.description.size();
        const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end);
        dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (text_size.y * 0.5f)), text_color,
                    text_begin, text_end);
        cursor_x += text_size.x + item_spacing;
        AddHit(ImVec2(helper_x - (item_spacing * 0.5f), bar_y),
               ImVec2(cursor_x - (item_spacing * 0.5f), bar_y + bar_height),
               helper.button[0] == 'B' ? HitKind::Back : HitKind::Accept);
    }
}

void RenderSocialArea(ImDrawList* dl, float ease) {
    if (ease <= 0.0f)
        return;

    const float scale = ImGui::GetIO().FontGlobalScale;
    const float start_offset = 200.0f * scale;
    const float current_offset = start_offset * (1.0f - ease);
    const float avatar_size = 72.0f * scale;
    const float side_margin = 32.0f * scale;
    const float top_margin = 32.0f * scale;
    const float bar_height = 50.0f * scale;
    const ImVec2 avatar_center(side_margin + (avatar_size * 0.5f) - current_offset,
                               top_margin + (bar_height * 0.5f));
    const float radius = avatar_size * 0.5f;

    dl->AddCircleFilled(avatar_center, radius, IM_COL32(45, 45, 45, static_cast<int>(255.0f * ease)));

    const float image_radius = radius - (4.0f * scale);
    if (s_avatar_texture_id != 0) {
        const ImVec2 image_min(avatar_center.x - image_radius, avatar_center.y - image_radius);
        const ImVec2 image_max(avatar_center.x + image_radius, avatar_center.y + image_radius);
        dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(s_avatar_texture_id)), image_min,
                            image_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE,
                            image_radius);
        dl->AddCircle(avatar_center, image_radius, IM_COL32(255, 255, 255, 60), 0, 1.0f);
    } else {
        dl->AddCircleFilled(avatar_center, image_radius, IM_COL32(200, 200, 210, 255));
    }
}

void RenderStatusBar(ImDrawList* dl, ImVec2 display_size, float ease) {
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float bar_height = 50.0f * scale;
    const float top_margin = 32.0f * scale;
    const float side_margin = 18.0f * scale;
    const float item_spacing = 20.0f * scale;
    const float padding = 20.0f * scale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();

    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    char time_str[16] = "00:00";
    if (localtime_r(&now, &local_tm))
        std::strftime(time_str, sizeof(time_str), "%H:%M", &local_tm);

    u32 battery_level = 0;
    const bool has_battery = psmGetBatteryChargePercentage(&battery_level) == 0;
    PsmChargerType charger_type = PsmChargerType_Unconnected;
    const bool charging =
        psmGetChargerType(&charger_type) == 0 && charger_type != PsmChargerType_Unconnected;

    float total_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;
    if (has_battery) {
        total_width += item_spacing + (34.0f * scale);
        if (charging)
            total_width += 20.0f * scale;
    }
    total_width += padding * 2.0f;

    const float bar_x = display_size.x - total_width - side_margin;
    const float bar_y = top_margin + ((1.0f - ease) * -20.0f);
    const float center_y = bar_y + (bar_height * 0.5f);
    float cursor_x = bar_x + padding;
    const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

    dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (font_size * 0.5f)), text_color,
                time_str);
    cursor_x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;

    if (!has_battery)
        return;

    cursor_x += item_spacing;
    const float body_width = 32.0f * scale;
    const float body_height = 20.0f * scale;
    const float tip_width = 4.0f * scale;
    const float tip_height = 10.0f * scale;
    const ImVec2 body_min(cursor_x, center_y - (body_height * 0.5f));
    const ImVec2 body_max(body_min.x + body_width, body_min.y + body_height);

    dl->AddRect(body_min, body_max, text_color, 3.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(body_max.x, body_min.y + ((body_height - tip_height) * 0.5f)),
                      ImVec2(body_max.x + tip_width, body_min.y + ((body_height + tip_height) * 0.5f)),
                      text_color, 2.0f, ImDrawFlags_RoundCornersRight);

    const float pct = std::clamp(static_cast<float>(battery_level) / 100.0f, 0.0f, 1.0f);
    if (pct > 0.0f) {
        const float pad = 4.0f * scale;
        const ImVec2 fill_min(body_min.x + pad, body_min.y + pad);
        const ImVec2 fill_max(fill_min.x + ((body_width - (pad * 2.0f)) * pct), body_max.y - pad);
        dl->AddRectFilled(fill_min, fill_max, text_color, 1.5f);
    }

    if (charging) {
        const float icon_height = 16.0f * scale;
        const float icon_width = icon_height * (448.0f / 512.0f);
        const ImVec2 icon_min(body_max.x + tip_width + (6.0f * scale),
                              center_y - (icon_height * 0.5f));
        const auto BoltPoint = [&](float x, float y) {
            return ImVec2(icon_min.x + ((x / 448.0f) * icon_width),
                          icon_min.y + ((y / 512.0f) * icon_height));
        };

        dl->PathLineTo(BoltPoint(338.8f, -9.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(350.7f, -1.3f), BoltPoint(355.1f, 14.3f),
                                   BoltPoint(349.7f, 27.9f));
        dl->PathLineTo(BoltPoint(271.3f, 224.0f));
        dl->PathLineTo(BoltPoint(416.0f, 224.0f));
        dl->PathBezierCubicCurveTo(BoltPoint(429.5f, 224.0f), BoltPoint(441.5f, 232.4f),
                                   BoltPoint(446.1f, 245.1f));
        dl->PathBezierCubicCurveTo(BoltPoint(450.7f, 257.8f), BoltPoint(446.8f, 272.0f),
                                   BoltPoint(436.5f, 280.6f));
        dl->PathLineTo(BoltPoint(148.5f, 520.6f));
        dl->PathBezierCubicCurveTo(BoltPoint(137.2f, 530.0f), BoltPoint(121.1f, 530.5f),
                                   BoltPoint(109.2f, 521.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(97.3f, 513.3f), BoltPoint(92.9f, 497.7f),
                                   BoltPoint(98.3f, 484.1f));
        dl->PathLineTo(BoltPoint(176.7f, 288.0f));
        dl->PathLineTo(BoltPoint(32.0f, 288.0f));
        dl->PathBezierCubicCurveTo(BoltPoint(18.5f, 288.0f), BoltPoint(6.5f, 279.6f),
                                   BoltPoint(1.9f, 266.9f));
        dl->PathBezierCubicCurveTo(BoltPoint(-2.7f, 254.2f), BoltPoint(1.2f, 240.0f),
                                   BoltPoint(11.5f, 231.4f));
        dl->PathLineTo(BoltPoint(299.5f, -8.6f));
        dl->PathBezierCubicCurveTo(BoltPoint(310.8f, -18.0f), BoltPoint(326.9f, -18.5f),
                                   BoltPoint(338.8f, -9.9f));
        dl->PathFillConcave(IM_COL32(235, 235, 235, static_cast<int>(255.0f * ease)));
    }
}

int GetToastSlot(ToastCorner corner) {
    switch (corner) {
    case ToastCorner::TopLeft:
        return 0;
    case ToastCorner::TopRight:
        return 1;
    case ToastCorner::BottomLeft:
        return 2;
    case ToastCorner::BottomRight:
        return 3;
    }
    return 0;
}

bool GetAndAdvanceToast(ToastCorner corner, float delta_time, std::string* message, float* timer) {
    std::lock_guard lock(s_toast_mutex);
    const int slot = GetToastSlot(corner);
    if (s_toast_messages[slot].empty() || s_toast_timers[slot] <= 0.0f)
        return false;

    *message = s_toast_messages[slot];
    *timer = s_toast_timers[slot];
    s_toast_timers[slot] = std::max(0.0f, s_toast_timers[slot] - delta_time);
    if (s_toast_timers[slot] <= 0.0f)
        s_toast_messages[slot].clear();

    return true;
}

void RenderToastSlot(ImDrawList* dl, ToastCorner corner, float delta_time) {
    std::string message;
    float timer = 0.0f;
    if (!GetAndAdvanceToast(corner, delta_time, &message, &timer))
        return;

    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float alpha =
        timer < kToastFadeDuration ? std::clamp(timer / kToastFadeDuration, 0.0f, 1.0f) : 1.0f;
    const float margin_x = 24.0f * scale;
    const float margin_y = 16.0f * scale;
    const float pad_x = 16.0f * scale;
    const float pad_y = 8.0f * scale;
    const float rounding = 14.0f * scale;
    const ImVec2 text_size = ImGui::CalcTextSize(message.c_str());
    const float toast_w = text_size.x + pad_x * 2.0f;
    const float toast_h = text_size.y + pad_y * 2.0f;
    float toast_x = margin_x;
    float toast_y = margin_y;

    if (corner == ToastCorner::TopRight || corner == ToastCorner::BottomRight)
        toast_x = std::max(margin_x, display_size.x - margin_x - toast_w);
    if (corner == ToastCorner::BottomLeft || corner == ToastCorner::BottomRight)
        toast_y = std::max(margin_y, display_size.y - margin_y - toast_h);

    const ImVec2 p0(toast_x, toast_y);
    const ImVec2 p1(p0.x + text_size.x + pad_x * 2.0f, p0.y + text_size.y + pad_y * 2.0f);

    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(153.0f * alpha)), rounding);
    dl->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y),
                IM_COL32(255, 255, 255, static_cast<int>(240.0f * alpha)), message.c_str());
}

void RenderToast(ImDrawList* dl, float delta_time) {
    RenderToastSlot(dl, ToastCorner::TopLeft, delta_time);
    RenderToastSlot(dl, ToastCorner::TopRight, delta_time);
    RenderToastSlot(dl, ToastCorner::BottomLeft, delta_time);
    RenderToastSlot(dl, ToastCorner::BottomRight, delta_time);
}

// The FPS counter, rendered resolution and fast-forward marker, each in the
// corner its setting asks for. Position 0 is hidden; 1..4 are the corners in
// the order top left, top right, bottom left, bottom right.
void RenderHud(ImDrawList* dl, ImVec2 display_size) {
    HudStats stats;
    int fps_position;
    int resolution_position;
    {
        std::lock_guard lock(s_hud_mutex);
        stats = s_hud_stats;
        fps_position = s_fps_position;
        resolution_position = s_resolution_position;
    }

    std::array<std::string, 5> lines{};
    if (fps_position > 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f FPS", stats.fps);
        lines[fps_position] = text;
    }
    if (resolution_position > 0 && stats.rendered_width > 0 && stats.rendered_height > 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%dx%d", stats.rendered_width, stats.rendered_height);
        std::string& line = lines[resolution_position];
        line = line.empty() ? text : line + "  " + text;
    }
    if (stats.fast_forward) {
        std::string& line = lines[2];
        line = line.empty() ? std::string(">>") : std::string(">>  ") + line;
    }

    const float scale = ImGui::GetIO().FontGlobalScale;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.6f;
    const float margin = 12.0f * scale;
    const float pad_x = 10.0f * scale;
    const float pad_y = 4.0f * scale;
    for (int position = 1; position <= 4; ++position) {
        const std::string& line = lines[position];
        if (line.empty()) {
            continue;
        }
        const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.c_str());
        const float box_w = text_size.x + (pad_x * 2.0f);
        const float box_h = text_size.y + (pad_y * 2.0f);
        const bool right = position == 2 || position == 4;
        const bool bottom = position == 3 || position == 4;
        const ImVec2 p0(right ? display_size.x - margin - box_w : margin,
                        bottom ? display_size.y - margin - box_h : margin);
        dl->AddRectFilled(p0, ImVec2(p0.x + box_w, p0.y + box_h), IM_COL32(0, 0, 0, 140),
                          8.0f * scale);
        dl->AddText(font, font_size, ImVec2(p0.x + pad_x, p0.y + pad_y),
                    IM_COL32(255, 255, 255, 230), line.c_str());
    }
}

bool HudActive() {
    std::lock_guard lock(s_hud_mutex);
    return s_fps_position > 0 || s_resolution_position > 0 || s_hud_stats.fast_forward;
}

void OpenScreen(MenuScreen screen) {
    s_menu = screen;
    s_selected = 0;
}

bool IsActionInRange(Action action, Action first, Action last) {
    const int value = static_cast<int>(action);
    return value >= static_cast<int>(first) && value <= static_cast<int>(last);
}

// The game's own settings were saved or deleted: the core gets the values
// that now apply (deleting brings the shared ones back).
void OnGameSettingsChanged() {
    ReloadHudPositions();
    std::lock_guard lock(s_pending_mutex);
    s_settings_changed = true;
}

// A setting was changed from the menu: tell the emulation thread, and say so
// when the change only takes effect on the next launch.
void OnOptionChanged(const TicoConfig::OptionDef& option) {
    ReloadHudPositions();
    {
        std::lock_guard lock(s_pending_mutex);
        s_settings_changed = true;
    }
    if (option.needs_restart) {
        ShowToast(TrOr("emulator_applies_next_launch", "Use Restart to apply"),
                  ToastCorner::TopRight);
    }
}

// What A does on the selected row. Returns the action for the emulation thread, if any.
Action AcceptSelection(const std::vector<MenuRow>& rows) {
    if (rows.empty() || s_selected < 0 || s_selected >= static_cast<int>(rows.size())) {
        return Action::None;
    }

    switch (s_menu) {
    case MenuScreen::QuickMenu: {
        const std::vector<QuickItem> items = BuildQuickItems();
        s_quick_selected = s_selected;
        switch (items[static_cast<std::size_t>(s_selected)]) {
        case QuickItem::SaveState:
            RefreshSlots();
            OpenScreen(MenuScreen::SaveStates);
            break;
        case QuickItem::LoadState:
            RefreshSlots();
            OpenScreen(MenuScreen::LoadStates);
            // the auto save first when there is one, else slot 1
            s_selected = s_slot_occupied[kAutoSlotIndex] ? 0 : 1;
            break;
        case QuickItem::Rewind:
            RefreshRewindPoints();
            OpenScreen(MenuScreen::Rewind);
            break;
        case QuickItem::ChangeDisc:
            OpenScreen(MenuScreen::Discs);
            for (std::size_t i = 0; i < s_disc_entries.size(); ++i) {
                if (s_disc_entries[i].current) {
                    s_selected = static_cast<int>(i);
                }
            }
            break;
        case QuickItem::Cheats:
            RefreshCheats();
            OpenScreen(MenuScreen::Cheats);
            break;
        case QuickItem::Mods:
            RefreshMods();
            OpenScreen(MenuScreen::Mods);
            break;
        case QuickItem::Settings:
            OpenScreen(MenuScreen::SettingsCategories);
            break;
        case QuickItem::Reset:
        case QuickItem::Restart:
        case QuickItem::Exit:
            // asked first, with Cancel selected so a held button cannot confirm
            s_confirm_item = items[static_cast<std::size_t>(s_selected)];
            s_confirm_from_side = false;
            s_menu = MenuScreen::GameConfirm;
            s_selected = 1;
            return Action::None;
        }
        return Action::None;
    }
    case MenuScreen::SaveStates:
        return MakeSaveActionForSlot(SlotForRow(s_selected));
    case MenuScreen::LoadStates:
        if (SlotForRow(s_selected) < 0) {
            // the undo rows
            if (s_selected == kUserSlotCount + 1) {
                return s_undo_state.can_undo_load ? Action::UndoLoadState : Action::None;
            }
            return s_undo_state.overwritten_saved_at.empty() ? Action::None : Action::UndoSaveState;
        }
        if (!s_slot_occupied[static_cast<std::size_t>(SlotForRow(s_selected))]) {
            return Action::None;
        }
        return MakeLoadActionForSlot(SlotForRow(s_selected));
    case MenuScreen::Rewind: {
        if (s_rewind_points.empty()) {
            return Action::None;
        }
        std::lock_guard lock(s_pending_mutex);
        s_rewind_index = s_selected;
        return Action::Rewind;
    }
    case MenuScreen::Discs: {
        if (s_disc_entries[static_cast<std::size_t>(s_selected)].current) {
            return Action::None;
        }
        std::lock_guard lock(s_pending_mutex);
        s_disc_index = s_selected;
        return Action::SwapDisc;
    }
    case MenuScreen::Cheats:
        if (!s_cheat_entries.empty() && s_cheat_toggle_cb) {
            const CheatMenuEntry& entry = s_cheat_entries[static_cast<std::size_t>(s_selected)];
            if (entry.is_add_row) {
                return Action::AddCheat;
            }
            if (entry.toggleable && s_cheat_toggle_cb(entry.source_index)) {
                RefreshCheats();
            }
        }
        return Action::None;
    case MenuScreen::Mods:
        StepMod(1);
        return Action::None;
    case MenuScreen::SettingsCategories:
        s_category_selected = s_selected;
        OpenScreen(MenuScreen::SettingsOptions);
        return Action::None;
    case MenuScreen::Library: {
        s_quick_selected = s_selected;
        const int game_rows = static_cast<int>(s_library_entries.size());
        if (s_selected == 0) {
            OpenScreen(MenuScreen::SettingsCategories);
            return Action::None;
        }
        if (s_selected == static_cast<int>(rows.size()) - 1) {
            return Action::Exit;
        }
        if (game_rows > 0 && s_selected <= game_rows && s_library_cb.launch) {
            s_library_cb.launch(s_library_entries[static_cast<std::size_t>(s_selected - 1)].path);
        }
        return Action::None;
    }
    case MenuScreen::FolderBrowser: {
        if (s_folder_dir.empty()) {
            // the drive list: open the chosen drive
            if (s_selected >= 2 && s_selected - 2 < static_cast<int>(s_folder_drives.size())) {
                OpenFolderBrowser(s_folder_drives[static_cast<std::size_t>(s_selected - 2)].second);
            }
            return Action::None;
        }
        if (s_selected == 1 && IsDriveRoot(s_folder_dir)) {
            const std::string from = s_folder_dir;
            OpenDriveList();
            for (std::size_t i = 0; i < s_folder_drives.size(); ++i) {
                if (s_folder_drives[i].second == from) {
                    s_selected = static_cast<int>(i) + 2;
                }
            }
            return Action::None;
        }
        if (s_selected == 0) {
            if (StoreFolder(s_folder_dir)) {
                RefreshLibrary();
            } else {
                ShowToast(TrOr("emulator_folder_exists", "Folder already added"), ToastCorner::TopRight);
            }
            const int group = s_folder_group;
            const int index = s_folder_index >= 0 ? s_folder_index
                                                  : static_cast<int>(FolderGroups()[static_cast<std::size_t>(group)].folders.size()) - 1;
            s_menu = MenuScreen::SettingsOptions;
            s_selected = FolderRowFor(group, index);
        } else if (s_selected == 1) {
            const std::string from = s_folder_dir;
            OpenFolderBrowser(ParentFolder(s_folder_dir));
            for (std::size_t i = 0; i < s_folder_subdirs.size(); ++i) {
                if (s_folder_dir + s_folder_subdirs[i] + "/" == from) {
                    s_selected = static_cast<int>(i) + 2;
                }
            }
        } else {
            OpenFolderBrowser(s_folder_dir + s_folder_subdirs[static_cast<std::size_t>(s_selected - 2)]);
        }
        return Action::None;
    }
    case MenuScreen::FolderActions: {
        std::vector<LibraryFolderGroup> groups = FolderGroups();
        if (s_folder_group >= static_cast<int>(groups.size()) || !s_folder_cb.set) {
            OpenScreen(MenuScreen::SettingsOptions);
            return Action::None;
        }
        std::vector<std::string> folders = groups[static_cast<std::size_t>(s_folder_group)].folders;
        if (s_folder_index < 0 || s_folder_index >= static_cast<int>(folders.size())) {
            OpenScreen(MenuScreen::SettingsOptions);
            return Action::None;
        }
        switch (s_selected) {
        case 0: // change
            OpenFolderBrowser(folders[static_cast<std::size_t>(s_folder_index)]);
            break;
        case 1: // move up
            if (s_folder_index > 0) {
                std::swap(folders[static_cast<std::size_t>(s_folder_index)],
                          folders[static_cast<std::size_t>(s_folder_index - 1)]);
                --s_folder_index;
                s_folder_cb.set(s_folder_group, folders);
                RefreshLibrary();
            }
            break;
        case 2: // move down
            if (s_folder_index + 1 < static_cast<int>(folders.size())) {
                std::swap(folders[static_cast<std::size_t>(s_folder_index)],
                          folders[static_cast<std::size_t>(s_folder_index + 1)]);
                ++s_folder_index;
                s_folder_cb.set(s_folder_group, folders);
                RefreshLibrary();
            }
            break;
        case 3: // remove, after asking
            s_menu = MenuScreen::FolderConfirm;
            s_selected = 1; // the safe choice
            break;
        default: // back
            s_menu = MenuScreen::SettingsOptions;
            s_selected = FolderRowFor(s_folder_group, s_folder_index);
            break;
        }
        return Action::None;
    }
    case MenuScreen::Resume:
        return s_selected == 0 ? MakeLoadActionForSlot(kAutoSlotIndex) : Action::Resume;
    case MenuScreen::Notice: {
        std::lock_guard lock(s_pending_mutex);
        s_notice_choice = s_selected;
        return Action::NoticeChoice;
    }
    case MenuScreen::GameSettingsConfirm:
        if (s_selected == 0) {
            TicoConfig::DeleteGameSettings();
            OnGameSettingsChanged();
            ShowToast(TrOr("emulator_game_settings_deleted", "This game uses the shared settings again"),
                      ToastCorner::TopRight);
        }
        s_menu = MenuScreen::SettingsOptions;
        s_selected = 0;
        return Action::None;
    case MenuScreen::GameConfirm:
        if (s_selected == 0) {
            return s_confirm_item == QuickItem::Restart ? Action::Restart
                   : s_confirm_item == QuickItem::Reset ? Action::Reset
                                                        : Action::Exit;
        }
        OpenScreen(MenuScreen::QuickMenu);
        s_selected = s_quick_selected;
        s_side_focus = s_confirm_from_side;
        return Action::None;
    case MenuScreen::FolderConfirm: {
        if (s_selected == 0) {
            std::vector<LibraryFolderGroup> groups = FolderGroups();
            if (s_folder_group < static_cast<int>(groups.size()) && s_folder_cb.set) {
                std::vector<std::string> folders = groups[static_cast<std::size_t>(s_folder_group)].folders;
                if (s_folder_index >= 0 && s_folder_index < static_cast<int>(folders.size())) {
                    folders.erase(folders.begin() + s_folder_index);
                    s_folder_cb.set(s_folder_group, folders);
                    RefreshLibrary();
                }
            }
            s_menu = MenuScreen::SettingsOptions;
            s_selected = FolderRowFor(s_folder_group, -1);
        } else {
            s_menu = MenuScreen::FolderActions;
            s_selected = 3;
        }
        return Action::None;
    }
    case MenuScreen::ShaderBrowser: {
        if (s_browse_entries.empty()) {
            return Action::None;
        }
        const ShaderBrowseEntry entry = s_browse_entries[static_cast<std::size_t>(s_selected)];
        if (entry.is_dir) {
            const std::string from = s_browse_dir;
            OpenShaderBrowser(entry.path);
            // going up lands on the folder we came from
            for (std::size_t i = 0; i < s_browse_entries.size(); ++i) {
                if (s_browse_entries[i].path == from) {
                    s_selected = static_cast<int>(i);
                }
            }
            return Action::None;
        }
        if (s_shader_cb.select) {
            s_shader_cb.select(entry.path);
        }
        s_menu = MenuScreen::SettingsOptions;
        s_selected = 0;
        return Action::None;
    }
    case MenuScreen::SettingsOptions: {
        if (PlayersCategoryActive()) {
            return s_selected == static_cast<int>(rows.size()) - 1 ? Action::ControllerOrder
                                                                    : Action::None;
        }
        if (GameCategoryActive()) {
            if (s_selected == 0) {
                if (TicoConfig::GameSettingsActive()) {
                    s_menu = MenuScreen::GameSettingsConfirm;
                    s_selected = 1; // the safe choice
                } else {
                    TicoConfig::SaveGameSettings();
                    OnGameSettingsChanged();
                    ShowToast(TrOr("emulator_game_settings_saved", "Settings saved for this game"),
                              ToastCorner::TopRight);
                }
            }
            return Action::None;
        }
        if (LibraryCategoryActive()) {
            const std::vector<LibraryFolderGroup> groups = FolderGroups();
            const std::vector<FolderEntry> entries = FolderEntries(groups);
            if (s_selected < 0 || s_selected >= static_cast<int>(entries.size())) {
                return Action::None;
            }
            const FolderEntry entry = entries[static_cast<std::size_t>(s_selected)];
            s_folder_group = entry.group;
            if (entry.kind == FolderEntry::Add &&
                groups[static_cast<std::size_t>(entry.group)].folders.size() < kMaxLibraryFolders) {
                s_folder_index = -1;
                // with a USB drive connected, start from the drives to choose from
                if (UsbStorage::Volumes().empty()) {
                    OpenFolderBrowser("sdmc:/");
                } else {
                    OpenDriveList();
                }
            } else if (entry.kind == FolderEntry::Folder) {
                s_folder_index = entry.index;
                s_menu = MenuScreen::FolderActions;
                s_selected = 0;
            }
            return Action::None;
        }
        if (ShaderCategoryActive()) {
            const int parameter_count = static_cast<int>(rows.size()) - 2;
            if (s_selected == 0) {
                OpenShaderBrowser(s_shader_cb.browse_start ? s_shader_cb.browse_start()
                                                           : std::string());
            } else if (parameter_count > 0 && s_selected == static_cast<int>(rows.size()) - 1 &&
                       s_shader_cb.reset_parameters) {
                s_shader_cb.reset_parameters();
            }
            return Action::None;
        }
        const TicoConfig::OptionDef* selected = SelectedOption();
        if (!selected) {
            return Action::None;
        }
        const TicoConfig::OptionDef& option = *selected;
        if (option.type == TicoConfig::OptionType::Text) {
            std::lock_guard lock(s_pending_mutex);
            s_text_edit_option = &option;
            return Action::EditText;
        }
        TicoConfig::StepOption(option, 1);
        OnOptionChanged(option);
        return Action::None;
    }
    }
    return Action::None;
}

// What A does on the sidebar: Settings opens; Restart and Exit ask first.
Action ActivateSidebarItem() {
    const QuickItem item = kSidebarItems[std::clamp(s_side_selected, 0, kSidebarCount - 1)];
    s_quick_selected = s_selected;
    if (item == QuickItem::Settings) {
        OpenScreen(MenuScreen::SettingsCategories);
        return Action::None;
    }
    s_confirm_item = item;
    s_confirm_from_side = true;
    s_menu = MenuScreen::GameConfirm;
    s_selected = 1; // Cancel
    return Action::None;
}

// What B does: one level up, or close the menu from the top.
Action CancelScreen() {
    switch (s_menu) {
    case MenuScreen::QuickMenu:
    case MenuScreen::Resume: // B starts the game over
        return Action::Resume;
    case MenuScreen::Library:
        // the library is the root while no game runs; Exit leaves it
        break;
    case MenuScreen::FolderBrowser:
        if (s_folder_index >= 0) {
            s_menu = MenuScreen::FolderActions;
            s_selected = 0;
        } else {
            s_menu = MenuScreen::SettingsOptions;
            s_selected = FolderRowFor(s_folder_group, -1);
        }
        break;
    case MenuScreen::FolderActions:
        s_menu = MenuScreen::SettingsOptions;
        s_selected = FolderRowFor(s_folder_group, s_folder_index);
        break;
    case MenuScreen::FolderConfirm:
        s_menu = MenuScreen::FolderActions;
        s_selected = 3;
        break;
    case MenuScreen::GameSettingsConfirm:
        s_menu = MenuScreen::SettingsOptions;
        s_selected = 0;
        break;
    case MenuScreen::Notice: {
        // B answers with the last choice (the way out)
        std::lock_guard lock(s_pending_mutex);
        s_notice_choice = static_cast<int>(s_notice_choices.size()) - 1;
        return Action::NoticeChoice;
    }
    case MenuScreen::SettingsOptions:
        s_menu = MenuScreen::SettingsCategories;
        s_selected = s_category_selected;
        break;
    case MenuScreen::ShaderBrowser:
        s_menu = MenuScreen::SettingsOptions;
        s_selected = 0;
        break;
    default:
        s_menu = RootScreen();
        s_selected = s_quick_selected;
        break;
    }
    return Action::None;
}

// The first region drawn under pos; a panel only when nothing on it is.
const HitRegion* HitTest(ImVec2 pos) {
    const HitRegion* panel = nullptr;
    for (const HitRegion& hit : s_hits) {
        if (pos.x >= hit.min.x && pos.x < hit.max.x && pos.y >= hit.min.y && pos.y < hit.max.y) {
            if (hit.kind != HitKind::Panel) {
                return &hit;
            }
            panel = panel ? panel : &hit;
        }
    }
    return panel;
}

bool IsSelectedRow(HitKind kind, int index) {
    switch (kind) {
    case HitKind::MenuRow:
        return s_selected == index;
    case HitKind::CategoryRow:
        return s_menu == MenuScreen::SettingsCategories && s_selected == index;
    case HitKind::OptionRow:
        return s_menu == MenuScreen::SettingsOptions && s_selected == index;
    case HitKind::SideItem:
        return s_side_focus && s_side_selected == index;
    default:
        return false;
    }
}

// Puts focus on a row: settings rows also move focus between the sidebar and
// the options pane.
void SelectRow(HitKind kind, int index) {
    if (kind == HitKind::SideItem) {
        s_side_focus = true;
        s_side_selected = index;
        return;
    }
    if (kind == HitKind::MenuRow && s_menu == MenuScreen::QuickMenu) {
        s_side_focus = false;
    }
    if (kind == HitKind::CategoryRow) {
        s_menu = MenuScreen::SettingsCategories;
        s_category_selected = index;
    } else if (kind == HitKind::OptionRow) {
        s_menu = MenuScreen::SettingsOptions;
    }
    s_selected = index;
}

// Turns this frame's touch into navigation, using the regions the previous
// frame drew. display_size is the overlay's; touch comes in 1280x720.
NavInput ApplyTouch(ImVec2 display_size, int item_count, NavInput nav) {
    const TouchInput touch = s_touch;
    const bool pressed = touch.down && !s_touch_was_down;
    const bool released = !touch.down && s_touch_was_down;
    s_touch_was_down = touch.down;
    if (s_touch_ignore) {
        s_touch_ignore = touch.down;
        return nav;
    }
    if (!touch.down && !released) {
        return nav;
    }

    const ImVec2 pos = touch.down ? ImVec2(touch.x * display_size.x / 1280.0f,
                                           touch.y * display_size.y / 720.0f)
                                  : s_touch_last;
    s_touch_last = pos;
    const float scale = ImGui::GetIO().FontGlobalScale;

    if (pressed) {
        const HitRegion* hit = HitTest(pos);
        if (!hit) {
            nav.cancel = true; // outside the menu
            return nav;
        }
        s_touch_tracking = true;
        s_touch_moved = false;
        s_touch_start = pos;
        s_touch_kind = hit->kind;
        s_touch_index = hit->index;
        s_touch_scrolled = 0;
        s_touch_row_height = hit->max.y - hit->min.y;
        s_touch_started_selected = IsSelectedRow(hit->kind, hit->index);
        if (hit->kind == HitKind::MenuRow || hit->kind == HitKind::CategoryRow ||
            hit->kind == HitKind::OptionRow || hit->kind == HitKind::SideItem) {
            SelectRow(hit->kind, hit->index);
        }
        return nav;
    }
    if (!s_touch_tracking) {
        return nav;
    }

    if (touch.down) {
        if (std::fabs(pos.x - s_touch_start.x) > 18.0f * scale ||
            std::fabs(pos.y - s_touch_start.y) > 18.0f * scale) {
            s_touch_moved = true;
        }
        // dragging a list moves its selection a row at a time, scrolling it
        const bool list = s_touch_kind == HitKind::MenuRow ||
                          s_touch_kind == HitKind::CategoryRow ||
                          s_touch_kind == HitKind::OptionRow;
        if (list && s_touch_moved && s_touch_row_height > 0.0f && item_count > 0) {
            const int rows = static_cast<int>((s_touch_start.y - pos.y) / s_touch_row_height);
            if (rows != s_touch_scrolled) {
                s_selected = std::clamp(s_selected + rows - s_touch_scrolled, 0, item_count - 1);
                if (s_touch_kind == HitKind::CategoryRow) {
                    s_category_selected = s_selected;
                }
                s_touch_scrolled = rows;
            }
        }
        return nav;
    }

    // released: a tap if it neither moved nor left where it started
    s_touch_tracking = false;
    const HitRegion* hit = HitTest(pos);
    if (s_touch_moved || !hit || hit->kind != s_touch_kind || hit->index != s_touch_index) {
        return nav;
    }
    switch (hit->kind) {
    case HitKind::MenuRow:
    case HitKind::OptionRow:
    case HitKind::SideItem:
        nav.accept = s_touch_started_selected;
        break;
    case HitKind::CategoryRow:
        nav.right = s_touch_started_selected; // into its options
        break;
    case HitKind::StepLeft:
        nav.left = true;
        break;
    case HitKind::StepRight:
        nav.right = true;
        break;
    case HitKind::Back:
        nav.cancel = true;
        break;
    case HitKind::Accept:
        nav.accept = true;
        break;
    case HitKind::Panel:
        break;
    }
    return nav;
}

} // namespace

void SetVisible(bool visible) {
    if (visible && !s_visible) {
        OverlayTranslation::TranslationManager::Instance().Init();
        s_anim_timer = 0.0f;
        s_quick_selected = 0;
        s_category_selected = 0;
        RefreshDiscs();
        if (s_library_mode) {
            RefreshLibrary();
        }
        OpenScreen(RootScreen());
        s_dark = TicoConfig::DarkMode(); // tico's theme may have changed since
        s_side_focus = false;
        s_side_selected = 0;
        s_hits.clear();
        s_touch_tracking = false;
        s_touch_ignore = true; // until the finger that may be down lifts
    } else if (!visible) {
        s_anim_timer = 0.0f;
        s_cheat_entries.clear();
        s_mod_entries.clear();
        s_rewind_points.clear();
        s_disc_entries.clear();
        OpenScreen(RootScreen());
    }

    s_visible = visible;
}

void ShowNotice(std::string message, std::vector<std::string> choices) {
    s_notice_message = std::move(message);
    s_notice_choices = std::move(choices);
    if (s_notice_choices.empty()) {
        s_notice_choices.push_back(TrOr("emulator_ok", "OK"));
    }
    OpenScreen(MenuScreen::Notice);
}

int ConsumeNoticeChoice() {
    std::lock_guard lock(s_pending_mutex);
    const int choice = s_notice_choice;
    s_notice_choice = -1;
    return choice;
}

void SetPlayerCallbacks(PlayerCallbacks callbacks) {
    s_player_cb = std::move(callbacks);
}

void ShowResumePrompt() {
    RefreshSlots();
    OpenScreen(MenuScreen::Resume);
}

void SetHardcoreMode(bool hardcore) {
    s_hardcore = hardcore;
}

void SetGameTitle(std::string title) {
    s_title = std::move(title);
}

void SetNickname(std::string nickname) {
    s_nickname = std::move(nickname);
}

void SetAvatarTextureId(unsigned long long texture_id) {
    s_avatar_texture_id = texture_id;
}

void SetSidebarIconTextures(unsigned long long settings, unsigned long long restart,
                            unsigned long long exit) {
    s_side_icons = {settings, restart, exit};
}

void SetBorderTextureId(unsigned long long texture_id) {
    s_border_texture_id = texture_id;
}

void SetSlotOccupiedCallback(SlotOccupiedFn callback) {
    s_slot_occupied_cb = std::move(callback);
}

void SetUndoStateCallback(UndoStateFn callback) {
    s_undo_state_cb = std::move(callback);
    s_undo_state = {};
}

void SetSlotPreviewCallback(SlotPreviewFn callback) {
    s_slot_preview_cb = std::move(callback);
    s_slot_preview = {};
}

void SetCheatCallbacks(CheatListFn list_callback, CheatToggleFn toggle_callback) {
    s_cheat_list_cb = std::move(list_callback);
    s_cheat_toggle_cb = std::move(toggle_callback);
    s_cheat_entries.clear();
}

void RefreshCheatList() {
    RefreshCheats();
}

void SetModCallbacks(ModListFn list_callback, ModStepFn step_callback) {
    s_mod_list_cb = std::move(list_callback);
    s_mod_step_cb = std::move(step_callback);
    s_mod_entries.clear();
}

void SetRewindCallback(RewindListFn callback) {
    s_rewind_list_cb = std::move(callback);
    s_rewind_points.clear();
}

void SetLibraryCallbacks(LibraryCallbacks callbacks) {
    s_library_cb = std::move(callbacks);
    s_library_entries.clear();
}

void SetLibraryMode(bool library) {
    s_library_mode = library;
}

void RefreshLibrary() {
    s_library_entries = s_library_cb.list ? s_library_cb.list() : std::vector<LibraryEntry>{};
}

void SetLibraryFolderCallbacks(LibraryFolderCallbacks callbacks) {
    s_folder_cb = std::move(callbacks);
}

void SetShaderCallbacks(ShaderCallbacks callbacks) {
    s_shader_cb = std::move(callbacks);
    s_browse_entries.clear();
}

void SetDiscCallback(DiscListFn callback) {
    s_disc_list_cb = std::move(callback);
    s_disc_entries.clear();
}

void SetHudStats(const HudStats& stats) {
    std::lock_guard lock(s_hud_mutex);
    s_hud_stats = stats;
}

void ReloadSettings() {
    ReloadHudPositions();
}

void ShowToast(std::string message, ToastCorner corner) {
    std::lock_guard lock(s_toast_mutex);
    if (message.empty()) {
        for (std::string& toast_message : s_toast_messages)
            toast_message.clear();
        s_toast_timers.fill(0.0f);
        return;
    }

    const int slot = GetToastSlot(corner);
    s_toast_messages[slot] = std::move(message);
    s_toast_timers[slot] = kToastDuration;
}

bool HasTransientContent() {
    if (HudActive()) {
        return true;
    }
    std::lock_guard lock(s_toast_mutex);
    for (int i = 0; i < kToastSlotCount; ++i) {
        if (!s_toast_messages[i].empty() && s_toast_timers[i] > 0.0f)
            return true;
    }
    return false;
}

bool ConsumeSettingsChanged() {
    std::lock_guard lock(s_pending_mutex);
    const bool changed = s_settings_changed;
    s_settings_changed = false;
    return changed;
}

int ConsumeRewindIndex() {
    std::lock_guard lock(s_pending_mutex);
    const int index = s_rewind_index;
    s_rewind_index = -1;
    return index;
}

int ConsumeDiscIndex() {
    std::lock_guard lock(s_pending_mutex);
    const int index = s_disc_index;
    s_disc_index = -1;
    return index;
}

const TicoConfig::OptionDef* ConsumeTextEditOption() {
    std::lock_guard lock(s_pending_mutex);
    const TicoConfig::OptionDef* option = s_text_edit_option;
    s_text_edit_option = nullptr;
    return option;
}

void NotifyOptionEdited(const TicoConfig::OptionDef& option) {
    OnOptionChanged(option);
}

void FeedNav(const NavInput& nav) {
    s_nav = nav;
}

void FeedTouch(const TouchInput& touch) {
    s_touch = touch;
}

Action Render(int display_w, int display_h) {
    const float delta_time = ImGui::GetIO().DeltaTime;
    const ImVec2 display_size(static_cast<float>(display_w), static_cast<float>(display_h));
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (!s_visible) {
        RenderHud(dl, display_size);
        RenderToast(dl, delta_time);
        return Action::None;
    }

    NavInput nav = s_nav;
    s_nav = {};

    s_anim_timer = std::min(s_anim_timer + delta_time, kAnimDuration);
    const float ease = EaseOutCubic(s_anim_timer / kAnimDuration);

    std::vector<MenuRow> rows = BuildRows();
    int item_count = static_cast<int>(rows.size());
    if (item_count > 0) {
        s_selected = std::clamp(s_selected, 0, item_count - 1);
    }
    // only once the menu has slid in, so rows are hit where they are drawn
    if (ease >= 1.0f) {
        const MenuScreen before = s_menu;
        nav = ApplyTouch(display_size, item_count, nav);
        if (s_menu != before) {
            rows = BuildRows();
            item_count = static_cast<int>(rows.size());
        }
    }
    // the quick menu's sidebar: left onto it, right back to the list; it takes
    // the d-pad and A while it has focus
    Action side_action = Action::None;
    if (s_menu == MenuScreen::QuickMenu) {
        if (nav.left && !s_side_focus) {
            // land on the sidebar item level with the row: the rail is centred
            // on the list, so it starts (rows - items) / 2 rows down; a row
            // exactly between two items goes to the lower one
            const float offset = (static_cast<float>(item_count) - kSidebarCount) * 0.5f;
            s_side_selected = std::clamp(
                static_cast<int>(std::floor(static_cast<float>(s_selected) - offset + 0.5f)), 0,
                kSidebarCount - 1);
            s_side_focus = true;
            nav.left = false;
        } else if (nav.right && s_side_focus) {
            s_side_focus = false;
            nav.right = false;
        }
        if (s_side_focus) {
            if (nav.up)
                s_side_selected = (s_side_selected + kSidebarCount - 1) % kSidebarCount;
            if (nav.down)
                s_side_selected = (s_side_selected + 1) % kSidebarCount;
            nav.up = nav.down = false;
            if (nav.accept) {
                nav.accept = false;
                side_action = ActivateSidebarItem();
                rows = BuildRows();
                item_count = static_cast<int>(rows.size());
            }
        }
    }
    if (item_count > 0) {
        s_selected = std::clamp(s_selected, 0, item_count - 1);
        if (nav.up)
            s_selected = (s_selected - 1 + item_count) % item_count;
        if (nav.down)
            s_selected = (s_selected + 1) % item_count;
    }

    const TicoConfig::OptionDef* stepped =
        s_menu == MenuScreen::SettingsOptions && (nav.left || nav.right) ? SelectedOption() : nullptr;
    if (s_menu == MenuScreen::SettingsOptions && ShaderCategoryActive() && (nav.left || nav.right)) {
        StepShaderParameter(s_selected, nav.right ? 1 : -1);
        rows = BuildRows();
    }
    if (s_menu == MenuScreen::Mods && (nav.left || nav.right)) {
        StepMod(nav.right ? 1 : -1);
        rows = BuildRows();
    }
    if (stepped) {
        const TicoConfig::OptionDef& option = *stepped;
        if (option.type != TicoConfig::OptionType::Text || option.choice_count > 0) {
            TicoConfig::StepOption(option, nav.right ? 1 : -1);
            OnOptionChanged(option);
            rows = BuildRows();
        }
    }

    // after the options were stepped, so the right that opens them changes nothing
    if (s_menu == MenuScreen::SettingsCategories) {
        s_category_selected = s_selected;
        if (nav.right && !nav.accept) {
            OpenScreen(MenuScreen::SettingsOptions);
            rows = BuildRows();
        }
    }

    Action result = Action::None;
    if (nav.cancel) {
        result = CancelScreen();
        rows = BuildRows();
    }

    s_hits.clear();
    RenderOverlayBackground(dl, display_size, ease);
    RenderTitleCard(dl, display_size, ease);
    if (s_menu == MenuScreen::SettingsCategories || s_menu == MenuScreen::SettingsOptions) {
        RenderSettings(dl, display_size, ease);
    } else if (s_menu == MenuScreen::SaveStates || s_menu == MenuScreen::LoadStates ||
               s_menu == MenuScreen::Resume) {
        RenderStates(dl, display_size, ease, rows);
    } else {
        RenderMenu(dl, display_size, ease, rows);
    }
    RenderHelpersBar(dl, display_size, ease);
    RenderSocialArea(dl, ease);
    RenderStatusBar(dl, display_size, ease);
    RenderToast(dl, delta_time);

    if (side_action != Action::None) {
        result = side_action;
    }
    if (nav.accept && result == Action::None) {
        result = AcceptSelection(rows);
    }

    return result;
}

bool IsSaveStateAction(Action action) {
    return IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot6);
}

bool IsLoadStateAction(Action action) {
    return IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot6);
}

int GetStateSlotForAction(Action action) {
    if (IsSaveStateAction(action))
        return static_cast<int>(action) - static_cast<int>(Action::SaveStateSlot1) + 1;
    if (IsLoadStateAction(action))
        return static_cast<int>(action) - static_cast<int>(Action::LoadStateSlot1) + 1;
    return 0;
}

} // namespace SwitchFrontend::OverlayUI
