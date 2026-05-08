// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace DolphinNX::TicoCore
{

void ReloadConfig();
std::string GetConfigValue(std::string_view key, std::string_view default_value = {});
void SetConfigValue(const std::string& key, const std::string& value);
bool SaveConfig();

void ApplyConfig(bool is_gamecube_disc);

std::string GetLoadedConfigPath();
std::size_t GetLoadedOptionCount();

}  // namespace DolphinNX::TicoCore
