// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/ControllerProfiles.h"

#include <algorithm>
#include <array>
#include <dirent.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <switch.h>

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

namespace DolphinNX::ControllerProfiles
{
namespace
{
constexpr const char* kFallbackProfile = "pro";

// Searched in order; the first hit wins.
//
// Note there is deliberately no romfs: entry. Profiles are authored in the Tico
// frontend's assets, but romfs is per-NRO — this core cannot read Tico's romfs, and
// its own romfs carries only fonts and lang. Tico is therefore responsible for
// deploying the shipped profiles onto the SD card, the same way dolphin.jsonc gets
// there. If no profile file is found, callers fall back to Dolphin's built-in
// defaults and the mapping is left exactly as it is today.
constexpr std::array<const char*, 4> kProfileDirs = {{
    "sdmc:/tico/config/cores/profiles/dolphin/",
    "tico/config/cores/profiles/dolphin/",
    // Dev/PC builds run from the source tree.
    "assets/config/cores/profiles/dolphin/",
    "../assets/config/cores/profiles/dolphin/",
}};

// Groups a profile is allowed to map, per target. Every one of these is cleared
// before a profile is applied, so an omitted group means "unbound" rather than
// "keep Dolphin's default". Groups absent from this list (Rumble, Options, Mic,
// ...) are never touched.
const std::map<std::string, std::vector<std::string>, std::less<>> kMappableGroups = {
    {Target::GameCube, {"Buttons", "D-Pad", "Main Stick", "C-Stick", "Triggers"}},
    {Target::Wiimote,
     {"Buttons", "D-Pad", "Shake", "IR", "IMUAccelerometer", "IMUGyroscope", "IMUIR"}},
    {Target::WiimoteSideways,
     {"Buttons", "D-Pad", "Shake", "IR", "IMUAccelerometer", "IMUGyroscope", "IMUIR"}},
    {Target::Nunchuk, {"Stick", "Buttons", "IMUAccelerometer"}},
    {Target::Classic, {"Buttons", "Left Stick", "Right Stick", "Triggers", "D-Pad"}},
};

struct Profile
{
  std::string id;
  u32 styles = 0;
  // target -> group -> control -> switch input name
  std::map<std::string, std::map<std::string, std::map<std::string, std::string>>, std::less<>>
      targets;
};

std::map<std::string, std::optional<Profile>, std::less<>> s_cache;

u32 ParseStyleTag(std::string_view name)
{
  static const std::map<std::string_view, u32> kStyles = {
      {"NpadFullKey", HidNpadStyleTag_NpadFullKey},
      {"NpadHandheld", HidNpadStyleTag_NpadHandheld},
      {"NpadJoyDual", HidNpadStyleTag_NpadJoyDual},
      {"NpadJoyLeft", HidNpadStyleTag_NpadJoyLeft},
      {"NpadJoyRight", HidNpadStyleTag_NpadJoyRight},
      {"NpadGc", HidNpadStyleTag_NpadGc},
      {"NpadPalma", HidNpadStyleTag_NpadPalma},
      {"NpadLark", HidNpadStyleTag_NpadLark},
      {"NpadHandheldLark", HidNpadStyleTag_NpadHandheldLark},
      {"NpadLucia", HidNpadStyleTag_NpadLucia},
      {"NpadLagon", HidNpadStyleTag_NpadLagon},
      {"NpadLager", HidNpadStyleTag_NpadLager},
      {"NpadSystemExt", HidNpadStyleTag_NpadSystemExt},
      {"NpadSystem", HidNpadStyleTag_NpadSystem},
  };

  const auto it = kStyles.find(name);
  return it == kStyles.end() ? 0 : it->second;
}

// Same JSON-comment stripping as TicoCore: profiles are plain JSON today, but the
// frontend writes .jsonc and users hand-edit these, so tolerate comments.
std::string StripJsonComments(std::string_view input)
{
  std::string out;
  out.reserve(input.size());

  bool in_string = false;
  bool escaped = false;
  for (size_t i = 0; i < input.size(); ++i)
  {
    const char c = input[i];

    if (in_string)
    {
      out += c;
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }

    if (c == '"')
    {
      in_string = true;
      out += c;
      continue;
    }

    if (c == '/' && i + 1 < input.size())
    {
      if (input[i + 1] == '/')
      {
        while (i < input.size() && input[i] != '\n')
          ++i;
        if (i < input.size())
          out += '\n';
        continue;
      }
      if (input[i + 1] == '*')
      {
        i += 2;
        while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/'))
          ++i;
        ++i;
        continue;
      }
    }

    out += c;
  }

  return out;
}

std::optional<Profile> ParseProfile(const std::string& id, const std::string& text)
{
  picojson::value root;
  const std::string error = picojson::parse(root, StripJsonComments(text));
  if (!error.empty() || !root.is<picojson::object>())
  {
    ERROR_LOG_FMT(COMMON, "DolphinNX profile '{}' failed to parse: {}", id, error);
    return std::nullopt;
  }

  const auto& obj = root.get<picojson::object>();
  Profile profile;
  profile.id = id;

  if (const auto styles = obj.find("styles"); styles != obj.end() && styles->second.is<picojson::array>())
  {
    for (const auto& style : styles->second.get<picojson::array>())
    {
      if (!style.is<std::string>())
        continue;
      const u32 tag = ParseStyleTag(style.get<std::string>());
      if (tag == 0)
        WARN_LOG_FMT(COMMON, "DolphinNX profile '{}': unknown style '{}'", id,
                     style.get<std::string>());
      profile.styles |= tag;
    }
  }

  const auto targets = obj.find("targets");
  if (targets == obj.end() || !targets->second.is<picojson::object>())
  {
    ERROR_LOG_FMT(COMMON, "DolphinNX profile '{}' has no targets", id);
    return std::nullopt;
  }

  for (const auto& [target_name, groups_value] : targets->second.get<picojson::object>())
  {
    if (!groups_value.is<picojson::object>())
      continue;

    auto& groups = profile.targets[target_name];
    for (const auto& [group_name, controls_value] : groups_value.get<picojson::object>())
    {
      if (!controls_value.is<picojson::object>())
        continue;

      auto& controls = groups[group_name];
      for (const auto& [control_name, expr] : controls_value.get<picojson::object>())
      {
        if (expr.is<std::string>())
          controls[control_name] = expr.get<std::string>();
      }
    }
  }

  return profile;
}

const std::optional<Profile>& Load(const std::string& id)
{
  if (const auto it = s_cache.find(id); it != s_cache.end())
    return it->second;

  auto& slot = s_cache[id];
  slot = std::nullopt;

  for (const char* dir : kProfileDirs)
  {
    const std::string path = std::string(dir) + id + ".json";
    std::string content;
    if (!File::ReadFileToString(path, content))
      continue;

    slot = ParseProfile(id, content);
    if (slot)
    {
      INFO_LOG_FMT(COMMON, "DolphinNX loaded controller profile '{}' from {}", id, path);
      break;
    }
  }

  if (!slot)
    ERROR_LOG_FMT(COMMON, "DolphinNX controller profile '{}' not found", id);

  return slot;
}

// Profile ids available on disk, from the first search directory that exists.
std::vector<std::string> ListProfileIds()
{
  std::vector<std::string> ids;

  for (const char* dir : kProfileDirs)
  {
    DIR* handle = opendir(dir);
    if (!handle)
      continue;

    while (const dirent* entry = readdir(handle))
    {
      const std::string_view name = entry->d_name;
      if (name.size() < 6 || name.substr(name.size() - 5) != ".json")
        continue;
      ids.emplace_back(name.substr(0, name.size() - 5));
    }
    closedir(handle);

    if (!ids.empty())
      break;
  }

  std::sort(ids.begin(), ids.end());
  return ids;
}

ControllerEmu::ControlGroup* FindGroup(ControllerEmu::ControlGroupContainer* controller,
                                       std::string_view name)
{
  for (const auto& group : controller->groups)
  {
    if (group->name == name)
      return group.get();
  }
  return nullptr;
}

}  // namespace

std::string Resolve(const std::string& configured, unsigned player)
{
  if (!configured.empty() && configured != "auto")
    return configured;

  const u32 no1_style = hidGetNpadStyleSet(static_cast<HidNpadIdType>(HidNpadIdType_No1 + player));
  const u32 handheld_style = (player == 0) ? hidGetNpadStyleSet(HidNpadIdType_Handheld) : 0;
  // In handheld mode the console's built-in Joy-Cons are reported under the
  // Handheld npad id, not No1 -- mirroring padInitialize(s_pads[0], No1, Handheld)
  // in Input.cpp. Without this, auto-detect sees No1 == 0, falls back to the
  // default profile, and a remapped "handheld" profile silently never applies.
  u32 style_set = no1_style;
  if (style_set == 0 && player == 0)
    style_set = handheld_style;
  if (style_set == 0)
    return kFallbackProfile;

  // Ask every profile on disk whether it claims one of the reported styles. This is
  // enumerated rather than hardcoded so shipping a new profile needs no code change.
  // User-created profiles declare no styles and so are never auto-selected, which is
  // what keeps detection unambiguous; the frontend's validator enforces the same for
  // shipped ones.
  for (const std::string& id : ListProfileIds())
  {
    const auto& profile = Load(id);
    const u32 pstyles = profile ? profile->styles : 0;
    if (profile && (pstyles & style_set) != 0)
      return profile->id;
  }

  return kFallbackProfile;
}

bool Apply(ControllerEmu::ControlGroupContainer* controller, const std::string& profile_id,
           const std::string& target)
{
  if (!controller)
    return false;

  const auto& profile = Load(profile_id);
  if (!profile)
    return false;

  const auto target_it = profile->targets.find(target);
  if (target_it == profile->targets.end())
  {
    WARN_LOG_FMT(COMMON, "DolphinNX profile '{}' has no '{}' target", profile_id, target);
    return false;
  }

  const auto mappable = kMappableGroups.find(target);
  if (mappable == kMappableGroups.end())
    return false;

  // Clear first, then bind. A profile fully determines the mapping for its target.
  for (const std::string& group_name : mappable->second)
  {
    auto* group = FindGroup(controller, group_name);
    if (!group)
      continue;
    for (size_t i = 0; i < group->controls.size(); ++i)
      group->SetControlExpression(static_cast<int>(i), "");
  }

  for (const auto& [group_name, controls] : target_it->second)
  {
    auto* group = FindGroup(controller, group_name);
    if (!group)
    {
      WARN_LOG_FMT(COMMON, "DolphinNX profile '{}': {} has no group '{}'", profile_id, target,
                   group_name);
      continue;
    }

    for (const auto& [control_name, expression] : controls)
    {
      bool bound = false;
      for (size_t i = 0; i < group->controls.size(); ++i)
      {
        if (group->controls[i]->name != control_name)
          continue;
        group->SetControlExpression(static_cast<int>(i), "`" + expression + "`");
        bound = true;
        break;
      }

      if (!bound)
      {
        WARN_LOG_FMT(COMMON, "DolphinNX profile '{}': {}/{} has no control '{}'", profile_id,
                     target, group_name, control_name);
      }
    }
  }

  return true;
}

void Reload()
{
  s_cache.clear();
}

}  // namespace DolphinNX::ControllerProfiles
