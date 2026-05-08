// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Overlay/TranslationManager.h"

#include <array>
#include <string_view>
#include <utility>

#include <picojson.h>

#include "Common/FileUtil.h"

namespace DolphinNX::OverlayTranslation
{
namespace
{

constexpr std::array<const char*, 5> kGeneralConfigPaths = {{
    "sdmc:/tiicu/config/general.jsonc",
    "sdmc:/tico/config/general.jsonc",
    "tico/config/general.jsonc",
    "assets/config/general.jsonc",
    "../assets/config/general.jsonc",
}};

std::string StripJsonComments(std::string_view input)
{
  std::string output;
  output.reserve(input.size());

  bool in_string = false;
  bool escaped = false;

  for (size_t i = 0; i < input.size(); ++i)
  {
    const char c = input[i];

    if (in_string)
    {
      output.push_back(c);
      if (escaped)
      {
        escaped = false;
      }
      else if (c == '\\')
      {
        escaped = true;
      }
      else if (c == '"')
      {
        in_string = false;
      }
      continue;
    }

    if (c == '"')
    {
      in_string = true;
      output.push_back(c);
      continue;
    }

    if (c == '/' && i + 1 < input.size())
    {
      if (input[i + 1] == '/')
      {
        i += 2;
        while (i < input.size() && input[i] != '\n')
          ++i;
        if (i < input.size())
          output.push_back('\n');
        continue;
      }

      if (input[i + 1] == '*')
      {
        i += 2;
        while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/'))
          ++i;
        if (i + 1 < input.size())
          ++i;
        continue;
      }
    }

    output.push_back(c);
  }

  return output;
}

std::string LoadConfiguredLanguage()
{
  for (const char* path : kGeneralConfigPaths)
  {
    std::string content;
    if (!File::ReadFileToString(path, content))
      continue;

    const std::string stripped = StripJsonComments(content);
    picojson::value root;
    const std::string error = picojson::parse(root, stripped);
    if (!error.empty() || !root.is<picojson::object>())
      continue;

    const auto& object = root.get<picojson::object>();
    const auto it = object.find("language");
    if (it != object.end() && it->second.is<std::string>())
      return it->second.get<std::string>();
  }

  return "English";
}

std::string_view GetLanguageFilename(std::string_view language)
{
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
  if (language == "Chinese")
    return "zh.json";
  return "en.json";
}

}  // namespace

TranslationManager& TranslationManager::Instance()
{
  static TranslationManager instance;
  return instance;
}

bool TranslationManager::Init()
{
  const std::string language = LoadConfiguredLanguage();
  if (m_current_language == language && !m_translations.empty())
    return true;

  m_current_language = language;
  m_translations.clear();

  const std::string filename(GetLanguageFilename(language));
  if (LoadLanguageFile(filename))
    return true;

  if (filename != "en.json")
    return LoadLanguageFile("en.json");

  return false;
}

bool TranslationManager::LoadLanguageFile(const std::string& filename)
{
  std::string content;
  if (!File::ReadFileToString("romfs:/lang/" + filename, content))
    return false;

  picojson::value root;
  const std::string error = picojson::parse(root, content);
  if (!error.empty() || !root.is<picojson::object>())
    return false;

  std::unordered_map<std::string, std::string> translations;
  for (const auto& [key, value] : root.get<picojson::object>())
  {
    if (value.is<std::string>())
      translations.emplace(key, value.get<std::string>());
  }

  m_translations = std::move(translations);
  return true;
}

std::string TranslationManager::GetString(const std::string& key) const
{
  const auto it = m_translations.find(key);
  if (it != m_translations.end())
    return it->second;
  return key;
}

std::string tr(const std::string& key)
{
  return TranslationManager::Instance().GetString(key);
}

}  // namespace DolphinNX::OverlayTranslation
