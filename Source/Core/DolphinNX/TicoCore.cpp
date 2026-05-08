// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/TicoCore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/Enums.h"
#include "VideoCommon/VideoConfig.h"

namespace DolphinNX::TicoCore
{
namespace
{

using OptionMap = std::map<std::string, std::string, std::less<>>;

constexpr std::array<const char*, 6> kConfigPaths = {{
    "sdmc:/tiicu/config/cores/dolphin.jsonc",
    "sdmc:/tico/config/cores/dolphin.jsonc",
    "tico/config/cores/dolphin.jsonc",
    "romfs:/assets/config/cores/dolphin.jsonc",
    "assets/config/cores/dolphin.jsonc",
    "../assets/config/cores/dolphin.jsonc",
}};

constexpr std::string_view kDefaultWritableConfigPath = "sdmc:/tico/config/cores/dolphin.jsonc";

constexpr std::array<std::string_view, 30> kFixedBaseOptions = {{
    "dolphin_cpu_core",
    "dolphin_main_cpu_thread",
    "dolphin_fastmem",
    "dolphin_fastmem_arena",
    "dolphin_main_accurate_cpu_cache",
    "dolphin_cheats_enabled",
    "dolphin_enable_savestates",
    "dolphin_dsp_hle",
    "dolphin_dsp_thread",
    "dolphin_renderer",
    "dolphin_dsp_jit",
    "dolphin_osd_enabled",
    "dolphin_log_level",
    "dolphin_debug_mode_enabled",
    "dolphin_gc_sp1",
    "dolphin_show_fps",
    "dolphin_show_ftimes",
    "dolphin_show_vps",
    "dolphin_show_vtimes",
    "dolphin_show_graphs",
    "dolphin_show_speed",
    "dolphin_show_speed_colors",
    "dolphin_overlay_stats",
    "dolphin_overlay_proj_stats",
    "dolphin_overlay_scissor_stats",
    "dolphin_enable_validation_layer",
    "dolphin_texfmt_overlay_enable",
    "dolphin_texfmt_overlay_center",
    "dolphin_enable_wireframe",
    "dolphin_mods_enable",
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 73> kDefaultOptions = {{
    {"display_mode", "Display"},
    {"display_size", "4:3"},
    {"integer_scale", "Auto"},
    {"dolphin_cpu_clock_rate", "1.0"},
    {"dolphin_emulation_speed", "1.0"},
    {"dolphin_precision_frame_timing", "disabled"},
    {"dolphin_cheats_import", "enabled"},
    {"dolphin_skip_gc_bios", "disabled"},
    {"dolphin_language", "1"},
    {"dolphin_fast_disc_speed", "disabled"},
    {"dolphin_main_mmu", "disabled"},
    {"dolphin_rush_presentation", "disabled"},
    {"dolphin_early_presentation", "disabled"},
    {"dolphin_call_back_audio_method", "0"},
    {"dolphin_enable_gamecube_mic", "disabled"},
    {"dolphin_hotkey_activate_microphone", "Disabled"},
    {"dolphin_widescreen", "enabled"},
    {"dolphin_progressive_scan", "enabled"},
    {"dolphin_pal60", "disabled"},
    {"dolphin_sensor_bar_position", "0"},
    {"dolphin_enable_rumble", "enabled"},
    {"dolphin_wiimote_continuous_scanning", "disabled"},
    {"dolphin_alt_gc_ports_on_wii", "disabled"},
    {"dolphin_wiispeak_enable", "disabled"},
    {"dolphin_wiispeak_muted", "enabled"},
    {"dolphin_wii_logi_microphone_enable", "disabled"},
    {"dolphin_widescreen_hack", "disabled"},
    {"dolphin_crop_overscan", "disabled"},
    {"dolphin_efb_scale", "1"},
    {"dolphin_shader_compilation_mode", "2"},
    {"dolphin_wait_for_shaders", "disabled"},
    {"dolphin_anti_aliasing", "0"},
    {"dolphin_texture_cache_accuracy", "128"},
    {"dolphin_gpu_texture_decoding", "disabled"},
    {"dolphin_pixel_lighting", "disabled"},
    {"dolphin_fast_depth_calculation", "enabled"},
    {"dolphin_disable_fog", "disabled"},
    {"dolphin_force_texture_filtering_mode", "0"},
    {"dolphin_max_anisotropy", "0"},
    {"dolphin_load_custom_textures", "disabled"},
    {"dolphin_cache_custom_textures", "disabled"},
    {"dolphin_enhance_output_resampling", "0"},
    {"dolphin_force_true_color", "disabled"},
    {"dolphin_disable_copy_filter", "enabled"},
    {"dolphin_enhance_hdr_output", "disabled"},
    {"dolphin_mipmap_detection", "disabled"},
    {"dolphin_efb_access_enable", "disabled"},
    {"dolphin_efb_access_defer_invalidation", "disabled"},
    {"dolphin_efb_access_tile_size", "64"},
    {"dolphin_bbox_enabled", "disabled"},
    {"dolphin_force_progressive", "enabled"},
    {"dolphin_efb_to_texture", "enabled"},
    {"dolphin_xfb_to_texture_enable", "enabled"},
    {"dolphin_efb_to_vram", "disabled"},
    {"dolphin_defer_efb_copies", "enabled"},
    {"dolphin_immediate_xfb", "disabled"},
    {"dolphin_skip_dupe_frames", "enabled"},
    {"dolphin_early_xfb_output", "enabled"},
    {"dolphin_efb_scaled_copy", "enabled"},
    {"dolphin_efb_emulate_format_changes", "disabled"},
    {"dolphin_vertex_rounding", "disabled"},
    {"dolphin_vi_skip", "auto"},
    {"dolphin_fast_texture_sampling", "enabled"},
    {"dolphin_hotkey_sideways_toggle", "L3"},
    {"dolphin_ir_mode", "1"},
    {"dolphin_ir_offset", "0"},
    {"dolphin_ir_yaw", "25"},
    {"dolphin_ir_pitch", "25"},
    {"dolphin_ir_deadzone", "0"},
    {"dolphin_ir_modifier", "None"},
    {"dolphin_swing_modifier", "Disabled"},
    {"dolphin_swing_angle", "90"},
    {"dolphin_save_load_settings", "disabled"},
}};

static_assert(kDefaultOptions.size() == 73);

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

std::string TrimAscii(std::string_view input)
{
  size_t start = 0;
  size_t end = input.size();

  while (start < end && static_cast<unsigned char>(input[start]) <= ' ')
    ++start;
  while (end > start && static_cast<unsigned char>(input[end - 1]) <= ' ')
    --end;

  return std::string(input.substr(start, end - start));
}

std::string ToLowerAscii(std::string_view input)
{
  std::string lower;
  lower.reserve(input.size());
  for (const char c : input)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lower;
}

std::optional<bool> ParseBool(std::string_view value)
{
  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "1" || lower == "true" || lower == "enabled" || lower == "yes" ||
      lower == "on")
  {
    return true;
  }

  if (lower == "0" || lower == "false" || lower == "disabled" || lower == "no" ||
      lower == "off")
  {
    return false;
  }

  return std::nullopt;
}

std::optional<int> ParseInt(std::string_view value)
{
  const std::string trimmed = TrimAscii(value);
  if (trimmed.empty())
    return std::nullopt;

  char* end = nullptr;
  const long parsed = std::strtol(trimmed.c_str(), &end, 10);
  if (!end || *end != '\0')
    return std::nullopt;

  return static_cast<int>(parsed);
}

std::optional<double> ParseDouble(std::string_view value)
{
  const std::string trimmed = TrimAscii(value);
  if (trimmed.empty())
    return std::nullopt;

  char* end = nullptr;
  const double parsed = std::strtod(trimmed.c_str(), &end);
  if (!end || *end != '\0')
    return std::nullopt;

  return parsed;
}

std::string NormalizeNumber(double value)
{
  if (std::fabs(value - std::round(value)) < 0.000001)
    return std::to_string(static_cast<long long>(std::llround(value)));

  std::string text = std::to_string(value);
  while (!text.empty() && text.back() == '0')
    text.pop_back();
  if (!text.empty() && text.back() == '.')
    text.pop_back();
  return text;
}

std::string JsonValueToString(const picojson::value& value)
{
  if (value.is<std::string>())
    return value.get<std::string>();

  if (value.is<bool>())
    return value.get<bool>() ? "true" : "false";

  if (value.is<double>())
    return NormalizeNumber(value.get<double>());

  return {};
}

bool IsWritablePath(std::string_view path)
{
  return !path.starts_with("romfs:/");
}

std::string EscapeJsonString(std::string_view input)
{
  std::string escaped;
  escaped.reserve(input.size());

  for (const char c : input)
  {
    switch (c)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(c);
      break;
    }
  }

  return escaped;
}

AspectMode ParseAspectMode(std::string_view value, AspectMode fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<AspectMode>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "auto")
    return static_cast<AspectMode>(0);
  if (lower == "force wide" || lower == "forcewide")
    return static_cast<AspectMode>(1);
  if (lower == "force standard" || lower == "forcestandard")
    return static_cast<AspectMode>(2);
  if (lower == "stretch")
    return AspectMode::Stretch;
  if (lower == "custom")
    return static_cast<AspectMode>(4);
  if (lower == "custom stretch" || lower == "customstretch")
    return static_cast<AspectMode>(5);
  if (lower == "raw")
    return static_cast<AspectMode>(6);

  return fallback;
}

ViewportMode ParseViewportMode(std::string_view value, ViewportMode fallback)
{
  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "display")
    return ViewportMode::Display;
  if (lower == "integer")
    return ViewportMode::Integer;
  return fallback;
}

ViewportDisplaySize ParseViewportDisplaySize(std::string_view value,
                                             ViewportDisplaySize fallback)
{
  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "stretch")
    return ViewportDisplaySize::Stretch;
  if (lower == "4:3" || lower == "43")
    return ViewportDisplaySize::FourThree;
  if (lower == "16:9" || lower == "169")
    return ViewportDisplaySize::SixteenNine;
  if (lower == "original")
    return ViewportDisplaySize::Original;
  return fallback;
}

ViewportIntegerScale ParseViewportIntegerScale(std::string_view value,
                                               ViewportIntegerScale fallback)
{
  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "auto")
    return ViewportIntegerScale::Auto;
  if (lower == "1x")
    return ViewportIntegerScale::OneX;
  if (lower == "2x")
    return ViewportIntegerScale::TwoX;
  return fallback;
}

AspectMode ResolveViewportAspectMode(ViewportMode viewport_mode,
                                     ViewportDisplaySize viewport_display_size)
{
  if (viewport_mode == ViewportMode::Integer)
    return AspectMode::Raw;

  switch (viewport_display_size)
  {
  case ViewportDisplaySize::Stretch:
    return AspectMode::Stretch;
  case ViewportDisplaySize::SixteenNine:
    return AspectMode::ForceWide;
  case ViewportDisplaySize::Original:
    return AspectMode::Auto;
  case ViewportDisplaySize::FourThree:
  default:
    return AspectMode::ForceStandard;
  }
}

ShaderCompilationMode ParseShaderCompilationMode(std::string_view value,
                                                 ShaderCompilationMode fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<ShaderCompilationMode>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "synchronous")
    return ShaderCompilationMode::Synchronous;
  if (lower == "synchronous ubershaders" || lower == "sync (ubershaders)" ||
      lower == "syncubershaders")
  {
    return ShaderCompilationMode::SynchronousUberShaders;
  }
  if (lower == "asynchronous ubershaders" || lower == "async (ubershaders)" ||
      lower == "asyncubershaders")
  {
    return ShaderCompilationMode::AsynchronousUberShaders;
  }
  if (lower == "asynchronous skip rendering" || lower == "async (skip rendering)" ||
      lower == "asyncskiprendering")
  {
    return ShaderCompilationMode::AsynchronousSkipRendering;
  }

  return fallback;
}

TextureFilteringMode ParseTextureFilteringMode(std::string_view value,
                                               TextureFilteringMode fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<TextureFilteringMode>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "default")
    return TextureFilteringMode::Default;
  if (lower == "nearest" || lower == "nearest (sharp)")
    return TextureFilteringMode::Nearest;
  if (lower == "linear" || lower == "linear (smooth)")
    return TextureFilteringMode::Linear;

  return fallback;
}

AnisotropicFilteringMode ParseAnisotropy(std::string_view value,
                                         AnisotropicFilteringMode fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<AnisotropicFilteringMode>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "1x" || lower == "off")
    return AnisotropicFilteringMode::Force1x;
  if (lower == "2x")
    return AnisotropicFilteringMode::Force2x;
  if (lower == "4x")
    return AnisotropicFilteringMode::Force4x;
  if (lower == "8x")
    return AnisotropicFilteringMode::Force8x;
  if (lower == "16x")
    return AnisotropicFilteringMode::Force16x;

  return fallback;
}

OutputResamplingMode ParseOutputResampling(std::string_view value, OutputResamplingMode fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<OutputResamplingMode>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "default")
    return OutputResamplingMode::Default;
  if (lower == "bilinear")
    return OutputResamplingMode::Bilinear;
  if (lower == "b-spline" || lower == "bspline")
    return OutputResamplingMode::BSpline;
  if (lower == "mitchell-netravali" || lower == "mitchell netravali")
    return OutputResamplingMode::MitchellNetravali;
  if (lower == "catmull-rom" || lower == "catmull rom")
    return OutputResamplingMode::CatmullRom;
  if (lower == "sharp bilinear")
    return OutputResamplingMode::SharpBilinear;
  if (lower == "area sampling")
    return OutputResamplingMode::AreaSampling;

  return fallback;
}

ExpansionInterface::EXIDeviceType ParseSP1Device(std::string_view value,
                                                 ExpansionInterface::EXIDeviceType fallback)
{
  if (const auto parsed = ParseInt(value))
    return static_cast<ExpansionInterface::EXIDeviceType>(*parsed);

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "none")
    return ExpansionInterface::EXIDeviceType::None;
  if (lower == "dummy")
    return ExpansionInterface::EXIDeviceType::Dummy;
  if (lower == "microphone")
    return ExpansionInterface::EXIDeviceType::Microphone;
  if (lower == "ethernet")
    return ExpansionInterface::EXIDeviceType::Ethernet;
  if (lower == "ethernet xlink" || lower == "ethernetxlink")
    return ExpansionInterface::EXIDeviceType::EthernetXLink;
  if (lower == "ethernet tapserver" || lower == "ethernettapserver")
    return ExpansionInterface::EXIDeviceType::EthernetTapServer;
  if (lower == "ethernet built-in" || lower == "ethernet builtin" || lower == "ethernetbuiltin")
    return ExpansionInterface::EXIDeviceType::EthernetBuiltIn;
  if (lower == "modem tapserver" || lower == "modemtapserver")
    return ExpansionInterface::EXIDeviceType::ModemTapServer;
  if (lower == "baseboard")
    return ExpansionInterface::EXIDeviceType::Baseboard;

  return fallback;
}

Common::Log::LogLevel ParseLogLevel(std::string_view value, Common::Log::LogLevel fallback)
{
  if (const auto parsed = ParseInt(value))
  {
    const int clamped = std::clamp(*parsed, 1, static_cast<int>(Common::Log::MAX_EFFECTIVE_LOGLEVEL));
    return static_cast<Common::Log::LogLevel>(clamped);
  }

  const std::string lower = ToLowerAscii(TrimAscii(value));
  if (lower == "notice")
    return Common::Log::LogLevel::LNOTICE;
  if (lower == "error")
    return Common::Log::LogLevel::LERROR;
  if (lower == "warning")
    return Common::Log::LogLevel::LWARNING;
  if (lower == "info")
    return Common::Log::LogLevel::LINFO;
  if (lower == "debug")
    return Common::Log::LogLevel::LDEBUG;

  return fallback;
}

std::optional<Config::Location> ParseRawLocationKey(std::string_view key)
{
  const size_t first_slash = key.find('/');
  if (first_slash == std::string_view::npos)
    return std::nullopt;

  const size_t second_slash = key.find('/', first_slash + 1);
  if (second_slash == std::string_view::npos)
    return std::nullopt;

  if (key.find('/', second_slash + 1) != std::string_view::npos)
    return std::nullopt;

  const std::string system_name(key.substr(0, first_slash));
  const std::optional<Config::System> system = Config::GetSystemFromName(system_name);
  if (!system)
    return std::nullopt;

  const std::string section(key.substr(first_slash + 1, second_slash - first_slash - 1));
  const std::string config_key(key.substr(second_slash + 1));
  if (section.empty() || config_key.empty())
    return std::nullopt;

  return Config::Location{*system, section, config_key};
}

bool IsFixedBaseOption(std::string_view key)
{
  return std::find(kFixedBaseOptions.begin(), kFixedBaseOptions.end(), key) !=
         kFixedBaseOptions.end();
}

bool IsViewportOption(std::string_view key)
{
  return key == "display_mode" || key == "display_size" || key == "integer_scale";
}

class ConfigManager final
{
public:
  void ReloadConfig()
  {
    m_loaded = false;
    m_options.clear();
    m_loaded_path.clear();
    m_save_path = std::string(kDefaultWritableConfigPath);
    EnsureLoaded();
  }

  std::string GetConfigValue(std::string_view key, std::string_view default_value)
  {
    EnsureLoaded();

    const auto it = m_options.find(key);
    if (it != m_options.end())
      return it->second;
    return std::string(default_value);
  }

  void SetConfigValue(const std::string& key, const std::string& value)
  {
    EnsureLoaded();
    if (IsFixedBaseOption(key))
      return;
    m_options[key] = value;
    if (IsViewportOption(key))
      ApplyViewportOverrides();
  }

  bool SaveConfig()
  {
    EnsureLoaded();

    if (!File::CreateFullPath(m_save_path))
      return false;

    std::ostringstream out;
    out << "{\n";

    bool first = true;
    for (const auto& [key, value] : m_options)
    {
      if (!first)
        out << ",\n";
      first = false;

      out << "  \"" << EscapeJsonString(key) << "\": \"" << EscapeJsonString(value) << '"';
    }

    out << "\n}\n";
    return File::WriteStringToFile(m_save_path, out.str());
  }

  std::string GetLoadedConfigPath()
  {
    EnsureLoaded();
    return m_loaded_path;
  }

  std::size_t GetLoadedOptionCount()
  {
    EnsureLoaded();
    return m_options.size();
  }

  void ApplyConfig(bool is_gamecube_disc)
  {
    EnsureLoaded();

    Config::ConfigChangeCallbackGuard config_guard;

    ApplyBaselineDefaults(is_gamecube_disc);
    ApplyOverrides();
    ApplyRawLocationOverrides();
  }

private:
  void EnsureLoaded()
  {
    if (m_loaded)
      return;

    LoadBuiltinDefaults();

    for (const char* path : kConfigPaths)
    {
      std::string content;
      if (!File::ReadFileToString(path, content))
        continue;

      const std::string stripped = StripJsonComments(content);
      picojson::value root;
      const std::string error = picojson::parse(root, stripped);
      if (!error.empty() || !root.is<picojson::object>())
        continue;

      for (const auto& [key, value] : root.get<picojson::object>())
      {
        const std::string normalized = JsonValueToString(value);
        if (!normalized.empty())
          m_options[key] = normalized;
      }

      for (const std::string_view key : kFixedBaseOptions)
        m_options.erase(std::string(key));

      m_loaded_path = path;
      m_save_path = IsWritablePath(path) ? path : std::string(kDefaultWritableConfigPath);
      break;
    }

    m_loaded = true;
  }

  void LoadBuiltinDefaults()
  {
    for (const auto& [key, value] : kDefaultOptions)
      m_options[std::string(key)] = std::string(value);
  }

  bool GetBool(std::string_view key, bool fallback)
  {
    const auto it = m_options.find(key);
    if (it == m_options.end())
      return fallback;

    if (const auto parsed = ParseBool(it->second))
      return *parsed;

    return fallback;
  }

  int GetInt(std::string_view key, int fallback)
  {
    const auto it = m_options.find(key);
    if (it == m_options.end())
      return fallback;

    if (const auto parsed = ParseInt(it->second))
      return *parsed;

    return fallback;
  }

  double GetDouble(std::string_view key, double fallback)
  {
    const auto it = m_options.find(key);
    if (it == m_options.end())
      return fallback;

    if (const auto parsed = ParseDouble(it->second))
      return *parsed;

    return fallback;
  }

  std::string GetString(std::string_view key, std::string_view fallback)
  {
    const auto it = m_options.find(key);
    if (it != m_options.end())
      return it->second;
    return std::string(fallback);
  }

  void ApplyViewportOverrides()
  {
    Config::ConfigChangeCallbackGuard guard;

    const ViewportMode viewport_mode =
        ParseViewportMode(GetString("display_mode", "Display"), Config::Get(Config::GFX_VIEWPORT_MODE));
    const ViewportDisplaySize viewport_display_size = ParseViewportDisplaySize(
        GetString("display_size", "4:3"), Config::Get(Config::GFX_VIEWPORT_DISPLAY_SIZE));
    const ViewportIntegerScale viewport_integer_scale = ParseViewportIntegerScale(
        GetString("integer_scale", "Auto"), Config::Get(Config::GFX_VIEWPORT_INTEGER_SCALE));

    Config::SetBase(Config::GFX_VIEWPORT_MODE, viewport_mode);
    Config::SetBase(Config::GFX_VIEWPORT_DISPLAY_SIZE, viewport_display_size);
    Config::SetBase(Config::GFX_VIEWPORT_INTEGER_SCALE, viewport_integer_scale);
    Config::SetBase(Config::GFX_ASPECT_RATIO,
                    ResolveViewportAspectMode(viewport_mode, viewport_display_size));
  }

  void ApplyRawLocationOverrides()
  {
    const auto layer = Config::GetLayer(Config::LayerType::Base);
    if (!layer)
      return;

    for (const auto& [key, value] : m_options)
    {
      const std::optional<Config::Location> location = ParseRawLocationKey(key);
      if (!location)
        continue;

      layer->Set(*location, value);
    }
  }

  void ApplyBaselineDefaults(bool is_gamecube_disc)
  {
    Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::JITARM64);
    Config::SetBase(Config::MAIN_FASTMEM, true);
    Config::SetBase(Config::MAIN_FASTMEM_ARENA, true);
    Config::SetBase(Config::MAIN_ACCURATE_CPU_CACHE, false);
    Config::SetBase(Config::MAIN_CPU_THREAD, true);
    Config::SetBase(Config::MAIN_LARGE_ENTRY_POINTS_MAP, true);
    Config::SetBase(Config::MAIN_SKIP_IPL, true);
    Config::SetBase(Config::MAIN_MMU, false);
    Config::SetBase(Config::MAIN_SYNC_ON_SKIP_IDLE, true);

    Config::SetBase(Config::MAIN_DSP_HLE, true);
    Config::SetBase(Config::MAIN_DSP_THREAD, false);
    Config::SetBase(Config::MAIN_DSP_JIT, true);
    Config::SetBase(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_SWITCHNX));
    Config::SetBase(Config::MAIN_AUDIO_LATENCY, 0);
    Config::SetBase(Config::MAIN_AUDIO_BUFFER_SIZE, 128);
    Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, true);
    Config::SetBase(Config::MAIN_DPL2_DECODER, false);
    Config::SetBase(Config::MAIN_DUMP_AUDIO, false);

    Config::SetBase(Config::GFX_VSYNC, true);
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));
    Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING, false);
    Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING, false);
    Config::SetBase(Config::GFX_EFB_SCALE, 1);
    Config::SetBase(Config::GFX_MSAA, 0);
    Config::SetBase(Config::GFX_SSAA, false);
    Config::SetBase(Config::GFX_VIEWPORT_MODE, ViewportMode::Display);
    Config::SetBase(Config::GFX_VIEWPORT_DISPLAY_SIZE, ViewportDisplaySize::FourThree);
    Config::SetBase(Config::GFX_VIEWPORT_INTEGER_SCALE, ViewportIntegerScale::Auto);
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Stretch);
    Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, true);
    Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, false);
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                    ShaderCompilationMode::AsynchronousUberShaders);
    Config::SetBase(Config::GFX_SHADER_PRECOMPILER_THREADS, 1);

    Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);
    Config::SetBase(Config::GFX_HACK_EFB_DEFER_INVALIDATION, false);
    Config::SetBase(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE, 64);
    Config::SetBase(Config::GFX_HACK_BBOX_ENABLE, false);
    Config::SetBase(Config::GFX_HACK_FORCE_PROGRESSIVE, true);
    Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, true);
    Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, true);
    Config::SetBase(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, false);
    Config::SetBase(Config::GFX_HACK_DEFER_EFB_COPIES, true);
    Config::SetBase(Config::GFX_HACK_IMMEDIATE_XFB, false);
    Config::SetBase(Config::GFX_HACK_SKIP_DUPLICATE_XFBS, true);
    Config::SetBase(Config::GFX_HACK_EARLY_XFB_OUTPUT, true);
    Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED, true);
    Config::SetBase(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, false);
    Config::SetBase(Config::GFX_HACK_VERTEX_ROUNDING, false);
    Config::SetBase(Config::GFX_HACK_VI_SKIP, is_gamecube_disc);
    Config::SetBase(Config::GFX_HACK_FAST_TEXTURE_SAMPLING, true);
    Config::SetBase(Config::GFX_FAST_DEPTH_CALC, true);

    Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, false);
    Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, true);

    Config::SetBase(Config::MAIN_EMULATION_SPEED, 1.0f);
    Config::SetBase(Config::MAIN_PRECISION_FRAME_TIMING, false);

    Config::SetBase(Config::SYSCONF_LANGUAGE, static_cast<u32>(DiscIO::Language::English));
    Config::SetBase(Config::MAIN_GC_LANGUAGE,
                    DiscIO::ToGameCubeLanguage(DiscIO::Language::English));
    Config::SetBase(Config::SYSCONF_WIDESCREEN, true);
    Config::SetBase(Config::SYSCONF_PROGRESSIVE_SCAN, true);
    Config::SetBase(Config::SYSCONF_PAL60, false);
    Config::SetBase(Config::SYSCONF_SENSOR_BAR_POSITION, 0u);
    Config::SetBase(Config::SYSCONF_WIIMOTE_MOTOR, true);

    Config::SetBase(Config::MAIN_ENABLE_CHEATS, false);
    Config::SetBase(Config::MAIN_ENABLE_SAVESTATES, false);
    Config::SetBase(Config::MAIN_OSD_MESSAGES, false);
    Config::SetBase(Config::MAIN_ENABLE_DEBUGGING, false);
    Config::SetBase(Config::MAIN_TIME_TRACKING, false);
    Common::Log::LogManager::GetInstance()->SetConfigLogLevel(Common::Log::LogLevel::LNOTICE);
    Config::SetBase(Config::MAIN_FAST_DISC_SPEED, false);
    Config::SetBase(Config::MAIN_OVERCLOCK, 1.0f);
    Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, false);
    Config::SetBase(Config::MAIN_RUSH_FRAME_PRESENTATION, false);
    Config::SetBase(Config::MAIN_SMOOTH_EARLY_PRESENTATION, false);
    Config::SetBase(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING, false);
    Config::SetBase(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::None);
    Config::SetBase(Config::MAIN_EMULATE_WII_SPEAK, false);
    Config::SetBase(Config::MAIN_WII_SPEAK_MUTED, true);
    Config::SetBase(Config::MAIN_EMULATE_LOGITECH_MIC[0], false);
    Config::SetBase(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
    Config::SetBase(Config::GFX_SHOW_FPS, false);
    Config::SetBase(Config::GFX_SHOW_FTIMES, false);
    Config::SetBase(Config::GFX_SHOW_VPS, false);
    Config::SetBase(Config::GFX_SHOW_VTIMES, false);
    Config::SetBase(Config::GFX_SHOW_GRAPHS, false);
    Config::SetBase(Config::GFX_SHOW_SPEED, false);
    Config::SetBase(Config::GFX_SHOW_SPEED_COLORS, false);
    Config::SetBase(Config::GFX_OVERLAY_STATS, false);
    Config::SetBase(Config::GFX_OVERLAY_PROJ_STATS, false);
    Config::SetBase(Config::GFX_OVERLAY_SCISSOR_STATS, false);
    Config::SetBase(Config::GFX_ENABLE_VALIDATION_LAYER, false);
    Config::SetBase(Config::GFX_TEXFMT_OVERLAY_ENABLE, false);
    Config::SetBase(Config::GFX_TEXFMT_OVERLAY_CENTER, false);
    Config::SetBase(Config::GFX_ENABLE_WIREFRAME, false);
    Config::SetBase(Config::GFX_MODS_ENABLE, false);

    Core::SetIsThrottlerTempDisabled(false);
  }

  void ApplyOverrides()
  {
    Config::SetBase(Config::MAIN_SKIP_IPL,
                    GetBool("dolphin_skip_gc_bios", Config::Get(Config::MAIN_SKIP_IPL)));
    Config::SetBase(Config::MAIN_SYNC_ON_SKIP_IDLE,
                    GetBool("dolphin_sync_on_skip_idle",
                            Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE)));
    Config::SetBase(Config::MAIN_EMULATION_SPEED,
                    static_cast<float>(GetDouble("dolphin_emulation_speed",
                                                 Config::Get(Config::MAIN_EMULATION_SPEED))));
    {
      const float cpu_clock =
          static_cast<float>(GetDouble("dolphin_cpu_clock_rate", Config::Get(Config::MAIN_OVERCLOCK)));
      Config::SetBase(Config::MAIN_OVERCLOCK, cpu_clock);
      Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, std::fabs(cpu_clock - 1.0f) > 0.0001f);
    }
    Config::SetBase(Config::MAIN_PRECISION_FRAME_TIMING,
                    GetBool("dolphin_precision_frame_timing",
                            Config::Get(Config::MAIN_PRECISION_FRAME_TIMING)));
    Config::SetBase(Config::MAIN_MMU,
                    GetBool("dolphin_main_mmu", Config::Get(Config::MAIN_MMU)));
    Config::SetBase(Config::MAIN_SYNC_GPU,
                    GetBool("dolphin_sync_gpu", Config::Get(Config::MAIN_SYNC_GPU)));
    Config::SetBase(Config::MAIN_SYNC_GPU_MAX_DISTANCE,
                    GetInt("dolphin_sync_gpu_max_distance",
                           Config::Get(Config::MAIN_SYNC_GPU_MAX_DISTANCE)));
    Config::SetBase(Config::MAIN_SYNC_GPU_MIN_DISTANCE,
                    GetInt("dolphin_sync_gpu_min_distance",
                           Config::Get(Config::MAIN_SYNC_GPU_MIN_DISTANCE)));
    Config::SetBase(Config::MAIN_SYNC_GPU_OVERCLOCK,
                    static_cast<float>(GetDouble("dolphin_sync_gpu_overclock",
                                                 Config::Get(Config::MAIN_SYNC_GPU_OVERCLOCK))));
    Config::SetBase(Config::MAIN_CUSTOM_RTC_ENABLE,
                    GetBool("dolphin_custom_rtc_enable",
                            Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE)));
    Config::SetBase(Config::MAIN_CUSTOM_RTC_VALUE,
                    static_cast<u32>(GetInt("dolphin_custom_rtc_value",
                                            static_cast<int>(
                                                Config::Get(Config::MAIN_CUSTOM_RTC_VALUE)))));
    Config::SetBase(Config::MAIN_AUTO_DISC_CHANGE,
                    GetBool("dolphin_auto_disc_change",
                            Config::Get(Config::MAIN_AUTO_DISC_CHANGE)));
    Config::SetBase(Config::MAIN_ALLOW_SD_WRITES,
                    GetBool("dolphin_allow_sd_writes",
                            Config::Get(Config::MAIN_ALLOW_SD_WRITES)));
    Config::SetBase(Config::MAIN_FAST_DISC_SPEED,
                    GetBool("dolphin_fast_disc_speed", Config::Get(Config::MAIN_FAST_DISC_SPEED)));
    Config::SetBase(Config::MAIN_RUSH_FRAME_PRESENTATION,
                    GetBool("dolphin_rush_presentation",
                            Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION)));
    Config::SetBase(Config::MAIN_SMOOTH_EARLY_PRESENTATION,
                    GetBool("dolphin_early_presentation",
                            Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION)));

    const int language =
        std::clamp(GetInt("dolphin_language", static_cast<int>(Config::Get(Config::SYSCONF_LANGUAGE))),
                   0, 9);
    Config::SetBase(Config::SYSCONF_LANGUAGE, static_cast<u32>(language));
    Config::SetBase(Config::MAIN_GC_LANGUAGE,
                    DiscIO::ToGameCubeLanguage(static_cast<DiscIO::Language>(language)));

    Config::SetBase(Config::MAIN_DSP_CAPTURE_LOG,
                    GetBool("dolphin_dsp_capture_log",
                            Config::Get(Config::MAIN_DSP_CAPTURE_LOG)));
    Config::SetBase(Config::MAIN_DSP_JIT,
                    GetBool("dolphin_dsp_jit", Config::Get(Config::MAIN_DSP_JIT)));
    Config::SetBase(Config::MAIN_DPL2_DECODER,
                    GetBool("dolphin_dpl2_decoder", Config::Get(Config::MAIN_DPL2_DECODER)));
    Config::SetBase(Config::MAIN_AUDIO_LATENCY,
                    GetInt("dolphin_audio_latency", Config::Get(Config::MAIN_AUDIO_LATENCY)));
    Config::SetBase(Config::MAIN_AUDIO_BUFFER_SIZE,
                    GetInt("dolphin_audio_buffer_size",
                           Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE)));
    Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS,
                    GetBool("dolphin_audio_fill_gaps",
                            Config::Get(Config::MAIN_AUDIO_FILL_GAPS)));
    Config::SetBase(Config::MAIN_AUDIO_VOLUME,
                    GetInt("dolphin_audio_volume", Config::Get(Config::MAIN_AUDIO_VOLUME)));
    Config::SetBase(Config::MAIN_AUDIO_MUTED,
                    GetBool("dolphin_audio_muted", Config::Get(Config::MAIN_AUDIO_MUTED)));
    Config::SetBase(
        Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT,
        GetBool("dolphin_audio_mute_on_disabled_speed_limit",
                Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT)));

    Config::SetBase(Config::MAIN_OSD_MESSAGES,
                    GetBool("dolphin_osd_enabled", Config::Get(Config::MAIN_OSD_MESSAGES)));
    Config::SetBase(Config::MAIN_ENABLE_DEBUGGING,
                    GetBool("dolphin_debug_mode_enabled",
                            Config::Get(Config::MAIN_ENABLE_DEBUGGING)));
    Common::Log::LogManager::GetInstance()->SetConfigLogLevel(
        ParseLogLevel(GetString("dolphin_log_level", "1"),
                      Config::Get(Common::Log::LOGGER_VERBOSITY)));

    Config::SetBase(Config::MAIN_SERIAL_PORT_1,
                    ParseSP1Device(GetString("dolphin_gc_sp1", "255"),
                                   Config::Get(Config::MAIN_SERIAL_PORT_1)));
    if (GetBool("dolphin_enable_gamecube_mic", false))
      Config::SetBase(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::Microphone);
    Config::SetBase(Config::SYSCONF_WIDESCREEN,
                    GetBool("dolphin_widescreen", Config::Get(Config::SYSCONF_WIDESCREEN)));
    Config::SetBase(Config::SYSCONF_PROGRESSIVE_SCAN,
                    GetBool("dolphin_progressive_scan",
                            Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN)));
    Config::SetBase(Config::SYSCONF_PAL60,
                    GetBool("dolphin_pal60", Config::Get(Config::SYSCONF_PAL60)));
    Config::SetBase(Config::SYSCONF_SENSOR_BAR_POSITION,
                    static_cast<u32>(std::clamp(
                        GetInt("dolphin_sensor_bar_position",
                               static_cast<int>(Config::Get(Config::SYSCONF_SENSOR_BAR_POSITION))),
                        0, 1)));
    Config::SetBase(Config::SYSCONF_WIIMOTE_MOTOR,
                    GetBool("dolphin_enable_rumble", Config::Get(Config::SYSCONF_WIIMOTE_MOTOR)));
    Config::SetBase(
        Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
        GetBool("dolphin_wiimote_continuous_scanning",
                Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING)));
    Config::SetBase(Config::MAIN_EMULATE_WII_SPEAK,
                    GetBool("dolphin_wiispeak_enable",
                            Config::Get(Config::MAIN_EMULATE_WII_SPEAK)));
    Config::SetBase(Config::MAIN_WII_SPEAK_MUTED,
                    GetBool("dolphin_wiispeak_muted",
                            Config::Get(Config::MAIN_WII_SPEAK_MUTED)));
    Config::SetBase(Config::MAIN_EMULATE_LOGITECH_MIC[0],
                    GetBool("dolphin_wii_logi_microphone_enable",
                            Config::Get(Config::MAIN_EMULATE_LOGITECH_MIC[0])));

    Config::SetBase(Config::GFX_VSYNC, GetBool("dolphin_vsync", Config::Get(Config::GFX_VSYNC)));
    Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                    GetBool("dolphin_widescreen_hack",
                            Config::Get(Config::GFX_WIDESCREEN_HACK)));
    ApplyViewportOverrides();
    Config::SetBase(
        Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH,
        GetInt("dolphin_custom_aspect_ratio_width",
               Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH)));
    Config::SetBase(
        Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT,
        GetInt("dolphin_custom_aspect_ratio_height",
               Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT)));
    Config::SetBase(Config::GFX_CROP,
                    GetBool("dolphin_crop_overscan", Config::Get(Config::GFX_CROP)));
    Config::SetBase(Config::GFX_SHOW_FPS,
                    GetBool("dolphin_show_fps", Config::Get(Config::GFX_SHOW_FPS)));
    Config::SetBase(Config::GFX_SHOW_FTIMES,
                    GetBool("dolphin_show_ftimes", Config::Get(Config::GFX_SHOW_FTIMES)));
    Config::SetBase(Config::GFX_SHOW_VPS,
                    GetBool("dolphin_show_vps", Config::Get(Config::GFX_SHOW_VPS)));
    Config::SetBase(Config::GFX_SHOW_VTIMES,
                    GetBool("dolphin_show_vtimes", Config::Get(Config::GFX_SHOW_VTIMES)));
    Config::SetBase(Config::GFX_SHOW_GRAPHS,
                    GetBool("dolphin_show_graphs", Config::Get(Config::GFX_SHOW_GRAPHS)));
    Config::SetBase(Config::GFX_SHOW_SPEED,
                    GetBool("dolphin_show_speed", Config::Get(Config::GFX_SHOW_SPEED)));
    Config::SetBase(Config::GFX_SHOW_SPEED_COLORS,
                    GetBool("dolphin_show_speed_colors",
                            Config::Get(Config::GFX_SHOW_SPEED_COLORS)));
    Config::SetBase(Config::GFX_OVERLAY_STATS,
                    GetBool("dolphin_overlay_stats", Config::Get(Config::GFX_OVERLAY_STATS)));
    Config::SetBase(Config::GFX_OVERLAY_PROJ_STATS,
                    GetBool("dolphin_overlay_proj_stats",
                            Config::Get(Config::GFX_OVERLAY_PROJ_STATS)));
    Config::SetBase(Config::GFX_OVERLAY_SCISSOR_STATS,
                    GetBool("dolphin_overlay_scissor_stats",
                            Config::Get(Config::GFX_OVERLAY_SCISSOR_STATS)));
    Config::SetBase(
        Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
        GetInt("dolphin_texture_cache_accuracy",
               Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES)));
    Config::SetBase(Config::GFX_HIRES_TEXTURES,
                    GetBool("dolphin_load_custom_textures",
                            Config::Get(Config::GFX_HIRES_TEXTURES)));
    Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                    GetBool("dolphin_cache_custom_textures",
                            Config::Get(Config::GFX_CACHE_HIRES_TEXTURES)));
    Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                    GetBool("dolphin_gpu_texture_decoding",
                            Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING)));
    Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING,
                    GetBool("dolphin_pixel_lighting",
                            Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING)));
    Config::SetBase(Config::GFX_FAST_DEPTH_CALC,
                    GetBool("dolphin_fast_depth_calculation",
                            Config::Get(Config::GFX_FAST_DEPTH_CALC)));
    Config::SetBase(Config::GFX_DISABLE_FOG,
                    GetBool("dolphin_disable_fog", Config::Get(Config::GFX_DISABLE_FOG)));
    Config::SetBase(Config::GFX_EFB_SCALE,
                    GetInt("dolphin_efb_scale", Config::Get(Config::GFX_EFB_SCALE)));
    Config::SetBase(Config::GFX_BACKEND_MULTITHREADING,
                    GetBool("dolphin_backend_multithreading",
                            Config::Get(Config::GFX_BACKEND_MULTITHREADING)));
    Config::SetBase(
        Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL,
        GetInt("dolphin_command_buffer_execute_interval",
               Config::Get(Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL)));
    Config::SetBase(Config::GFX_SHADER_CACHE,
                    GetBool("dolphin_shader_cache", Config::Get(Config::GFX_SHADER_CACHE)));
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                    ParseShaderCompilationMode(GetString("dolphin_shader_compilation_mode", "2"),
                                               Config::Get(Config::GFX_SHADER_COMPILATION_MODE)));
    Config::SetBase(
        Config::GFX_SHADER_COMPILER_THREADS,
        GetInt("dolphin_shader_compiler_threads",
               Config::Get(Config::GFX_SHADER_COMPILER_THREADS)));
    Config::SetBase(
        Config::GFX_SHADER_PRECOMPILER_THREADS,
        GetInt("dolphin_shader_precompiler_threads",
               Config::Get(Config::GFX_SHADER_PRECOMPILER_THREADS)));
    Config::SetBase(
        Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
        GetBool("dolphin_wait_for_shaders",
                Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING)));
    Config::SetBase(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE,
                    GetBool("dolphin_save_texture_cache_to_state",
                            Config::Get(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE)));
    Config::SetBase(Config::GFX_CPU_CULL,
                    GetBool("dolphin_cpu_cull", Config::Get(Config::GFX_CPU_CULL)));
    Config::SetBase(Config::GFX_ENABLE_VALIDATION_LAYER,
                    GetBool("dolphin_enable_validation_layer",
                            Config::Get(Config::GFX_ENABLE_VALIDATION_LAYER)));
    Config::SetBase(Config::GFX_TEXFMT_OVERLAY_ENABLE,
                    GetBool("dolphin_texfmt_overlay_enable",
                            Config::Get(Config::GFX_TEXFMT_OVERLAY_ENABLE)));
    Config::SetBase(Config::GFX_TEXFMT_OVERLAY_CENTER,
                    GetBool("dolphin_texfmt_overlay_center",
                            Config::Get(Config::GFX_TEXFMT_OVERLAY_CENTER)));
    Config::SetBase(Config::GFX_ENABLE_WIREFRAME,
                    GetBool("dolphin_enable_wireframe",
                            Config::Get(Config::GFX_ENABLE_WIREFRAME)));
    Config::SetBase(Config::GFX_MODS_ENABLE,
                    GetBool("dolphin_mods_enable", Config::Get(Config::GFX_MODS_ENABLE)));

    const int anti_aliasing = GetInt("dolphin_anti_aliasing", 0);
    switch (anti_aliasing)
    {
    case 1:
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 2:
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 3:
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 4:
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 5:
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 6:
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    default:
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    }

    Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                    ParseTextureFilteringMode(
                        GetString("dolphin_force_texture_filtering_mode", "0"),
                        Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING)));
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                    ParseAnisotropy(GetString("dolphin_max_anisotropy", "0"),
                                    Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY)));
    Config::SetBase(
        Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
        ParseOutputResampling(GetString("dolphin_enhance_output_resampling", "0"),
                              Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING)));
    Config::SetBase(Config::GFX_ENHANCE_POST_SHADER,
                    GetString("dolphin_post_shader",
                              Config::Get(Config::GFX_ENHANCE_POST_SHADER)));
    Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR,
                    GetBool("dolphin_force_true_color",
                            Config::Get(Config::GFX_ENHANCE_FORCE_TRUE_COLOR)));
    Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
                    GetBool("dolphin_disable_copy_filter",
                            Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER)));
    Config::SetBase(Config::GFX_ENHANCE_HDR_OUTPUT,
                    GetBool("dolphin_enhance_hdr_output",
                            Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT)));
    Config::SetBase(
        Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
        GetBool("dolphin_mipmap_detection",
                Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION)));
    Config::SetBase(
        Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD,
        static_cast<float>(GetDouble("dolphin_mipmap_detection_threshold",
                                     Config::Get(
                                         Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD))));

    Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE,
                    GetBool("dolphin_efb_access_enable",
                            Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE)));
    Config::SetBase(Config::GFX_HACK_EFB_DEFER_INVALIDATION,
                    GetBool("dolphin_efb_access_defer_invalidation",
                            Config::Get(Config::GFX_HACK_EFB_DEFER_INVALIDATION)));
    Config::SetBase(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE,
                    GetInt("dolphin_efb_access_tile_size",
                           Config::Get(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE)));
    Config::SetBase(Config::GFX_HACK_BBOX_ENABLE,
                    GetBool("dolphin_bbox_enabled", Config::Get(Config::GFX_HACK_BBOX_ENABLE)));
    Config::SetBase(
        Config::GFX_HACK_FORCE_PROGRESSIVE,
        GetBool("dolphin_force_progressive", Config::Get(Config::GFX_HACK_FORCE_PROGRESSIVE)));
    Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                    GetBool("dolphin_efb_to_texture",
                            Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM)));
    Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                    GetBool("dolphin_xfb_to_texture_enable",
                            Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM)));
    Config::SetBase(Config::GFX_HACK_DISABLE_COPY_TO_VRAM,
                    GetBool("dolphin_efb_to_vram",
                            Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM)));
    Config::SetBase(Config::GFX_HACK_DEFER_EFB_COPIES,
                    GetBool("dolphin_defer_efb_copies",
                            Config::Get(Config::GFX_HACK_DEFER_EFB_COPIES)));
    Config::SetBase(Config::GFX_HACK_IMMEDIATE_XFB,
                    GetBool("dolphin_immediate_xfb",
                            Config::Get(Config::GFX_HACK_IMMEDIATE_XFB)));
    Config::SetBase(Config::GFX_HACK_CAP_IMMEDIATE_XFB,
                    GetBool("dolphin_cap_immediate_xfb",
                            Config::Get(Config::GFX_HACK_CAP_IMMEDIATE_XFB)));
    Config::SetBase(Config::GFX_HACK_SKIP_DUPLICATE_XFBS,
                    GetBool("dolphin_skip_dupe_frames",
                            Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS)));
    Config::SetBase(Config::GFX_HACK_EARLY_XFB_OUTPUT,
                    GetBool("dolphin_early_xfb_output",
                            Config::Get(Config::GFX_HACK_EARLY_XFB_OUTPUT)));
    Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED,
                    GetBool("dolphin_efb_scaled_copy",
                            Config::Get(Config::GFX_HACK_COPY_EFB_SCALED)));
    Config::SetBase(
        Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES,
        GetBool("dolphin_efb_emulate_format_changes",
                Config::Get(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES)));
    Config::SetBase(Config::GFX_HACK_VERTEX_ROUNDING,
                    GetBool("dolphin_vertex_rounding",
                            Config::Get(Config::GFX_HACK_VERTEX_ROUNDING)));
    {
      const std::string vi_skip = GetString("dolphin_vi_skip", "");
      if (!vi_skip.empty() && ToLowerAscii(TrimAscii(vi_skip)) != "auto")
      {
        Config::SetBase(Config::GFX_HACK_VI_SKIP,
                        GetBool("dolphin_vi_skip", Config::Get(Config::GFX_HACK_VI_SKIP)));
      }
    }
    Config::SetBase(Config::GFX_HACK_FAST_TEXTURE_SAMPLING,
                    GetBool("dolphin_fast_texture_sampling",
                            Config::Get(Config::GFX_HACK_FAST_TEXTURE_SAMPLING)));
    Config::SetBase(Config::GFX_HACK_MISSING_COLOR_VALUE,
                    static_cast<u32>(GetInt("dolphin_missing_color_value",
                                            static_cast<int>(
                                                Config::Get(Config::GFX_HACK_MISSING_COLOR_VALUE)))));
    Config::SetBase(Config::GFX_PERF_QUERIES_ENABLE,
                    GetBool("dolphin_perf_queries_enable",
                            Config::Get(Config::GFX_PERF_QUERIES_ENABLE)));

    Config::SetBase(Config::SESSION_USE_FMA,
                    GetBool("dolphin_session_use_fma", Config::Get(Config::SESSION_USE_FMA)));
    Config::SetBase(Config::SESSION_LOAD_IPL_DUMP,
                    GetBool("dolphin_session_load_ipl_dump",
                            Config::Get(Config::SESSION_LOAD_IPL_DUMP)));
    Config::SetBase(
        Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY,
        GetBool("dolphin_session_gci_folder_current_game_only",
                Config::Get(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY)));
    Config::SetBase(
        Config::SESSION_CODE_SYNC_OVERRIDE,
        GetBool("dolphin_session_code_sync_override",
                Config::Get(Config::SESSION_CODE_SYNC_OVERRIDE)));
    Config::SetBase(
        Config::SESSION_SAVE_DATA_WRITABLE,
        GetBool("dolphin_session_save_data_writable",
                Config::Get(Config::SESSION_SAVE_DATA_WRITABLE)));
    Config::SetBase(
        Config::SESSION_SHOULD_FAKE_ERROR_001,
        GetBool("dolphin_session_should_fake_error_001",
                Config::Get(Config::SESSION_SHOULD_FAKE_ERROR_001)));
  }

  bool m_loaded = false;
  OptionMap m_options;
  std::string m_loaded_path;
  std::string m_save_path = std::string(kDefaultWritableConfigPath);
};

ConfigManager& GetManager()
{
  static ConfigManager manager;
  return manager;
}

}  // namespace

void ReloadConfig()
{
  GetManager().ReloadConfig();
}

std::string GetConfigValue(std::string_view key, std::string_view default_value)
{
  return GetManager().GetConfigValue(key, default_value);
}

void SetConfigValue(const std::string& key, const std::string& value)
{
  GetManager().SetConfigValue(key, value);
}

bool SaveConfig()
{
  return GetManager().SaveConfig();
}

void ApplyConfig(bool is_gamecube_disc)
{
  GetManager().ApplyConfig(is_gamecube_disc);
}

std::string GetLoadedConfigPath()
{
  return GetManager().GetLoadedConfigPath();
}

std::size_t GetLoadedOptionCount()
{
  return GetManager().GetLoadedOptionCount();
}

}  // namespace DolphinNX::TicoCore
