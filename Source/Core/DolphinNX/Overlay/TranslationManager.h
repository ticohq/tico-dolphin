// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>

namespace DolphinNX::OverlayTranslation
{

class TranslationManager
{
public:
  static TranslationManager& Instance();

  bool Init();
  std::string GetString(const std::string& key) const;

private:
  TranslationManager() = default;

  bool LoadLanguageFile(const std::string& filename);

  std::string m_current_language;
  std::unordered_map<std::string, std::string> m_translations;
};

std::string tr(const std::string& key);

}  // namespace DolphinNX::OverlayTranslation
