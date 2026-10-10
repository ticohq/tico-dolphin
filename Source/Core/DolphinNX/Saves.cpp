// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Saves.h"

#include "TicoSession.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

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
// The current user's saves (TicoSession.h): tico's folder, or theirs in it.
std::string SavesRoot()
{
  return tico::UserContentRoot("sdmc:/tico/saves/", true);
}
std::string GameCubeSaves()
{
  return SavesRoot() + "gc";
}
std::string WiiSaves()
{
  return SavesRoot() + "wii";
}
std::string WiiImport()
{
  return SavesRoot() + "wii/import";
}

// Where Dolphin kept saves before (its defaults, under its user folder)
constexpr const char* kOldGameCubeSaves = "sdmc:/tico/system/gc/User/GC";
constexpr const char* kOldWiiTitles = "sdmc:/tico/system/gc/User/Wii/title";
constexpr const char* kMigrationMarker = "sdmc:/tico/config/.migrations/dolphin_saves_layout";
constexpr const char* kOldStateSaves = "sdmc:/tico/system/gc/User/StateSaves";
constexpr const char* kStatesMarker = "sdmc:/tico/config/.migrations/dolphin_states_layout";

// The user's states folder for a platform
std::string StatesFolder(bool wii)
{
  return tico::UserContentRoot("sdmc:/tico/states/", false) + (wii ? "wii/" : "gc/");
}

// A state's platform from its name (the game ID, then .sNN...): Wii discs and
// channels start R, S, W, H or X; anything else is GameCube's.
bool IsWiiStateFile(const std::string& name)
{
  return !name.empty() && std::string("RSWHX").find(name[0]) != std::string::npos;
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

void Migrate(const MigrationProgress& progress)
{
  // The old saves in tico/system/gc belong to the account tico's welcome
  // wizard moved everything to: anyone else leaves them for that account
  // (and the marker unwritten, so they still move when it plays).
  if (!tico::CurrentSession().inheritsShared)
    return;
  if (File::Exists(kMigrationMarker))
    return;

  struct Move
  {
    std::string from;
    std::string to;
  };
  std::vector<Move> moves;
  const auto add_files = [&moves](const std::string& from, const std::string& to) {
    if (!File::IsDirectory(from))
      return;
    for (const File::FSTEntry& entry : File::ScanDirectoryTree(from, false).children)
    {
      if (!entry.isDirectory)
        moves.push_back({entry.physicalName, to + "/" + entry.virtualName});
    }
  };

  // GameCube folder cards: <old>/<USA|EUR|JAP>/Card A -> saves/gc/<USA|EUR|JPN>,
  // Card B under saves/gc/Card B
  constexpr std::pair<const char*, const char*> kRegions[] = {
      {USA_DIR, USA_DIR}, {EUR_DIR, EUR_DIR}, {JAP_DIR, JPN_DIR}, {JPN_DIR, JPN_DIR}};
  for (const auto& [old_region, region] : kRegions)
  {
    const std::string old_dir = fmt::format("{}/{}", kOldGameCubeSaves, old_region);
    add_files(old_dir + "/Card A", fmt::format("{}/{}", GameCubeSaves(), region));
    add_files(old_dir + "/Card B", fmt::format("{}/Card B/{}", GameCubeSaves(), region));
  }

  // GameCube raw cards (MemoryCardA.USA.raw and the like) keep their names
  if (File::IsDirectory(kOldGameCubeSaves))
  {
    for (const File::FSTEntry& entry : File::ScanDirectoryTree(kOldGameCubeSaves, false).children)
    {
      const std::string& name = entry.virtualName;
      if (!entry.isDirectory && name.starts_with("MemoryCard") && name.ends_with(".raw"))
        moves.push_back({entry.physicalName, fmt::format("{}/{}", GameCubeSaves(), name)});
    }
  }

  // Wii: each game's data folder in the NAND, <old>/<type>/<id>/data
  for (const u32 type : {0x00010000u, 0x00010001u, 0x00010004u})
  {
    const std::string type_dir = fmt::format("{}/{:08x}", kOldWiiTitles, type);
    if (!File::IsDirectory(type_dir))
      continue;
    for (const File::FSTEntry& title : File::ScanDirectoryTree(type_dir, false).children)
    {
      const std::string data = title.physicalName + "/data";
      if (!title.isDirectory || title.virtualName.size() != 8 || !HasFiles(data))
        continue;
      const u64 title_id =
          (u64{type} << 32) | std::strtoul(title.virtualName.c_str(), nullptr, 16);
      moves.push_back({data, fmt::format("{}/{}", WiiSaves(), FolderName(title_id))});
    }
  }

  bool ok = true;
  for (size_t i = 0; i < moves.size(); ++i)
  {
    const Move& move = moves[i];
    if (progress)
      progress(move.to, i, moves.size());
    if (File::Exists(move.to))
    {
      // never over a save that is already there; the old one stays where it was
      WARN_LOG_FMT(CORE, "Saves: {} is already there, {} left in place", move.to, move.from);
      continue;
    }
    File::CreateFullPath(move.to);
    if (File::Rename(move.from, move.to))
    {
      NOTICE_LOG_FMT(CORE, "Saves: moved {} to {}", move.from, move.to);
    }
    else
    {
      ERROR_LOG_FMT(CORE, "Saves: could not move {} to {}", move.from, move.to);
      ok = false;
    }
  }
  if (progress && !moves.empty())
    progress({}, moves.size(), moves.size());

  // a move that failed is tried again on the next start
  if (ok)
  {
    File::CreateFullPath(kMigrationMarker);
    File::IOFile marker(kMigrationMarker, "wb");
  }
}

void ApplyStatesFolder(bool wii)
{
  const std::string folder = StatesFolder(wii);
  File::CreateFullPath(folder);
  File::SetUserPath(D_STATESAVES_IDX, folder);

  // Dolphin's own StateSaves (every user's, before): to the account tico moved
  // the shared data to, by platform, never over an existing state.
  if (!tico::CurrentSession().inheritsShared || File::Exists(kStatesMarker) ||
      !File::IsDirectory(kOldStateSaves))
    return;
  for (const File::FSTEntry& entry : File::ScanDirectoryTree(kOldStateSaves, false).children)
  {
    if (entry.isDirectory)
      continue;
    const std::string target = StatesFolder(IsWiiStateFile(entry.virtualName)) + entry.virtualName;
    File::CreateFullPath(target);
    if (!File::Exists(target))
      File::Rename(entry.physicalName, target);
  }
  File::CreateFullPath(kStatesMarker);
  File::IOFile marker(kStatesMarker, "wb");
}

void ApplyGameCubeCardPaths()
{
  // Dolphin adds the region: saves/gc/USA, saves/gc/MemoryCardA.USA.raw, ...
  Config::SetBase(Config::MAIN_GCI_FOLDER_A_PATH, GameCubeSaves());
  Config::SetBase(Config::MAIN_GCI_FOLDER_B_PATH, fmt::format("{}/Card B", GameCubeSaves()));
  Config::SetBase(Config::MAIN_MEMCARD_A_PATH, fmt::format("{}/MemoryCardA.raw", GameCubeSaves()));
  Config::SetBase(Config::MAIN_MEMCARD_B_PATH, fmt::format("{}/MemoryCardB.raw", GameCubeSaves()));
}

std::optional<DiscIO::Riivolution::SavegameRedirect> WiiSaveRedirect()
{
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  if (!IsGameTitle(title_id))
    return std::nullopt;
  // no clone: Migrate moved the saves out of the NAND, and a deleted save
  // folder means a new save rather than the NAND's old one coming back
  return DiscIO::Riivolution::SavegameRedirect{
      fmt::format("{}/{}", WiiSaves(), FolderName(title_id)), false};
}

void ImportWiiSaves()
{
  auto& system = Core::System::GetInstance();
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  if (!ios || !IsGameTitle(title_id) || !File::IsDirectory(WiiImport()))
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
  collect(collect, File::ScanDirectoryTree(WiiImport(), true));

  for (const std::string& path : candidates)
  {
    if (WiiSave::ReadDataBinTitleID(&ios->GetIOSC(), path) != title_id)
      continue;

    // the save it replaces stays beside it
    const std::string save_dir = fmt::format("{}/{}", WiiSaves(), FolderName(title_id));
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
