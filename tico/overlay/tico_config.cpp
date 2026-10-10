// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/tico_config.h"
#include "../TicoSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include <json.hpp>

#include "TicoLogger.h"

namespace SwitchFrontend::TicoConfig {
namespace {

using OptionMap = std::map<std::string, std::string, std::less<>>;

constexpr std::array<const char*, 3> kConfigPaths = {{
    "sdmc:/tico/config/cores/dolphin.jsonc",
    "sdmc:/tico/config/cores/dolphin.json",
    "romfs:/config/dolphin.jsonc",
}};

constexpr const char* kDefaultWritableConfigPath = "sdmc:/tico/config/cores/dolphin.jsonc";

void EnsureWritableConfigDirectory() {
    mkdir("sdmc:/tico", 0777);
    mkdir("sdmc:/tico/config", 0777);
    mkdir("sdmc:/tico/config/cores", 0777);
}

// Strips // line and /* */ block comments so a .jsonc file parses as plain JSON.
std::string StripJsonComments(std::string_view input) {
    std::string output;
    output.reserve(input.size());

    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (in_string) {
            output.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            output.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < input.size()) {
            if (input[i + 1] == '/') {
                i += 2;
                while (i < input.size() && input[i] != '\n') {
                    ++i;
                }
                if (i < input.size()) {
                    output.push_back('\n');
                }
                continue;
            }
            if (input[i + 1] == '*') {
                i += 2;
                while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) {
                    ++i;
                }
                if (i + 1 < input.size()) {
                    ++i;
                }
                continue;
            }
        }
        output.push_back(c);
    }
    return output;
}

bool ReadWholeFile(const char* path, std::string& out) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(fp);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    out.resize(read);
    return true;
}

// Converts a JSON scalar into the canonical string we store internally.
std::string JsonScalarToString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value.get<double>());
        return text;
    }
    return {};
}

std::string LowerCopy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<bool> ParseBool(std::string_view value) {
    const std::string lower = LowerCopy(value);
    if (lower == "true" || lower == "1" || lower == "on" || lower == "yes" ||
        lower == "enabled") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "off" || lower == "no" ||
        lower == "disabled") {
        return false;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Option catalogue, read from the module's settings.json packed in this NRO's
// romfs. tico reads its own copy from the installed module, so both list the
// same options. Option and tab labels are translation keys (lang/*.json);
// choice labels are English, translated through settings_dolphin_value_<slug>
// keys when the language files have one.

constexpr const char* kSettingsPath = "romfs:/module/settings.json";
// the libretro variables the core reads; anything else is the overlay's own
constexpr std::string_view kCorePrefix = "dolphin_";

// How settings.json spells booleans; tico's own default when it does not say.
std::string s_bool_true = "enabled";
std::string s_bool_false = "disabled";

// The translation key of a choice label: settings_dolphin_value_ and the label
// in lower case with runs of other characters turned into one underscore.
std::string ValueKey(std::string_view label) {
    std::string key = "settings_dolphin_value_";
    bool separator = false;
    for (const char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (separator && key.back() != '_') {
                key.push_back('_');
            }
            separator = false;
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            separator = true;
        }
    }
    return key;
}

struct Catalogue {
    // the catalogue's strings and arrays never move once built
    std::deque<std::string> strings;
    std::deque<std::vector<OptionChoice>> choice_lists;
    std::deque<std::vector<OptionDef>> option_lists;
    std::vector<OptionCategory> categories;

    const char* Keep(std::string text) {
        strings.push_back(std::move(text));
        return strings.back().c_str();
    }
};

bool ParseOption(Catalogue& catalogue, const nlohmann::json& source, OptionDef& option) {
    const std::string type = source.value("type", "");
    if (!source.contains("key") || !source["key"].is_string()) {
        return false;
    }
    if (type == "bool") {
        option.type = OptionType::Toggle;
    } else if (type == "enum") {
        option.type = OptionType::Choice;
    } else if (type == "string") {
        option.type = OptionType::Text;
    } else {
        // actions and anything newer are tico's own
        return false;
    }
    // The controller mapping editor is tico's own: it edits the profiles the
    // core reads at launch.
    if (source["key"].get<std::string>().rfind("dolphin_mapping_", 0) == 0) {
        return false;
    }
    option.key = catalogue.Keep(source["key"].get<std::string>());
    option.fallback = catalogue.Keep(source.value("label", source["key"].get<std::string>()));
    option.label_key = option.fallback;
    option.default_value =
        catalogue.Keep(source.contains("default") ? JsonScalarToString(source["default"]) : "");
    option.needs_restart = source.value("restart", false);
    option.max_length = source.value("max_length", 0);
    option.choices = nullptr;
    option.choice_count = 0;
    // a text option may list known values too: Left/Right pick one, A types another
    if (option.type == OptionType::Choice ||
        (option.type == OptionType::Text && source.contains("choices"))) {
        std::vector<OptionChoice> choices;
        for (const nlohmann::json& choice : source.value("choices", nlohmann::json::array())) {
            const std::string value =
                choice.contains("value") ? JsonScalarToString(choice["value"]) : "";
            const std::string label = choice.value("label", value);
            choices.push_back({catalogue.Keep(value), catalogue.Keep(ValueKey(label)),
                               catalogue.Keep(label)});
        }
        if (choices.empty() && option.type == OptionType::Choice) {
            return false;
        }
        catalogue.choice_lists.push_back(std::move(choices));
        option.choices = catalogue.choice_lists.back().data();
        option.choice_count = catalogue.choice_lists.back().size();
    }
    if (source.contains("depends_on") && source["depends_on"].is_object()) {
        const nlohmann::json& depends_on = source["depends_on"];
        option.shown_when_key = catalogue.Keep(depends_on.value("key", ""));
        option.shown_when_value = catalogue.Keep(
            depends_on.contains("value") ? JsonScalarToString(depends_on["value"]) : "");
    }
    return true;
}

Catalogue LoadCatalogue() {
    Catalogue catalogue;
    std::string content;
    nlohmann::json root;
    if (ReadWholeFile(kSettingsPath, content)) {
        root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
    }
    if (!root.is_object()) {
        LOG_WARN("OVERLAY", "could not read the settings definition at %s", kSettingsPath);
        root = nlohmann::json::object();
    }
    s_bool_true = root.value("bool_true_value", s_bool_true);
    s_bool_false = root.value("bool_false_value", s_bool_false);

    for (const nlohmann::json& tab : root.value("tabs", nlohmann::json::array())) {
        std::vector<OptionDef> options;
        for (const nlohmann::json& section : tab.value("sections", nlohmann::json::array())) {
            for (const nlohmann::json& source : section.value("options", nlohmann::json::array())) {
                OptionDef option{};
                if (!ParseOption(catalogue, source, option)) {
                    continue;
                }
                options.push_back(option);
            }
        }
        if (options.empty()) {
            continue;
        }
        catalogue.option_lists.push_back(std::move(options));
        const std::vector<OptionDef>& stored = catalogue.option_lists.back();
        const char* name = catalogue.Keep(tab.value("name", "Settings"));
        catalogue.categories.push_back({name, name, stored.data(), stored.size()});
    }
    // the menu always has a category to show, even without a definition
    if (catalogue.categories.empty()) {
        catalogue.categories.push_back({nullptr, "Settings", nullptr, 0});
    }
    return catalogue;
}

Catalogue& GetCatalogue() {
    static Catalogue catalogue = LoadCatalogue();
    return catalogue;
}

// Index of the choice whose stored value matches, or 0 when none does.
// The choice that is exactly @p value, or -1 (a text option's typed value).
int FindExactChoice(const OptionDef& option, std::string_view value) {
    for (std::size_t i = 0; i < option.choice_count; ++i) {
        if (value == option.choices[i].value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::size_t FindChoice(const OptionDef& option, std::string_view value) {
    const std::string lower = LowerCopy(value);
    for (std::size_t i = 0; i < option.choice_count; ++i) {
        if (LowerCopy(option.choices[i].value) == lower) {
            return i;
        }
    }
    return 0;
}

class Manager {
public:
    void ReloadConfig() {
        options.clear();
        changed_json.clear();
        original = nlohmann::json::object();
        loaded_path.clear();

        for (const char* path : kConfigPaths) {
            std::string content;
            if (!ReadWholeFile(path, content)) {
                continue;
            }
            const std::string stripped = StripJsonComments(content);
            nlohmann::json root = nlohmann::json::parse(stripped, nullptr, false);
            if (root.is_discarded() || !root.is_object()) {
                LOG_WARN("OVERLAY", "tico config at %s is not a JSON object", path);
                continue;
            }
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it.value().is_object() || it.value().is_array()) {
                    continue;
                }
                options[it.key()] = JsonScalarToString(it.value());
            }
            original = std::move(root);
            loaded_path = path;
            LOG_INFO("OVERLAY", "tico config loaded from %s (%zu options)", path, options.size());
            return;
        }
        LOG_INFO("OVERLAY", "no tico config found; using defaults");
    }

    std::string GetConfigValue(std::string_view key, std::string_view default_value) const {
        if (UsesGameLayer(key)) {
            const auto game_it = game_root.find(std::string(key));
            if (game_it != game_root.end() && !game_it->is_object() && !game_it->is_array()) {
                return JsonScalarToString(*game_it);
            }
        }
        const auto it = options.find(key);
        if (it != options.end()) {
            return it->second;
        }
        return std::string(default_value);
    }

    void SetConfigValue(const std::string& key, const std::string& value) {
        if (UsesGameLayer(key)) {
            game_root[key] = value;
            return;
        }
        options[key] = value;
        changed[key] = value;
    }

    std::string GetConfigJson(std::string_view key) const {
        if (UsesGameLayer(key)) {
            const auto game_it = game_root.find(std::string(key));
            if (game_it != game_root.end() && (game_it->is_object() || game_it->is_array())) {
                return game_it->dump();
            }
        }
        const auto changed_it = changed_json.find(key);
        if (changed_it != changed_json.end()) {
            return changed_it->second.dump();
        }
        const auto it = original.find(std::string(key));
        return it != original.end() && (it->is_object() || it->is_array()) ? it->dump()
                                                                          : std::string();
    }

    void SetConfigJson(const std::string& key, const std::string& json_text) {
        nlohmann::json value = nlohmann::json::parse(json_text, nullptr, false);
        if (value.is_discarded()) {
            return;
        }
        if (UsesGameLayer(key)) {
            game_root[key] = std::move(value);
            return;
        }
        changed_json[key] = std::move(value);
    }

    // Writes back what was read, with this session's changes as strings, the
    // way tico stores them (bools as settings.json's bool_true_value and
    // bool_false_value, tico's enabled/disabled when it names none). Keys tico
    // wrote keep their own JSON types.
    bool SaveConfig() {
        nlohmann::json root = original.is_object() ? original : nlohmann::json::object();
        for (const auto& [key, value] : changed) {
            root[key] = value;
        }
        for (const auto& [key, value] : changed_json) {
            root[key] = value;
        }
        const std::string serialized = root.dump(2);

        EnsureWritableConfigDirectory();

        const char* target = kDefaultWritableConfigPath;
        std::FILE* fp = std::fopen(target, "wb");
        if (!fp) {
            LOG_WARN("OVERLAY", "failed to open tico config for write: %s", target);
            return false;
        }
        const std::size_t written = std::fwrite(serialized.data(), 1, serialized.size(), fp);
        std::fclose(fp);
        if (written != serialized.size()) {
            LOG_WARN("OVERLAY", "failed to write full tico config: %s", target);
            return false;
        }
        loaded_path = target;
        return SaveGameFile();
    }

    // --- Per-game layer: sdmc:/tico/config/games/<core>/<game>.jsonc holds
    // what one game sets differently. While it exists it is read first and
    // takes every change (but the library's folders, which stay the core's).

    void SetGame(const std::string& rom_path) {
        game_file.clear();
        game_root = nlohmann::json::object();
        game_active = false;
        std::string name = rom_path;
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
            name = name.substr(1, name.size() - 2);
        }
        const std::size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        const std::size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            name = name.substr(0, dot);
        }
        if (name.empty()) {
            return;
        }
        game_file = GameDir() + name + ".jsonc";
        std::string content;
        if (ReadWholeFile(game_file.c_str(), content)) {
            nlohmann::json root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
            if (!root.is_discarded() && root.is_object()) {
                game_root = std::move(root);
                game_active = true;
                LOG_INFO("OVERLAY", "per-game settings from %s", game_file.c_str());
            }
        }
    }

    bool HasGame() const {
        return !game_file.empty();
    }

    bool GameActive() const {
        return game_active;
    }

    // The game gets its own file with every current value (the core's file,
    // this session's changes, settings.json's defaults for the rest); from
    // then on changes go to it.
    void SaveGameSettings() {
        if (game_file.empty()) {
            return;
        }
        nlohmann::json root = original.is_object() ? original : nlohmann::json::object();
        for (const auto& [key, value] : changed) {
            root[key] = value;
        }
        for (const auto& [key, value] : changed_json) {
            root[key] = value;
        }
        for (const OptionCategory& category : GetCatalogue().categories) {
            for (std::size_t i = 0; i < category.option_count; ++i) {
                const OptionDef& option = category.options[i];
                if (!root.contains(option.key)) {
                    root[option.key] = GetConfigValue(option.key, option.default_value);
                }
            }
        }
        root.erase("tico_rom_folders");
        game_root = std::move(root);
        game_active = true;
        SaveGameFile();
    }

    // Back to the core's settings: the game's file is removed.
    void DeleteGameSettings() {
        if (game_file.empty()) {
            return;
        }
        game_active = false;
        game_root = nlohmann::json::object();
        std::remove(game_file.c_str());
    }

private:
    bool UsesGameLayer(std::string_view key) const {
        return game_active && key != "tico_rom_folders";
    }

    // sdmc:/tico/config/games/<core>/, the core named as its own config file is
    static std::string GameDir() {
        std::string core = kDefaultWritableConfigPath;
        core = core.substr(core.find_last_of('/') + 1);
        core = core.substr(0, core.find_last_of('.'));
        return "sdmc:/tico/config/games/" + core + "/";
    }

    bool SaveGameFile() {
        if (!game_active || game_file.empty()) {
            return true;
        }
        mkdir("sdmc:/tico/config/games", 0777);
        mkdir(GameDir().c_str(), 0777);
        const std::string serialized = game_root.dump(2);
        std::FILE* fp = std::fopen(game_file.c_str(), "wb");
        if (!fp) {
            LOG_WARN("OVERLAY", "failed to open per-game config for write: %s", game_file.c_str());
            return false;
        }
        const bool ok = std::fwrite(serialized.data(), 1, serialized.size(), fp) == serialized.size();
        std::fclose(fp);
        return ok;
    }

public:

    std::string GetOptionValue(const OptionDef& option) const {
        return GetConfigValue(option.key, option.default_value);
    }

    bool GetBool(const OptionDef& option) const {
        if (const auto b = ParseBool(GetOptionValue(option))) {
            return *b;
        }
        return ParseBool(option.default_value).value_or(false);
    }

    // For Choice options: the position of the stored value in the choice list.
    int GetChoiceIndex(const OptionDef& option) const {
        return static_cast<int>(FindChoice(option, GetOptionValue(option)));
    }

    const std::string& GetLoadedPath() const {
        return loaded_path;
    }

    std::size_t GetOptionCount() const {
        return options.size();
    }

private:
    OptionMap options;
    OptionMap changed;
    std::map<std::string, nlohmann::json, std::less<>> changed_json;
    nlohmann::json original = nlohmann::json::object();
    std::string loaded_path;
    std::string game_file;
    nlohmann::json game_root = nlohmann::json::object();
    bool game_active = false;
};

Manager& GetManager() {
    static Manager manager;
    return manager;
}

// The value the core reads: toggles as settings.json's bool values, and a
// choice that the file names but the list does not know replaced by a known one.
std::string CoreValue(const OptionDef& option) {
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        return config.GetBool(option) ? s_bool_true : s_bool_false;
    case OptionType::Choice:
        if (option.choice_count == 0) {
            return config.GetOptionValue(option);
        }
        return option.choices[config.GetChoiceIndex(option)].value;
    case OptionType::Text:
    default:
        return config.GetOptionValue(option);
    }
}

} // namespace

void ReloadConfig() {
    GetManager().ReloadConfig();
}

int BorderTint() {
    std::string content;
    if (!tico::ReadSettings("display", content)) {
        return 0;
    }
    const nlohmann::json root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
    if (root.is_discarded() || !root.contains("border_tint") ||
        !root["border_tint"].is_number_integer()) {
        return 0;
    }
    return root["border_tint"].get<int>();
}

bool DarkMode() {
    std::string content;
    if (!tico::ReadSettings("display", content)) {
        return false;
    }
    const nlohmann::json root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
    return !root.is_discarded() && root.is_object() && root.value("dark_mode", false);
}

std::string ResumeOnLaunch() {
    std::string content;
    if (!tico::ReadSettings("general", content)) {
        return "ask";
    }
    const nlohmann::json root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
    const std::string mode = !root.is_discarded() && root.is_object()
                                 ? root.value("resume_on_launch", std::string("ask"))
                                 : std::string("ask");
    return (mode == "always" || mode == "never") ? mode : "ask";
}

std::string GetConfigValue(std::string_view key, std::string_view default_value) {
    return GetManager().GetConfigValue(key, default_value);
}

void SetConfigValue(const std::string& key, const std::string& value) {
    GetManager().SetConfigValue(key, value);
}

bool SaveConfig() {
    return GetManager().SaveConfig();
}

std::string GetConfigJson(std::string_view key) {
    return GetManager().GetConfigJson(key);
}

void SetConfigJson(const std::string& key, const std::string& json_text) {
    GetManager().SetConfigJson(key, json_text);
}

void SetGame(const std::string& rom_path) {
    GetManager().SetGame(rom_path);
}

bool HasGame() {
    return GetManager().HasGame();
}

bool GameSettingsActive() {
    return GetManager().GameActive();
}

void SaveGameSettings() {
    GetManager().SaveGameSettings();
}

void DeleteGameSettings() {
    GetManager().DeleteGameSettings();
}

void ApplyToCore(const CoreOptionFn& apply) {
    for (const OptionCategory& category : GetCategories()) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            const OptionDef& option = category.options[i];
            if (std::string_view(option.key).substr(0, kCorePrefix.size()) == kCorePrefix) {
                apply(option.key, CoreValue(option));
            }
        }
    }
}

const std::vector<OptionCategory>& GetCategories() {
    return GetCatalogue().categories;
}

const OptionDef* FindOption(std::string_view key) {
    for (const OptionCategory& category : GetCategories()) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            if (key == category.options[i].key) {
                return &category.options[i];
            }
        }
    }
    return nullptr;
}

std::string GetOptionValue(const OptionDef& option) {
    return GetManager().GetOptionValue(option);
}

std::string GetCoreValue(const OptionDef& option) {
    return CoreValue(option);
}

bool IsOptionShown(const OptionDef& option) {
    // shown when the option it depends on has the value, and that one is
    // shown too (a chain: Renderer = Vulkan > LSFG on > its options), as in
    // tico's settings; the depth limit stops a loop in a broken settings.json
    const OptionDef* current = &option;
    for (int depth = 0; depth < 8 && current->shown_when_key; ++depth) {
        const OptionDef* controller = FindOption(current->shown_when_key);
        if (!controller || CoreValue(*controller) != current->shown_when_value) {
            return false;
        }
        current = controller;
    }
    return !current->shown_when_key;
}

OptionValueLabel GetOptionValueLabel(const OptionDef& option) {
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        return config.GetBool(option) ? OptionValueLabel{"emulator_on", "On"}
                                      : OptionValueLabel{"emulator_off", "Off"};
    case OptionType::Choice: {
        const OptionChoice& choice = option.choices[config.GetChoiceIndex(option)];
        return {choice.label_key, choice.fallback};
    }
    case OptionType::Text:
    default: {
        const std::string value = config.GetOptionValue(option);
        const int choice = FindExactChoice(option, value);
        if (choice >= 0) {
            return {option.choices[choice].label_key, option.choices[choice].fallback};
        }
        return {nullptr, value};
    }
    }
}

void SetOptionValue(const OptionDef& option, const std::string& value) {
    GetManager().SetConfigValue(option.key, value);
    GetManager().SaveConfig();
}

void StepOption(const OptionDef& option, int direction) {
    if (direction == 0) {
        return;
    }
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        SetOptionValue(option, config.GetBool(option) ? s_bool_false : s_bool_true);
        break;
    case OptionType::Choice: {
        const int count = static_cast<int>(option.choice_count);
        if (count == 0) {
            break;
        }
        const int index = (config.GetChoiceIndex(option) + (direction > 0 ? 1 : count - 1)) % count;
        SetOptionValue(option, option.choices[index].value);
        break;
    }
    case OptionType::Text: {
        // known values, from a typed one to the first (or last)
        const int count = static_cast<int>(option.choice_count);
        if (count == 0) {
            break;
        }
        const int current = FindExactChoice(option, config.GetOptionValue(option));
        const int index = current < 0 ? (direction > 0 ? 0 : count - 1)
                                      : (current + (direction > 0 ? 1 : count - 1)) % count;
        SetOptionValue(option, option.choices[index].value);
        break;
    }
    default:
        break;
    }
}

std::string GetLoadedConfigPath() {
    return GetManager().GetLoadedPath();
}

std::size_t GetLoadedOptionCount() {
    return GetManager().GetOptionCount();
}

} // namespace SwitchFrontend::TicoConfig
