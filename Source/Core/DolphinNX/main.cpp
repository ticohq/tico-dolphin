// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdarg>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>

#include <switch.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "AudioCommon/AudioCommon.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Resources/CustomResourceManager.h"

#include "DolphinNX/Audio.h"
#include "DolphinNX/Input.h"
#include "DolphinNX/Overlay/Overlay.h"
#include "DolphinNX/TicoCore.h"
#include "DolphinNX/Overlay/VulkanOverlay.h"

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}

bool s_running = true;
static bool s_chainload_to_tico = false;
static std::atomic<u64> s_presented_frames{0};

static std::thread s_state_load_thread;
static std::atomic<bool> s_state_load_in_progress{false};

static std::optional<std::string> GetLaunchRomPath(int argc, char* argv[])
{
  for (int i = 1; i < argc; ++i)
  {
    if (argv[i] == nullptr || argv[i][0] == '\0')
      continue;

    const std::string_view arg(argv[i]);
    if (arg == "--tico-rom")
    {
      if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '\0')
        return std::string(argv[i + 1]);
      return std::nullopt;
    }

    if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-')
      continue;

    return std::string(arg);
  }

  return std::nullopt;
}

static void LOG(const char* fmt, ...);

struct BootGameMetadata
{
  std::string game_id;
  u16 revision = 0;
  DiscIO::Region region = DiscIO::Region::Unknown;
  DiscIO::Platform platform = DiscIO::Platform::NumberOfPlatforms;
};

static std::optional<BootGameMetadata> DetectBootGameMetadata(const std::string& rom_path)
{
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(rom_path);
  if (!volume)
    return std::nullopt;

  BootGameMetadata metadata;
  metadata.game_id = volume->GetGameID();
  metadata.revision = volume->GetRevision().value_or(0);
  metadata.region = volume->GetRegion();
  metadata.platform = volume->GetVolumeType();

  if (metadata.game_id.empty())
    return std::nullopt;

  return metadata;
}

static std::string DeriveDisplayTitleFromRomPath(std::string_view rom_path)
{
  if (rom_path.empty())
    return {};

  size_t filename_start = rom_path.find_last_of("/\\");
  if (filename_start == std::string_view::npos)
    filename_start = 0;
  else
    ++filename_start;

  std::string filename(rom_path.substr(filename_start));
  std::string title = filename;

  const size_t extension_start = title.find_last_of('.');
  if (extension_start != std::string::npos)
    title.resize(extension_start);

  std::string result;
  result.reserve(title.size());

  int paren_depth = 0;
  int bracket_depth = 0;
  bool last_was_space = false;

  for (const char c : title)
  {
    if (c == '(')
    {
      ++paren_depth;
      continue;
    }
    if (c == ')')
    {
      if (paren_depth > 0)
        --paren_depth;
      continue;
    }
    if (c == '[')
    {
      ++bracket_depth;
      continue;
    }
    if (c == ']')
    {
      if (bracket_depth > 0)
        --bracket_depth;
      continue;
    }

    if (paren_depth != 0 || bracket_depth != 0)
      continue;

    if (c == ' ' || c == '_')
    {
      if (!result.empty() && !last_was_space)
      {
        result.push_back(' ');
        last_was_space = true;
      }
      continue;
    }

    result.push_back(c);
    last_was_space = false;
  }

  const size_t first_non_space = result.find_first_not_of(" \t\r\n");
  if (first_non_space == std::string::npos)
    return {};

  const size_t last_non_space = result.find_last_not_of(" \t\r\n");
  return result.substr(first_non_space, last_non_space - first_non_space + 1);
}

static NWindow* s_nwindow = nullptr;
static u8 s_last_operation_mode = 0xFF;
static bool s_switch_clock_service_initialized = false;
static bool s_switch_clock_uses_clkrst = false;
static bool s_switch_clock_restore_cpu = false;
static bool s_switch_clock_restore_gpu = false;
static u32 s_switch_original_cpu_hz = 0;
static u32 s_switch_original_gpu_hz = 0;

static constexpr u32 kSwitchCpuClockHz = 1785000000;
static constexpr u32 kSwitchGpuClockHz = 768000000;

static constexpr PcvModule GetSwitchClockModule(bool cpu)
{
  return cpu ? PcvModule_CpuBus : PcvModule_GPU;
}

static constexpr PcvModuleId GetSwitchClockModuleId(bool cpu)
{
  return cpu ? PcvModuleId_CpuBus : PcvModuleId_GPU;
}

static bool InitializeSwitchClockService()
{
  if (s_switch_clock_service_initialized)
    return true;

  const bool use_clkrst = hosversionAtLeast(8, 0, 0);
  const Result rc = use_clkrst ? clkrstInitialize() : pcvInitialize();
  if (R_FAILED(rc))
  {
    LOG("Switch clock service init failed (%s) rc=0x%x\n", use_clkrst ? "clkrst" : "pcv",
        static_cast<unsigned>(rc));
    return false;
  }

  s_switch_clock_service_initialized = true;
  s_switch_clock_uses_clkrst = use_clkrst;
  LOG("Switch clock service initialized via %s\n", use_clkrst ? "clkrst" : "pcv");
  return true;
}

static void ShutdownSwitchClockService()
{
  if (!s_switch_clock_service_initialized)
    return;

  if (s_switch_clock_uses_clkrst)
    clkrstExit();
  else
    pcvExit();

  s_switch_clock_service_initialized = false;
}

static bool GetSwitchClockRate(bool cpu, u32* out_hz)
{
  if (out_hz == nullptr || !InitializeSwitchClockService())
    return false;

  Result rc = 0;
  if (s_switch_clock_uses_clkrst)
  {
    ClkrstSession session = {};
    rc = clkrstOpenSession(&session, GetSwitchClockModuleId(cpu), 3);
    if (R_FAILED(rc))
    {
      LOG("clkrstOpenSession(%s) failed rc=0x%x\n", cpu ? "cpu" : "gpu",
          static_cast<unsigned>(rc));
      return false;
    }

    rc = clkrstGetClockRate(&session, out_hz);
    clkrstCloseSession(&session);
  }
  else
  {
    rc = pcvGetClockRate(GetSwitchClockModule(cpu), out_hz);
  }

  if (R_FAILED(rc))
  {
    LOG("GetSwitchClockRate(%s) failed rc=0x%x\n", cpu ? "cpu" : "gpu",
        static_cast<unsigned>(rc));
    return false;
  }

  return true;
}

static bool SetSwitchClockRate(bool cpu, u32 hz)
{
  if (!InitializeSwitchClockService())
    return false;

  Result rc = 0;
  if (s_switch_clock_uses_clkrst)
  {
    ClkrstSession session = {};
    rc = clkrstOpenSession(&session, GetSwitchClockModuleId(cpu), 3);
    if (R_FAILED(rc))
    {
      LOG("clkrstOpenSession(%s) failed rc=0x%x\n", cpu ? "cpu" : "gpu",
          static_cast<unsigned>(rc));
      return false;
    }

    rc = clkrstSetClockRate(&session, hz);
    clkrstCloseSession(&session);
  }
  else
  {
    rc = pcvSetClockRate(GetSwitchClockModule(cpu), hz);
  }

  if (R_FAILED(rc))
  {
    LOG("SetSwitchClockRate(%s=%u) failed rc=0x%x\n", cpu ? "cpu" : "gpu", hz,
        static_cast<unsigned>(rc));
    return false;
  }

  return true;
}

static void ConfigureSwitchPerformance()
{
  if (GetSwitchClockRate(true, &s_switch_original_cpu_hz))
    s_switch_clock_restore_cpu = true;
  if (GetSwitchClockRate(false, &s_switch_original_gpu_hz))
    s_switch_clock_restore_gpu = true;

  const bool cpu_ok = SetSwitchClockRate(true, kSwitchCpuClockHz);
  const bool gpu_ok = SetSwitchClockRate(false, kSwitchGpuClockHz);

  LOG("Switch clocks target cpu=%u gpu=%u (saved cpu=%u gpu=%u, applied cpu=%d gpu=%d)\n",
      kSwitchCpuClockHz, kSwitchGpuClockHz, s_switch_original_cpu_hz, s_switch_original_gpu_hz,
      cpu_ok, gpu_ok);
}

static void RestoreSwitchPerformance()
{
  if (s_switch_clock_restore_gpu)
  {
    const bool restored = SetSwitchClockRate(false, s_switch_original_gpu_hz);
    LOG("Restore GPU clock %u -> %d\n", s_switch_original_gpu_hz, restored);
  }

  if (s_switch_clock_restore_cpu)
  {
    const bool restored = SetSwitchClockRate(true, s_switch_original_cpu_hz);
    LOG("Restore CPU clock %u -> %d\n", s_switch_original_cpu_hz, restored);
  }

  ShutdownSwitchClockService();
}

static void UpdateWindowModeAndCrop()
{
  if (!s_nwindow)
    return;

  const u8 operation_mode = appletGetOperationMode();
  if (operation_mode == s_last_operation_mode)
    return;

  s_last_operation_mode = operation_mode;

  if (operation_mode == AppletOperationMode_Handheld)
  {
    const Result dim_rc = nwindowSetDimensions(s_nwindow, 1280, 720);
    const Result crop_rc = nwindowSetCrop(s_nwindow, 0, 0, 1280, 720);
    LOG("Operation mode changed: Handheld (size=1280x720 rc=0x%x crop=0,0 1280x720 "
        "rc=0x%x)\n",
        static_cast<unsigned>(dim_rc), static_cast<unsigned>(crop_rc));
  }
  else
  {
    const Result dim_rc = nwindowSetDimensions(s_nwindow, 1920, 1080);
    const Result crop_rc = nwindowSetCrop(s_nwindow, 0, 0, 1920, 1080);
    LOG("Operation mode changed: Docked (size=1920x1080 rc=0x%x crop=0,0 1920x1080 "
        "rc=0x%x)\n",
        static_cast<unsigned>(dim_rc), static_cast<unsigned>(crop_rc));
  }
}

static bool IsGameCubeDisc(const std::optional<BootGameMetadata>& metadata)
{
  return metadata && metadata->platform == DiscIO::Platform::GameCubeDisc;
}

static constexpr bool kNxLogEnabled = false;
static constexpr const char kNxLogPath[] = "sdmc:/dolphin-nx.log";
static std::mutex s_log_mutex;
static bool s_log_ready = false;

static void LOG(const char* fmt, ...)
{
  if (!kNxLogEnabled || !s_log_ready)
    return;

  std::lock_guard<std::mutex> guard(s_log_mutex);
  FILE* fp = fopen(kNxLogPath, "a");
  if (!fp)
    return;

  va_list args;
  va_start(args, fmt);
  vfprintf(fp, fmt, args);
  va_end(args);
  fflush(fp);
  fclose(fp);
}

static bool SwitchMsgAlertHandler(const char* caption, const char* text, bool, Common::MsgType)
{
  LOG("Suppressed alert: %s - %s\n", caption ? caption : "(null)", text ? text : "(null)");
  return true;
}

static void SetDefaultEnvIfUnset(const char* name, const char* value)
{
  const char* current = std::getenv(name);
  if (current && current[0] != '\0')
  {
    LOG("ENV keep %s=%s\n", name, current);
    return;
  }

  setenv(name, value, 0);
  LOG("ENV default %s=%s\n", name, value);
}

static void RequestChainloadBackToTico()
{
  if (!s_chainload_to_tico)
    LOG("Return-to-Tico requested\n");

  s_chainload_to_tico = true;
  s_running = false;
}

static void ConfigureNextLoadForTico()
{
  if (!s_chainload_to_tico)
    return;

  const char* primary_nro = "sdmc:/switch/tico.nro";
  const char* fallback_nro = "sdmc:/switch/tico/tico.nro";
  const char* target_nro = nullptr;

  struct stat st;
  if (stat(primary_nro, &st) == 0)
  {
    target_nro = primary_nro;
  }
  else if (stat(fallback_nro, &st) == 0)
  {
    target_nro = fallback_nro;
  }

  if (target_nro)
  {
    char args[512];
    std::snprintf(args, sizeof(args), "%s --resume", target_nro);
    envSetNextLoad(target_nro, args);
    LOG("Chainloading back to %s with args: %s\n", target_nro, args);
  }
  else
  {
    LOG("Tico chainload target not found; exiting normally\n");
  }

  if (std::remove("imgui.ini") == 0)
    LOG("Deleted imgui.ini before exit\n");
}

static int ExitSwitchFrontend(int exit_code)
{
  LOG("ExitSwitchFrontend(%d)\n", exit_code);
  ConfigureNextLoadForTico();
  RestoreSwitchPerformance();

  romfsExit();
  socketExit();
  appletUnlockExit();
  return exit_code;
}

int main(int argc, char* argv[])
{
  appletLockExit();

  ConfigureSwitchPerformance();
  socketInitializeDefault();
  romfsInit();

  if (kNxLogEnabled)
  {
    if (FILE* fp = fopen(kNxLogPath, "w"))
    {
      fclose(fp);
      s_log_ready = true;
    }
  }

  LOG("=== Dolphin NX Standalone Boot Log ===\n");
  LOG("argc=%d\n", argc);

  const int exit_code = [&]() {
    const auto launch_rom_path = GetLaunchRomPath(argc, argv);
    if (!launch_rom_path)
    {
      LOG("Standalone launch rejected: missing ROM path\n");
      return 1;
    }

    const std::string rom_path = *launch_rom_path;
    LOG("Launch ROM: %s\n", rom_path.c_str());

    const auto boot_game_metadata = DetectBootGameMetadata(rom_path);
    if (boot_game_metadata)
    {
      LOG("Detected game: id=%s rev=%u region=%d platform=%d\n",
          boot_game_metadata->game_id.c_str(), boot_game_metadata->revision,
          static_cast<int>(boot_game_metadata->region),
          static_cast<int>(boot_game_metadata->platform));
    }
    else
    {
      LOG("Could not detect game metadata before boot\n");
    }

    std::string display_title = DeriveDisplayTitleFromRomPath(rom_path);
    if (display_title.empty() && boot_game_metadata)
      display_title = boot_game_metadata->game_id;
    LOG("Overlay title: %s\n", display_title.empty() ? "(empty)" : display_title.c_str());
    DolphinNX::OverlayUI::SetGameTitle(display_title);

    setenv("HOME", "sdmc:/tico/system/gc", 1);
    SetDefaultEnvIfUnset("MESA_VK_WSI_PRESENT_MODE", "fifo");
    LOG("Environment set\n");

    s_nwindow = nwindowGetDefault();
    LOG("NWindow: %p\n", (void*)s_nwindow);
    nwindowSetSwapInterval(s_nwindow, 1); 
    UpdateWindowModeAndCrop();

    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::Switch;
    wsi.display_connection = (void*)s_nwindow;
    wsi.render_window = (void*)s_nwindow;
    wsi.render_surface = (void*)s_nwindow;
    LOG("WSI configured (type=Switch, window=%p, surface=%p)\n", wsi.render_window,
        wsi.render_surface);

    const std::string user_dir = "sdmc:/tico/system/gc/User";
    const std::string sys_dir = "sdmc:/tico/system/gc/Sys";

    LOG("SetSysDirectory: %s\n", sys_dir.c_str());
    File::SetSysDirectory(sys_dir);
    LOG("SetUserDirectory: %s\n", user_dir.c_str());
    UICommon::SetUserDirectory(user_dir);
    LOG("CreateDirectories...\n");
    UICommon::CreateDirectories();
    LOG("UICommon::Init...\n");
    UICommon::Init();
    Common::SetEnableAlert(false);
    Common::SetAbortOnPanicAlert(false);
    Common::RegisterMsgAlertHandler(SwitchMsgAlertHandler);
    LOG("UICommon init complete; panic alerts redirected to log\n");

    Common::ScopeGuard ui_guard([] { UICommon::Shutdown(); });

    LOG("TicoCore::ReloadConfig...\n");
    DolphinNX::TicoCore::ReloadConfig();
    const std::string loaded_config_path = DolphinNX::TicoCore::GetLoadedConfigPath();
    LOG("TicoCore loaded %zu options from %s\n", DolphinNX::TicoCore::GetLoadedOptionCount(),
        loaded_config_path.empty() ? "(built-in defaults)" : loaded_config_path.c_str());

    LOG("TicoCore::ApplyConfig...\n");
    DolphinNX::TicoCore::ApplyConfig(IsGameCubeDisc(boot_game_metadata));
    LOG("Config applied\n");
    LOG("Configured GFX backend=%s, CPUThread=%d, Fastmem=%d, FastmemArena=%d, "
        "LargeEntryMap=%d, AudioBuffer=%d, PrecisionTiming=%d, SyncOnSkipIdle=%d, VSync=%d, "
        "BackendMT=%d, ForceProgressive=%d, VISkip=%d, EarlyXFB=%d, ImmediateXFB=%d, "
        "SkipDuplicateXFBs=%d, SkipXFBCopyToRam=%d, DeferEFBCopies=%d, Stereo=%d, "
        "DisableCopyToVRAM=%d, GPUTextureDecoding=%d\n",
        Config::Get(Config::MAIN_GFX_BACKEND).c_str(), Config::Get(Config::MAIN_CPU_THREAD),
        Config::Get(Config::MAIN_FASTMEM), Config::Get(Config::MAIN_FASTMEM_ARENA),
        Config::Get(Config::MAIN_LARGE_ENTRY_POINTS_MAP),
        Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE),
        Config::Get(Config::MAIN_PRECISION_FRAME_TIMING),
        Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE), Config::Get(Config::GFX_VSYNC),
        Config::Get(Config::GFX_BACKEND_MULTITHREADING),
        Config::Get(Config::GFX_HACK_FORCE_PROGRESSIVE),
        Config::Get(Config::GFX_HACK_VI_SKIP), Config::Get(Config::GFX_HACK_EARLY_XFB_OUTPUT),
        Config::Get(Config::GFX_HACK_IMMEDIATE_XFB),
        Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS),
        Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
        Config::Get(Config::GFX_HACK_DEFER_EFB_COPIES),
        static_cast<int>(Config::Get(Config::GFX_STEREO_MODE)),
        Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM),
        Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING));

    LOG("PopulateBackendInfo...\n");
    VideoBackendBase::PopulateBackendInfo(wsi);
    LOG("Backend info populated\n");

    LOG("GenerateFromFile: %s\n", rom_path.c_str());
    auto boot = BootParameters::GenerateFromFile(
        rom_path, BootSessionData(std::nullopt, DeleteSavestateAfterBoot::No));

    if (!boot)
    {
      LOG("FATAL: Failed to create boot parameters for: %s\n", rom_path.c_str());
      return 1;
    }
    LOG("Boot parameters created OK\n");

    LOG("Input::Init...\n");
    DolphinNX::Input::Init(wsi);
    Common::ScopeGuard input_guard([] { DolphinNX::Input::Shutdown(); });
    LOG("Input initialized\n");

    LOG("BootManager::BootCore...\n");
    auto& system = Core::System::GetInstance();
    if (!BootManager::BootCore(system, std::move(boot), wsi))
    {
      LOG("FATAL: BootCore failed for: %s\n", rom_path.c_str());
      return 1;
    }
    LOG("BootCore succeeded, emulation starting\n");
    if (g_video_backend)
      LOG("Active video backend: %s\n", g_video_backend->GetDisplayName().c_str());
    LOG("Core state: IsRunning=%d, IsRunningOrStarting=%d\n",
        Core::IsRunning(system), Core::IsRunningOrStarting(system));

    const auto present_hook = GetVideoEvents().after_present_event.Register(
        [](const PresentInfo&) { s_presented_frames.fetch_add(1, std::memory_order_relaxed); });

    bool overlay_ok = false;
    LOG("Overlay init deferred until the first present to avoid early Vulkan races\n");

    for (int wait = 0; wait < 100; wait++)
    {
      if (Core::IsRunningOrStarting(system))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      LOG("Waiting for emu thread to start... (%d)\n", wait);
    }
    LOG("Post-wait state: IsRunning=%d, IsRunningOrStarting=%d\n",
        Core::IsRunning(system), Core::IsRunningOrStarting(system));

    Common::SetCurrentThreadAffinity(2);
    LOG("Main thread pinned to core 2\n");
    LOG("Entering main loop\n");

    int frame = 0;
    bool overlay_paused_core = false;
    bool overlay_was_visible = false;
    const auto startup_started_at = std::chrono::steady_clock::now();
    bool startup_watchdog_fired = false;
    bool startup_abort_due_no_present = false;
    while (appletMainLoop() && s_running &&
           (Core::IsRunning(system) || Core::IsRunningOrStarting(system)))
    {
      if (s_state_load_in_progress.load(std::memory_order_acquire))
      {
        Common::SleepCurrentThread(8);
        continue;
      }

      UpdateWindowModeAndCrop();
      DolphinNX::Input::Update();

      if (!overlay_ok)
      {
        const u64 presented_frames = s_presented_frames.load(std::memory_order_relaxed);
        if (presented_frames > 0)
        {
          overlay_ok = DolphinNX::VulkanOverlay::Init();
          if (overlay_ok)
          {
            LOG("Overlay init succeeded during main loop after %llu presents\n",
                static_cast<unsigned long long>(presented_frames));
          }
        }
      }

      if (!startup_watchdog_fired &&
          s_presented_frames.load(std::memory_order_relaxed) == 0 &&
          Core::IsRunningOrStarting(system) &&
          std::chrono::steady_clock::now() - startup_started_at > std::chrono::seconds(12))
      {
        startup_watchdog_fired = true;
        startup_abort_due_no_present = true;
        LOG("Startup watchdog fired: still at 0 presents after 12s (state=%d running=%d)\n",
            static_cast<int>(Core::GetState(system)), Core::IsRunning(system));
        RequestChainloadBackToTico();
      }

      if (overlay_ok)
      {
        DolphinNX::VulkanOverlay::Update(DolphinNX::Input::GetPad());
        const bool overlay_visible = DolphinNX::VulkanOverlay::IsVisible();
        const bool overlay_just_opened = overlay_visible && !overlay_was_visible;
        const bool overlay_just_closed = !overlay_visible && overlay_was_visible;
        bool just_paused_for_overlay = false;

        if (overlay_just_opened && Core::GetState(system) == Core::State::Running)
        {
          Core::SetState(system, Core::State::Paused);
          overlay_paused_core = true;
          just_paused_for_overlay = true;
          LOG("Overlay: paused emulation on open\n");
        }

        if (overlay_visible && overlay_paused_core && !just_paused_for_overlay &&
            Core::GetState(system) == Core::State::Paused && g_presenter)
        {
          g_presenter->Present();
        }

        if (const int action = DolphinNX::VulkanOverlay::ConsumeAction(); action != 0)
        {
          using A = DolphinNX::OverlayUI::Action;
          const A overlay_action = static_cast<A>(action);
          if (DolphinNX::OverlayUI::IsSaveStateAction(overlay_action))
          {
            const int slot = DolphinNX::OverlayUI::GetStateSlotForAction(overlay_action);
            State::Save(system, slot);
            LOG("Overlay: SaveState slot %d\n", slot);
          }
          else if (DolphinNX::OverlayUI::IsLoadStateAction(overlay_action))
          {
            const int slot = DolphinNX::OverlayUI::GetStateSlotForAction(overlay_action);
            if (!s_state_load_in_progress.exchange(true, std::memory_order_acq_rel))
            {
              if (s_state_load_thread.joinable())
                s_state_load_thread.join();
              AudioCommon::SetSoundStreamRunning(system, false);

              s_state_load_thread = std::thread([&system, slot]() {
                Common::SetCurrentThreadName("StateLoad - switchnx");
                Common::SetCurrentThreadAffinity(2);
                State::Load(system, slot);
                AudioCommon::SetSoundStreamRunning(system, true);
                s_state_load_in_progress.store(false, std::memory_order_release);
              });
              LOG("Overlay: LoadState slot %d (worker spawned)\n", slot);
            }
            else
            {
              LOG("Overlay: LoadState slot %d ignored (load already in progress)\n", slot);
            }
          }
          else
          {
            switch (overlay_action)
            {
            case A::Exit:
              LOG("Overlay: Exit requested\n");
              RequestChainloadBackToTico();
              break;
            case A::Resume:
            case A::None:
            default:
              break;
            }
          }
        }

        if (DolphinNX::VulkanOverlay::ShouldExit())
          RequestChainloadBackToTico();

        if (overlay_just_closed && overlay_paused_core)
        {
          if (s_running && Core::GetState(system) == Core::State::Paused)
          {
            Core::SetState(system, Core::State::Running);
            LOG("Overlay: resumed emulation on close\n");
          }
          overlay_paused_core = false;
        }

        overlay_was_visible = overlay_visible;
      }

      Core::HostDispatchJobs(system);

      Common::SleepCurrentThread(1);

      frame++;
    }

    const u64 presented_frames = s_presented_frames.load(std::memory_order_relaxed);
    const auto startup_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              startup_started_at)
            .count();

    LOG("Main loop exited (frame=%d, presented=%llu, s_running=%d, IsRunning=%d, IsStarting=%d, "
        "elapsed_ms=%lld)\n",
        frame, static_cast<unsigned long long>(presented_frames), s_running, Core::IsRunning(system),
        Core::IsRunningOrStarting(system), static_cast<long long>(startup_elapsed));

    if (startup_abort_due_no_present ||
        (presented_frames == 0 && startup_elapsed >= 5000 &&
         (Core::IsRunning(system) || Core::IsRunningOrStarting(system))))
    {
      LOG("Skipping Core::Stop/Core::Shutdown after a no-present startup stall to avoid exit "
          "deadlock\n");
      if (s_state_load_thread.joinable())
        s_state_load_thread.detach();
      RequestChainloadBackToTico();
      return 1;
    }

    if (s_state_load_thread.joinable())
    {
      LOG("Waiting for in-flight state load worker before shutdown...\n");
      s_state_load_thread.join();
    }

    LOG("VulkanOverlay::Shutdown...\n");
    DolphinNX::VulkanOverlay::Shutdown();

    LOG("Core::Stop...\n");
    Core::Stop(system);
#if defined(__LIBUSB__)
    LOG("System::ShutdownUSBScanner...\n");
    system.ShutdownUSBScanner();
#endif

    LOG("Core::Stop done, Core::Shutdown...\n");
    Core::Shutdown(system);

    if (g_video_backend)
    {
      LOG("Video backend final shutdown...\n");
      g_video_backend->Shutdown();
    }
    LOG("CustomResourceManager final shutdown...\n");
    system.GetCustomResourceManager().Shutdown();
    LOG("Fifo final shutdown...\n");
    system.GetFifo().Shutdown();
    LOG("Core::Shutdown done\n");

    LOG("=== Dolphin NX shutdown complete ===\n");
    return 0;
  }();

  return ExitSwitchFrontend(exit_code);
}
