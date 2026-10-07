// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Portal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"
#include "Core/System.h"
#include "overlay/translation_manager.h"

namespace DolphinNX::Portal
{
namespace
{
namespace USB = IOS::HLE::USB;

// enough for two players and their traps or magic items
constexpr int kSlots = 4;
constexpr u8 kNoSlot = 0xFF;
constexpr const char* kFolders[] = {"sdmc:/tico/skylanders", "sdmc:/emulanders/figures"};

struct Figure
{
  std::string path;
  std::string name;
};

// the figures found when the screen was opened, and what is on each slot
std::vector<Figure> s_figures;
std::array<std::string, kSlots> s_slot_path;
std::array<u8, kSlots> s_slot_on_portal = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};

std::string TrOr(const char* key, const char* fallback)
{
  const std::string text = SwitchFrontend::OverlayTranslation::tr(key);
  return text == key ? fallback : text;
}

bool IsFigureFile(const File::FSTEntry& entry)
{
  std::string name = entry.virtualName;
  std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
  return !entry.isDirectory && entry.size == USB::FIGURE_SIZE &&
         (name.ends_with(".sky") || name.ends_with(".bin") || name.ends_with(".dump") ||
          name.ends_with(".dmp"));
}

// The figure's name from the IDs in its dump, or its file name.
std::string FigureName(const File::FSTEntry& entry)
{
  std::array<u8, USB::FIGURE_SIZE> data{};
  File::IOFile file(entry.physicalName, "rb");
  if (file && file.ReadBytes(data.data(), data.size()))
  {
    const auto ids = Core::System::GetInstance().GetSkylanderPortal().CalculateIDs(data);
    if (const auto it = USB::list_skylanders.find(ids); it != USB::list_skylanders.end())
      return it->second.name;
  }
  const size_t dot = entry.virtualName.find_last_of('.');
  return entry.virtualName.substr(0, dot);
}

void Scan(const File::FSTEntry& dir)
{
  for (const File::FSTEntry& entry : dir.children)
  {
    if (entry.isDirectory)
      Scan(entry);
    else if (IsFigureFile(entry))
      s_figures.push_back({entry.physicalName, FigureName(entry)});
  }
}

int FigureIndex(const std::string& path)
{
  for (size_t i = 0; i < s_figures.size(); ++i)
  {
    if (s_figures[i].path == path)
      return static_cast<int>(i);
  }
  return -1;
}

bool OnAnotherSlot(const std::string& path, int slot)
{
  for (int i = 0; i < kSlots; ++i)
  {
    if (i != slot && s_slot_path[i] == path)
      return true;
  }
  return false;
}

void TakeOff(int slot)
{
  if (s_slot_on_portal[slot] != kNoSlot)
    Core::System::GetInstance().GetSkylanderPortal().RemoveSkylander(s_slot_on_portal[slot]);
  s_slot_on_portal[slot] = kNoSlot;
  s_slot_path[slot].clear();
}

bool PutOn(int slot, const std::string& path)
{
  // r+b: the game writes its progress back into the figure
  File::IOFile file(path, "r+b");
  if (!file)
    return false;
  const u8 on_portal = Core::System::GetInstance().GetSkylanderPortal().LoadSkylander(
      std::make_unique<USB::SkylanderFigure>(std::move(file)));
  if (on_portal == kNoSlot)
    return false;
  s_slot_on_portal[slot] = on_portal;
  s_slot_path[slot] = path;
  return true;
}
}  // namespace

std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> List()
{
  s_figures.clear();
  for (const char* folder : kFolders)
  {
    if (File::IsDirectory(folder))
      Scan(File::ScanDirectoryTree(folder, true));
  }
  std::ranges::sort(s_figures, {}, &Figure::name);

  std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> rows;
  for (int slot = 0; slot < kSlots; ++slot)
  {
    const int figure = FigureIndex(s_slot_path[slot]);
    rows.push_back({fmt::format("{} {}", TrOr("emulator_portal_slot", "Slot"), slot + 1),
                    figure >= 0 ? s_figures[figure].name :
                                  TrOr("emulator_portal_empty", "Empty"),
                    slot});
  }
  if (s_figures.empty())
  {
    File::CreateDirs(kFolders[0]);
    rows.push_back({TrOr("emulator_portal_none", "No figures found"), {}, -1});
    rows.push_back({TrOr("emulator_portal_where",
                         "Copy figure dumps to sdmc:/tico/skylanders"),
                    {}, -1});
  }
  return rows;
}

bool Step(int slot, int direction)
{
  if (slot < 0 || slot >= kSlots || direction == 0)
    return false;

  // Empty (-1), then each figure not on another slot
  const int count = static_cast<int>(s_figures.size());
  int next = FigureIndex(s_slot_path[slot]);
  for (int tries = 0; tries <= count; ++tries)
  {
    next += direction > 0 ? 1 : -1;
    if (next >= count)
      next = -1;
    else if (next < -1)
      next = count - 1;
    if (next < 0 || !OnAnotherSlot(s_figures[next].path, slot))
      break;
  }

  TakeOff(slot);
  if (next >= 0 && !PutOn(slot, s_figures[next].path))
  {
    ERROR_LOG_FMT(IOS_USB, "Portal: could not put {} on the portal", s_figures[next].path);
    return true;  // the slot is empty now
  }
  return true;
}
}  // namespace DolphinNX::Portal
