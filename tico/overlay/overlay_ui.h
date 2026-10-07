// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "overlay/tico_config.h"

namespace SwitchFrontend::OverlayUI {

enum class Action {
    None,
    Resume,
    Exit,
    Reset,
    // reload the running game from disk, as if it were started again
    Restart,
    // the point to go back to is given by ConsumeRewindIndex
    Rewind,
    // a text setting needs the system keyboard; see ConsumeTextEditOption
    EditText,
    // the "add custom cheat" row was chosen
    AddCheat,
    // the disc to insert is given by ConsumeDiscIndex
    SwapDisc,
    // a ShowNotice choice was made; ConsumeNoticeChoice says which
    NoticeChoice,
    // Settings > Players: open the system's controller screen
    ControllerOrder,
    SaveStateSlot1,
    SaveStateSlot2,
    SaveStateSlot3,
    SaveStateSlot4,
    SaveStateSlot5,
    SaveStateSlot6,
    LoadStateSlot1,
    LoadStateSlot2,
    LoadStateSlot3,
    LoadStateSlot4,
    LoadStateSlot5,
    LoadStateSlot6,
};

enum class ToastCorner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// The slot (1-based) the frontend saves to automatically when the game closes;
// Load State lists it first, as "Auto". Slots 1-5 are the player's.
constexpr int kAutoStateSlot = 6;

bool IsSaveStateAction(Action action);
bool IsLoadStateAction(Action action);
int GetStateSlotForAction(Action action);

void SetVisible(bool visible);

Action Render(int display_w, int display_h);

void SetGameTitle(std::string title);
// RetroAchievements hardcore: Load State, Rewind and Cheats leave the menu.
void SetHardcoreMode(bool hardcore);
// Asks whether to continue from the auto save (its picture and when it was
// made) or start over. Call right after showing the overlay; Continue returns
// the load action for kAutoStateSlot, Start Over (or B) Action::Resume.
void ShowResumePrompt();

// A message that has to be answered before anything else (e.g. a missing
// BIOS): `message` as the title and one row per choice. Choosing returns
// Action::NoticeChoice; B picks the last choice.
void ShowNotice(std::string message, std::vector<std::string> choices);
int ConsumeNoticeChoice();

// Settings > Players: what each port has and a way to change the order.
struct PlayerCallbacks {
    // one entry per port, empty when nothing is connected to it
    std::function<std::vector<std::string>()> ports;
    // a line under the ports (e.g. that arcade boards read two), or empty
    std::function<std::string()> note;
};
// Pass a default-constructed struct to remove the category. Its last row
// returns Action::ControllerOrder.
void SetPlayerCallbacks(PlayerCallbacks callbacks);
void SetNickname(std::string nickname);
void SetAvatarTextureId(unsigned long long texture_id);
// The selection border strip for the tint picked in tico (0 when there is
// none; selected rows then get a plain highlight).
void SetBorderTextureId(unsigned long long texture_id);
// The quick menu's sidebar icons: Settings, Restart and Exit Game (0 for none).
void SetSidebarIconTextures(unsigned long long settings, unsigned long long restart,
                            unsigned long long exit);
void ShowToast(std::string message, ToastCorner corner = ToastCorner::TopLeft);
bool HasTransientContent();

// Predicate the overlay uses to label save/load slots as "In Use" or "Empty".
// The Switch frontend registers one that checks for the slot's state file.
using SlotOccupiedFn = std::function<bool(int slot)>;
void SetSlotOccupiedCallback(SlotOccupiedFn callback);

// What the Save/Load State panel shows beside a slot: the picture taken when
// it was saved (0 when there is none) and when that was.
struct SlotPreview {
    unsigned long long texture = 0;
    float aspect = 4.0f / 3.0f; // width / height to draw the picture at
    std::string saved_at;       // e.g. "2026-10-04 03:21"; empty for an empty slot
};
// Called for each slot (1-based) when Save or Load State opens. The frontend
// owns the textures and frees the previous one for a slot when asked again.
using SlotPreviewFn = std::function<SlotPreview(int slot)>;
void SetSlotPreviewCallback(SlotPreviewFn callback);

struct CheatMenuEntry {
    std::string name;
    bool enabled = false;
    bool toggleable = true;
    int source_index = -1;
    // a row that runs an action (Action::AddCheat) instead of toggling
    bool is_add_row = false;
};

using CheatListFn = std::function<std::vector<CheatMenuEntry>()>;
using CheatToggleFn = std::function<bool(int source_index)>;
void SetCheatCallbacks(CheatListFn list_callback, CheatToggleFn toggle_callback);
// Re-reads the cheat list, e.g. after a cheat was added outside the menu.
void RefreshCheatList();

// Lists the rewind points as seconds before now, nearest first.
using RewindListFn = std::function<std::vector<int>()>;
void SetRewindCallback(RewindListFn callback);

// The discs of a multi-disc game. The quick menu offers Change Disc when there
// is more than one; the list is read again each time the menu opens.
struct DiscMenuEntry {
    std::string name;
    bool current = false;
};
using DiscListFn = std::function<std::vector<DiscMenuEntry>()>;
void SetDiscCallback(DiscListFn callback);

// Shaders: a "Shaders" category in Settings, listed only in game (tico cannot
// see the presets on the SD card). Its first row opens a browser over preset
// folders; one row per #pragma parameter of the active preset follows, then a
// row that resets them.
struct ShaderBrowseEntry {
    std::string label;
    std::string path;
    bool is_dir = false;
};
struct ShaderParameter {
    std::string id;
    std::string label;
    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
};
struct ShaderCallbacks {
    // name of the active preset, for the first row
    std::function<std::string()> preset_label;
    // folder the browser opens in
    std::function<std::string()> browse_start;
    // entries of a folder, its parent first
    std::function<std::vector<ShaderBrowseEntry>(const std::string& dir)> browse;
    // a preset was chosen
    std::function<void(const std::string& path)> select;
    std::function<std::vector<ShaderParameter>()> parameters;
    std::function<void(const std::string& id, float value)> set_parameter;
    std::function<void()> reset_parameters;
};
// Pass a default-constructed struct to remove the category.
void SetShaderCallbacks(ShaderCallbacks callbacks);

// Library: when the core starts without a game, the menu's root is a list of
// the games found in the ROM folders instead of the quick menu, with Settings
// on top and Exit (Action::Exit) at the bottom.
struct LibraryEntry {
    std::string title;
    // shown on the right, e.g. the system
    std::string detail;
    std::string path;
};
struct LibraryCallbacks {
    // the games, read again each time the library opens
    std::function<std::vector<LibraryEntry>()> list;
    // a game was chosen
    std::function<void(const std::string& path)> launch;
};
void SetLibraryCallbacks(LibraryCallbacks callbacks);
// Whether the menu's root is the library (no game running) or the quick menu.
void SetLibraryMode(bool library);
// Reads the game list again, e.g. after the folders changed.
void RefreshLibrary();

// A "Library" category in Settings: the folders searched for games, one group
// per console. A group's base folders (tico's ROM bases) are listed but not
// edited here; its own folders are an ordered list edited like Dolphin's game
// folders: "Add folder", then per folder Change folder, Move up, Move down and
// Remove (which asks first).
struct LibraryFolderGroup {
    std::string label;
    std::vector<std::string> bases;
    std::vector<std::string> folders;
};
struct LibraryFolderCallbacks {
    std::function<std::vector<LibraryFolderGroup>()> groups;
    // the new ordered folder list of a group
    std::function<void(int group, const std::vector<std::string>& folders)> set;
};
void SetLibraryFolderCallbacks(LibraryFolderCallbacks callbacks);

// The overlay renders on the presentation thread and never calls into the
// emulator. The emulation thread polls these after each frame.

// True once after settings were changed from the menu.
bool ConsumeSettingsChanged();
// The rewind point chosen with Action::Rewind, or -1.
int ConsumeRewindIndex();
// The disc chosen with Action::SwapDisc, or -1.
int ConsumeDiscIndex();
// The option chosen with Action::EditText, or nullptr.
const TicoConfig::OptionDef* ConsumeTextEditOption();
// Call after storing a value for an option outside the menu (text entry), so
// the change is picked up like any other.
void NotifyOptionEdited(const TicoConfig::OptionDef& option);

// Figures shown by the FPS counter and rendered-resolution display.
struct HudStats {
    float fps = 0.0f;
    // the frame the core renders; 0 when unknown
    int rendered_width = 0;
    int rendered_height = 0;
    bool fast_forward = false;
};
void SetHudStats(const HudStats& stats);
// Re-reads the HUD positions from the config; call once it has been loaded.
void ReloadSettings();

struct NavInput {
    bool up;
    bool down;
    bool left;
    bool right;
    bool accept;
    bool cancel;
};
void FeedNav(const NavInput& nav);

// The touchscreen this frame, in its 1280x720 coordinates.
struct TouchInput {
    bool down;
    float x;
    float y;
};
void FeedTouch(const TouchInput& touch);

} // namespace SwitchFrontend::OverlayUI
