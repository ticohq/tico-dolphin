// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Riivolution.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <fmt/format.h>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Boot/Boot.h"
#include "Core/ConfigManager.h"
#include "DiscIO/RiivolutionParser.h"
#include "DiscIO/Volume.h"
#include "overlay/translation_manager.h"

namespace DolphinNX::Riivolution
{
namespace
{
namespace Riiv = DiscIO::Riivolution;

// as on a Wii's SD card: riivolution/*.xml, riivolution/config, the mods' files
constexpr const char* kRoot = "sdmc:/tico/mods/wii/";

// The patches for the game the menu was last opened in, and which option each
// of its rows is.
std::vector<Riiv::Disc> s_discs;
struct OptionRef
{
  size_t disc;
  size_t section;
  size_t option;
};
std::vector<OptionRef> s_options;
std::string s_game_id;

std::string TrOr(const char* key, const char* fallback)
{
  const std::string text = SwitchFrontend::OverlayTranslation::tr(key);
  return text == key ? fallback : text;
}

bool HasStandardGameID(std::string_view game_id)
{
  return game_id.size() == 4 || game_id.size() == 6;
}

std::string ConfigPath(std::string_view game_id)
{
  return fmt::format("{}riivolution/config/{}.xml", kRoot, game_id.substr(0, 4));
}

// Every patch for the game, with the choices saved for it.
std::vector<Riiv::Disc> LoadDiscs(const std::string& game_id, std::optional<u16> revision)
{
  std::vector<Riiv::Disc> discs;
  if (!HasStandardGameID(game_id))
    return discs;
  const std::optional<Riiv::Config> config = Riiv::ParseConfigFile(ConfigPath(game_id));
  for (const std::string& path : Common::DoFileSearch(std::string(kRoot) + "riivolution", ".xml"))
  {
    std::optional<Riiv::Disc> disc = Riiv::ParseFile(path);
    if (!disc || !disc->IsValidForGame(game_id, revision, std::nullopt))
      continue;
    if (config)
      Riiv::ApplyConfigDefaults(&*disc, *config);
    discs.push_back(std::move(*disc));
  }
  return discs;
}

// Riivolution's own config: one entry per option, by its id (or section + name)
void SaveChoices()
{
  Riiv::Config config;
  for (const Riiv::Disc& disc : s_discs)
  {
    for (const Riiv::Section& section : disc.m_sections)
    {
      for (const Riiv::Option& option : section.m_options)
      {
        std::string id = option.m_id.empty() ? section.m_name + option.m_name : option.m_id;
        config.m_options.push_back({std::move(id), option.m_selected_choice});
      }
    }
  }
  const std::string path = ConfigPath(s_game_id);
  File::CreateFullPath(path);
  if (!Riiv::WriteConfigFile(path, config))
    ERROR_LOG_FMT(BOOT, "Riivolution: could not write {}", path);
}

std::string ChoiceName(const Riiv::Option& option)
{
  if (option.m_selected_choice == 0 || option.m_selected_choice > option.m_choices.size())
    return TrOr("emulator_mods_disabled", "Disabled");
  return option.m_choices[option.m_selected_choice - 1].m_name;
}
}  // namespace

std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> List()
{
  const SConfig& config = SConfig::GetInstance();
  s_game_id = config.GetGameID();
  s_discs = LoadDiscs(s_game_id, config.GetRevision());
  s_options.clear();

  std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> rows;
  for (size_t d = 0; d < s_discs.size(); ++d)
  {
    const Riiv::Disc& disc = s_discs[d];
    for (size_t s = 0; s < disc.m_sections.size(); ++s)
    {
      const Riiv::Section& section = disc.m_sections[s];
      rows.push_back({section.m_name, {}, -1});
      for (size_t o = 0; o < section.m_options.size(); ++o)
      {
        const Riiv::Option& option = section.m_options[o];
        rows.push_back({option.m_name, ChoiceName(option), static_cast<int>(s_options.size())});
        s_options.push_back({d, s, o});
      }
    }
  }

  if (rows.empty())
  {
    // so the folder is there to copy mods into
    File::CreateDirs(std::string(kRoot) + "riivolution");
    rows.push_back({TrOr("emulator_mods_none", "No Riivolution mods for this game"), {}, -1});
    rows.push_back({TrOr("emulator_mods_where", "Copy them to sdmc:/tico/mods/wii"), {}, -1});
  }
  return rows;
}

bool Step(int source_index, int direction)
{
  if (source_index < 0 || source_index >= static_cast<int>(s_options.size()) || direction == 0)
    return false;
  const OptionRef& ref = s_options[static_cast<size_t>(source_index)];
  Riiv::Option& option = s_discs[ref.disc].m_sections[ref.section].m_options[ref.option];

  // Disabled (0), then each choice (1-based)
  const u32 count = static_cast<u32>(option.m_choices.size()) + 1;
  option.m_selected_choice = (option.m_selected_choice + (direction > 0 ? 1 : count - 1)) % count;
  SaveChoices();
  return true;
}

void AddPatches(BootParameters& boot)
{
  if (!std::holds_alternative<BootParameters::Disc>(boot.parameters))
    return;
  const DiscIO::VolumeDisc& volume = *std::get<BootParameters::Disc>(boot.parameters).volume;
  auto patches = Riiv::GenerateRiivolutionPatchesFromConfig(
      kRoot, volume.GetGameID(), volume.GetRevision(), volume.GetDiscNumber());
  if (patches.empty())
    return;
  NOTICE_LOG_FMT(BOOT, "Riivolution: applying {} patches", patches.size());
  AddRiivolutionPatches(&boot, std::move(patches));
}
}  // namespace DolphinNX::Riivolution
