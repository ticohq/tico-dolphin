// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/translation_manager.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include <json.hpp>

namespace SwitchFrontend::OverlayTranslation {
namespace {

constexpr std::array<const char*, 3> kGeneralConfigPaths = {{
    "sdmc:/tico/config/general.jsonc",
    "sdmc:/tico/config/general.json",
    "romfs:/config/general.jsonc",
}};

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

std::string LoadConfiguredLanguage() {
    for (const char* path : kGeneralConfigPaths) {
        std::string content;
        if (!ReadWholeFile(path, content)) {
            continue;
        }
        const std::string stripped = StripJsonComments(content);
        nlohmann::json root = nlohmann::json::parse(stripped, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            continue;
        }
        const auto it = root.find("language");
        if (it != root.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return "English";
}

std::string_view GetLanguageFilename(std::string_view language) {
    if (language == "English")
        return "en.json";
    if (language == "Portuguese" || language == "Português")
        return "pt.json";
    if (language == "Espanol" || language == "Español" || language == "Spanish")
        return "es.json";
    if (language == "Japanese")
        return "ja.json";
    if (language == "French")
        return "fr.json";
    if (language == "German" || language == "Deutsch" || language == "de")
        return "de.json";
    if (language == "Russian" || language == "Russkiy" || language == "Русский" ||
        language == "ru")
        return "ru.json";
    if (language == "Chinese")
        return "zh.json";
    return "en.json";
}

} // namespace

TranslationManager& TranslationManager::Instance() {
    static TranslationManager instance;
    return instance;
}

bool TranslationManager::Init() {
    const std::string language = LoadConfiguredLanguage();
    if (m_current_language == language && !m_translations.empty()) {
        return true;
    }

    m_current_language = language;
    m_translations.clear();

    // English first, so a string missing from a translation still reads as text
    const bool english = LoadLanguageFile("en.json");
    const std::string filename(GetLanguageFilename(language));
    if (filename != "en.json" && LoadLanguageFile(filename)) {
        return true;
    }
    return english;
}

bool TranslationManager::LoadLanguageFile(const std::string& filename) {
    std::string content;
    if (!ReadWholeFile(("romfs:/lang/" + filename).c_str(), content)) {
        return false;
    }
    nlohmann::json root = nlohmann::json::parse(content, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.value().is_string()) {
            m_translations.insert_or_assign(it.key(), it.value().get<std::string>());
        }
    }
    return true;
}

std::string TranslationManager::GetString(const std::string& key) const {
    const auto it = m_translations.find(key);
    if (it != m_translations.end()) {
        return it->second;
    }
    return key;
}

std::string tr(const std::string& key) {
    TranslationManager& manager = TranslationManager::Instance();
    // a toast can come before the menu has ever opened (and loaded them)
    if (!manager.IsLoaded()) {
        manager.Init();
    }
    return manager.GetString(key);
}

} // namespace SwitchFrontend::OverlayTranslation
