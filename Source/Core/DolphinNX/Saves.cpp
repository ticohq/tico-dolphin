// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Saves.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

namespace DolphinNX::Saves
{
namespace
{
constexpr const char* kGameCubeSaves = "sdmc:/tico/saves/gc";
constexpr const char* kWiiSaves = "sdmc:/tico/saves/wii";
constexpr const char* kWiiImport = "sdmc:/tico/saves/wii/import";

// Where Dolphin kept the memory cards before (its default, under its user folder)
constexpr const char* kOldGameCubeSaves = "sdmc:/tico/system/gc/User/GC";
constexpr const char* kMigrationMarker = "sdmc:/tico/config/.migrations/dolphin_saves_layout";

// Moves every file of @p from into @p to, keeping any that is already there.
void MoveFiles(const std::string& from, const std::string& to)
{
  if (!File::IsDirectory(from))
    return;
  for (const File::FSTEntry& entry : File::ScanDirectoryTree(from, false).children)
  {
    if (entry.isDirectory)
      continue;
    const std::string dest = to + "/" + entry.virtualName;
    if (File::Exists(dest))
    {
      WARN_LOG_FMT(CORE, "Saves: {} is already in {}, left in {}", entry.virtualName, to, from);
      continue;
    }
    File::CreateDirs(to);
    if (File::Rename(entry.physicalName, dest))
      NOTICE_LOG_FMT(CORE, "Saves: moved {} to {}", entry.physicalName, dest);
  }
}

// A title's folder: its game ID (the low half of the title ID, as the Wii's SD
// card names it), or the hex title ID when that isn't letters and digits.
std::string FolderName(u64 title_id)
{
  std::string id;
  for (int shift = 24; shift >= 0; shift -= 8)
    id += static_cast<char>((title_id >> shift) & 0xFF);
  if (std::ranges::all_of(id, [](char c) { return std::isalnum(static_cast<unsigned char>(c)); }))
    return id;
  return fmt::format("{:016x}", title_id);
}

bool IsGameTitle(u64 title_id)
{
  // discs, channels (WiiWare, Virtual Console) and disc games' channels; the
  // Wii Menu and system channels keep their data in the NAND
  const u32 type = static_cast<u32>(title_id >> 32);
  return type == 0x00010000 || type == 0x00010001 || type == 0x00010004;
}

bool HasFiles(const std::string& dir)
{
  return File::IsDirectory(dir) && !File::ScanDirectoryTree(dir, false).children.empty();
}

std::string Timestamp()
{
  const std::time_t now = std::time(nullptr);
  char text[32] = {};
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", std::localtime(&now));
  return text;
}
}  // namespace

void MigrateGameCubeSaves()
{
  if (File::Exists(kMigrationMarker))
    return;

  // folder cards: <old>/<USA|EUR|JAP>/Card A -> saves/gc/<USA|EUR|JPN>, Card B
  // under saves/gc/Card B
  constexpr std::pair<const char*, const char*> kRegions[] = {
      {USA_DIR, USA_DIR}, {EUR_DIR, EUR_DIR}, {JAP_DIR, JPN_DIR}, {JPN_DIR, JPN_DIR}};
  for (const auto& [old_region, region] : kRegions)
  {
    const std::string old_dir = fmt::format("{}/{}", kOldGameCubeSaves, old_region);
    MoveFiles(old_dir + "/Card A", fmt::format("{}/{}", kGameCubeSaves, region));
    MoveFiles(old_dir + "/Card B", fmt::format("{}/Card B/{}", kGameCubeSaves, region));
  }

  // raw cards (MemoryCardA.USA.raw and the like) keep their names
  if (File::IsDirectory(kOldGameCubeSaves))
  {
    for (const File::FSTEntry& entry :
         File::ScanDirectoryTree(kOldGameCubeSaves, false).children)
    {
      const std::string& name = entry.virtualName;
      if (entry.isDirectory || !name.starts_with("MemoryCard") || !name.ends_with(".raw"))
        continue;
      const std::string dest = fmt::format("{}/{}", kGameCubeSaves, name);
      File::CreateDirs(kGameCubeSaves);
      if (!File::Exists(dest) && File::Rename(entry.physicalName, dest))
        NOTICE_LOG_FMT(CORE, "Saves: moved {} to {}", entry.physicalName, dest);
    }
  }

  File::CreateFullPath(kMigrationMarker);
  File::IOFile marker(kMigrationMarker, "wb");
}

void ApplyGameCubeCardPaths()
{
  // Dolphin adds the region: saves/gc/USA, saves/gc/MemoryCardA.USA.raw, ...
  Config::SetBase(Config::MAIN_GCI_FOLDER_A_PATH, std::string(kGameCubeSaves));
  Config::SetBase(Config::MAIN_GCI_FOLDER_B_PATH, fmt::format("{}/Card B", kGameCubeSaves));
  Config::SetBase(Config::MAIN_MEMCARD_A_PATH, fmt::format("{}/MemoryCardA.raw", kGameCubeSaves));
  Config::SetBase(Config::MAIN_MEMCARD_B_PATH, fmt::format("{}/MemoryCardB.raw", kGameCubeSaves));
}

std::optional<DiscIO::Riivolution::SavegameRedirect> WiiSaveRedirect()
{
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  if (!IsGameTitle(title_id))
    return std::nullopt;
  // clone: on the game's first start here, its save comes along from the NAND
  return DiscIO::Riivolution::SavegameRedirect{
      fmt::format("{}/{}", kWiiSaves, FolderName(title_id)), true};
}

void ImportWiiSaves()
{
  auto& system = Core::System::GetInstance();
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  if (!ios || !IsGameTitle(title_id) || !File::IsDirectory(kWiiImport))
    return;

  // any .bin under import/ (a Wii's private/wii/title/<ID>/data.bin copied over
  // as it is works too) whose save is this game's
  std::vector<std::string> candidates;
  const auto collect = [&](const auto& self, const File::FSTEntry& dir) -> void {
    for (const File::FSTEntry& entry : dir.children)
    {
      if (entry.isDirectory)
      {
        self(self, entry);
        continue;
      }
      std::string lower = entry.virtualName;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
      if (lower.ends_with(".bin"))
        candidates.push_back(entry.physicalName);
    }
  };
  collect(collect, File::ScanDirectoryTree(kWiiImport, true));

  for (const std::string& path : candidates)
  {
    if (WiiSave::ReadDataBinTitleID(&ios->GetIOSC(), path) != title_id)
      continue;

    // the save it replaces stays beside it
    const std::string save_dir = fmt::format("{}/{}", kWiiSaves, FolderName(title_id));
    if (HasFiles(save_dir))
    {
      const std::string backup = fmt::format("{}.backup-{}", save_dir, Timestamp());
      if (!File::Copy(save_dir, backup))
      {
        ERROR_LOG_FMT(CORE, "Saves: could not back up {}, {} not imported", save_dir, path);
        Core::DisplayMessage("Could not back up this game's save, so data.bin was not imported",
                             6000);
        return;
      }
    }

    const auto data_bin = WiiSave::MakeDataBinStorage(&ios->GetIOSC(), path, "rb");
    const auto nand = WiiSave::MakeNandStorage(ios->GetFS().get(), title_id);
    if (WiiSave::Copy(data_bin.get(), nand.get()) != WiiSave::CopyResult::Success)
    {
      ERROR_LOG_FMT(CORE, "Saves: importing {} failed", path);
      Core::DisplayMessage("Could not import the save from data.bin", 6000);
      return;
    }

    // so it is not imported again
    File::Rename(path, path + ".imported");
    NOTICE_LOG_FMT(CORE, "Saves: imported {}", path);
    Core::DisplayMessage("Imported the save from data.bin", 5000);
    return;
  }
}
}  // namespace DolphinNX::Saves
