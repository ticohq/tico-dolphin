// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Cheats.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Core/ActionReplay.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PatchEngine.h"
#include "Core/System.h"
#include "overlay/translation_manager.h"

namespace DolphinNX::Cheats
{
namespace
{
using SwitchFrontend::OverlayUI::CheatMenuEntry;
using SwitchFrontend::OverlayTranslation::tr;

struct GameCheats
{
  std::vector<Gecko::GeckoCode> gecko;
  std::vector<ActionReplay::ARCode> action_replay;
  std::vector<PatchEngine::Patch> patches;
};

std::mutex s_mutex;
GameCheats s_cheats;
std::atomic_bool s_changed = false;

std::string GetLocalIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}

GameCheats Load()
{
  GameCheats cheats;
  const SConfig& config = SConfig::GetInstance();
  const std::string game_id = config.GetGameID();
  if (game_id.empty())
    return cheats;

  Common::IniFile local_ini;
  local_ini.Load(GetLocalIniPath(game_id));
  const Common::IniFile global_ini = SConfig::LoadDefaultGameIni(game_id, config.GetRevision());

  cheats.gecko = Gecko::LoadCodes(global_ini, local_ini);
  cheats.action_replay = ActionReplay::LoadCodes(global_ini, local_ini);
  PatchEngine::LoadPatchSection("OnFrame", &cheats.patches, global_ini, local_ini);
  return cheats;
}

void Save(const GameCheats& cheats)
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (game_id.empty())
    return;

  const std::string path = GetLocalIniPath(game_id);
  Common::IniFile local_ini;
  local_ini.Load(path);
  Gecko::SaveCodes(local_ini, cheats.gecko);
  ActionReplay::SaveCodes(&local_ini, cheats.action_replay);
  PatchEngine::SavePatchSection(&local_ini, cheats.patches);
  if (!local_ini.Save(path))
    ERROR_LOG_FMT(COMMON, "Could not save cheats to {}", path);
}

std::string NameOf(const std::string& name)
{
  return name.empty() ? tr("emulator_cheat_unnamed") : name;
}
}  // namespace

std::vector<CheatMenuEntry> List()
{
  std::vector<CheatMenuEntry> entries;
  if (!Config::AreCheatsEnabled())
  {
    entries.push_back({.name = tr("emulator_cheats_disabled"), .toggleable = false});
    return entries;
  }

  std::lock_guard lock(s_mutex);
  s_cheats = Load();
  // one index over all three lists, in this order
  int index = 0;
  for (const auto& code : s_cheats.gecko)
    entries.push_back({.name = NameOf(code.name), .enabled = code.enabled, .source_index = index++});
  for (const auto& code : s_cheats.action_replay)
    entries.push_back({.name = NameOf(code.name), .enabled = code.enabled, .source_index = index++});
  for (const auto& patch : s_cheats.patches)
    entries.push_back({.name = NameOf(patch.name), .enabled = patch.enabled, .source_index = index++});
  entries.push_back({.name = tr("emulator_cheats_download"), .is_add_row = true});
  return entries;
}

bool Toggle(int source_index)
{
  std::lock_guard lock(s_mutex);
  auto toggle = [&](auto& codes) {
    if (source_index < 0 || source_index >= static_cast<int>(codes.size()))
    {
      source_index -= static_cast<int>(codes.size());
      return false;
    }
    codes[static_cast<std::size_t>(source_index)].enabled ^= true;
    return true;
  };
  if (!toggle(s_cheats.gecko) && !toggle(s_cheats.action_replay) && !toggle(s_cheats.patches))
    return false;

  Save(s_cheats);
  s_changed.store(true);
  return true;
}

void ApplyIfChanged()
{
  if (!s_changed.exchange(false))
    return;
  auto& system = Core::System::GetInstance();
  Core::RunOnCPUThread(system, [&system] { PatchEngine::Reload(system); });
  INFO_LOG_FMT(COMMON, "Cheats reloaded into the running game");
}

std::string DownloadGeckoCodes()
{
  const std::string gametdb_id = SConfig::GetInstance().GetGameTDBID();
  if (gametdb_id.empty())
    return tr("emulator_cheats_download_failed");

  const auto downloaded = Gecko::DownloadCodes(gametdb_id);
  if (!downloaded)
  {
    WARN_LOG_FMT(COMMON, "Gecko code download for {} failed (HTTP {})", gametdb_id,
                 downloaded.error());
    return tr("emulator_cheats_download_failed");
  }
  if (downloaded->empty())
    return tr("emulator_cheats_download_none");

  std::lock_guard lock(s_mutex);
  s_cheats = Load();
  std::size_t added = 0;
  for (const Gecko::GeckoCode& code : *downloaded)
  {
    if (std::ranges::find(s_cheats.gecko, code) != s_cheats.gecko.end())
      continue;
    s_cheats.gecko.push_back(code);
    ++added;
  }
  if (added != 0)
    Save(s_cheats);

  const std::string format = tr("emulator_cheats_downloaded");
  char text[160];
  std::snprintf(text, sizeof(text), format.c_str(), static_cast<int>(added));
  return text;
}
}  // namespace DolphinNX::Cheats
