// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SwitchFrontend::TicoConfig {

// Loads the tico config for the Dolphin core from the first existing path in
// the search list (sdmc:/tico/config/cores/dolphin.jsonc and fallbacks). Safe
// to call repeatedly; a missing file simply yields an empty option set.
void ReloadConfig();

// The Animated Border tint picked in tico's Screen Colors (display.jsonc
// "border_tint"), indexing tico-nx's TintPalette. 0 when unset.
int BorderTint();
// tico's theme (dark_mode in display.jsonc); tico starts in the light one.
bool DarkMode();
// tico's General > Continue Last Game (resume_on_launch in general.jsonc):
// "ask", "always" or "never", for a game that has an auto save.
std::string ResumeOnLaunch();

// Returns the string value for `key`, or `default_value` if the key is absent.
std::string GetConfigValue(std::string_view key, std::string_view default_value = {});

// Sets an option in memory (call SaveConfig to persist it to the writable path).
void SetConfigValue(const std::string& key, const std::string& value);

// Writes the current option set back to the writable config path as JSON.
bool SaveConfig();

// A structured value (an object or array) as JSON text, empty when absent, and
// its replacement (persisted by SaveConfig). Scalars use Get/SetConfigValue.
std::string GetConfigJson(std::string_view key);
void SetConfigJson(const std::string& key, const std::string& json_text);

// Per-game settings (Settings > This Game). SetGame names the running game by
// its ROM path ("" for none, e.g. the library) and reads its overrides from
// sdmc:/tico/config/games/<core>/<game>.jsonc, if it has them; while it has
// them they are read first and take every change.
void SetGame(const std::string& rom_path);
bool HasGame();
bool GameSettingsActive();
// Gives the game its own file holding every current value, which then takes
// every change; Delete removes it, so the game follows the core's settings.
void SaveGameSettings();
void DeleteGameSettings();

// Hands every catalogued core option (its stored value, or its default) to
// `apply`, keyed by its libretro variable name. Options without the core's
// dolphin_ prefix are the overlay's own and are skipped.
using CoreOptionFn = std::function<void(const std::string& key, const std::string& value)>;
void ApplyToCore(const CoreOptionFn& apply);

std::string GetLoadedConfigPath();
std::size_t GetLoadedOptionCount();

// ---------------------------------------------------------------------------
// Option catalogue. The module's settings.json (tico/module) describes every
// setting; tico builds its settings screen from the copy in the installed
// module, and the overlay reads the same file from this NRO's romfs.

enum class OptionType {
    // bool_true_value / bool_false_value
    Toggle,
    // one of a fixed list of values
    Choice,
    // free text, edited with the system keyboard
    Text,
};

struct OptionChoice {
    // the value stored in the config file
    const char* value;
    // translation key and English text for the menu
    const char* label_key;
    const char* fallback;
};

struct OptionDef {
    const char* key;
    // translation key and English text for the menu
    const char* label_key;
    const char* fallback;
    OptionType type;
    const char* default_value;
    const OptionChoice* choices;
    std::size_t choice_count;
    // only read when a game starts
    bool needs_restart;
    // maximum length for Text options
    int max_length;
    // when set, the option is only listed while that option has this value
    const char* shown_when_key = nullptr;
    const char* shown_when_value = nullptr;
};

struct OptionCategory {
    const char* label_key;
    const char* fallback;
    const OptionDef* options;
    std::size_t option_count;
};

const std::vector<OptionCategory>& GetCategories();

// The catalogue entry for a key, or nullptr.
const OptionDef* FindOption(std::string_view key);

// Whether the menu lists the option, given the values of the options it depends on.
bool IsOptionShown(const OptionDef& option);

// The stored value of an option, or its default when the file does not set it.
std::string GetOptionValue(const OptionDef& option);

// The value the core reads: toggles spelled as settings.json's bool values, and
// a choice the file names but the list does not know replaced by a known one.
std::string GetCoreValue(const OptionDef& option);

// The text the menu shows for the option's current value (the translation key
// and English text of a choice, or the value itself for text).
struct OptionValueLabel {
    const char* label_key;
    std::string fallback;
};
OptionValueLabel GetOptionValueLabel(const OptionDef& option);

// Moves a Toggle or Choice option to its next (direction > 0) or previous
// value and saves the config. Toggles and choices wrap around.
void StepOption(const OptionDef& option, int direction);

// Stores a new value for any option and saves the config.
void SetOptionValue(const OptionDef& option, const std::string& value);

} // namespace SwitchFrontend::TicoConfig
