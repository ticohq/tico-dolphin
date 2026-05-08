// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Input.h"

#include <switch.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "DolphinNX/Overlay/Overlay.h"
#include "DolphinNX/Overlay/VulkanOverlay.h"

namespace DolphinNX
{
namespace Input
{

static const std::string source = "Switch";
static constexpr unsigned MAX_SWITCH_PLAYERS = 4;
static constexpr int MAX_SIXAXIS_HANDLES = 2;
static constexpr std::array<HidNpadIdType, MAX_SWITCH_PLAYERS> s_player_npad_ids = {
    HidNpadIdType_No1,
    HidNpadIdType_No2,
    HidNpadIdType_No3,
    HidNpadIdType_No4,
};
static PadState s_pads[MAX_SWITCH_PLAYERS] = {};
static bool s_hid_initialized = false;
static bool s_pad_initialized = false;

struct SixAxisPlayerState
{
  bool started = false;
  HidSixAxisSensorHandle handles[MAX_SIXAXIS_HANDLES] = {};
  int handle_count = 0;
  HidSixAxisSensorState state = {};
  HidNpadIdType id = HidNpadIdType_No1;
  HidNpadStyleTag style = HidNpadStyleTag_NpadFullKey;
};

enum class WiiControllerMode
{
  GameCube,
  WiimoteVertical,
  WiimoteHorizontal,
  WiimoteNunchuk,
  WiimoteClassic,
};

struct WiiControllerPlayerState
{
  WiiControllerMode mode = WiiControllerMode::WiimoteNunchuk;
  bool toggle_held = false;
  bool mode_applied = false;
  bool gamecube_mode_applied = false;
};

static SixAxisPlayerState s_sixaxis[MAX_SWITCH_PLAYERS] = {};
static WiiControllerPlayerState s_wii_controller_states[MAX_SWITCH_PLAYERS] = {};

static bool ShouldBlockGameplayInput()
{
  return DolphinNX::VulkanOverlay::IsVisible();
}

static const char* GetWiiControllerModeName(WiiControllerMode mode)
{
  switch (mode)
  {
  case WiiControllerMode::GameCube:
    return "GameCube Controller";
  case WiiControllerMode::WiimoteVertical:
    return "Wii Remote Vertical";
  case WiiControllerMode::WiimoteHorizontal:
    return "Wii Remote Horizontal";
  case WiiControllerMode::WiimoteNunchuk:
    return "Wii Remote + Nunchuk";
  case WiiControllerMode::WiimoteClassic:
    return "Classic Controller";
  }

  return "Unknown";
}

static DolphinNX::OverlayUI::ToastCorner GetToastCornerForPlayer(unsigned player)
{
  switch (player)
  {
  case 0:
    return DolphinNX::OverlayUI::ToastCorner::TopLeft;
  case 1:
    return DolphinNX::OverlayUI::ToastCorner::TopRight;
  case 2:
    return DolphinNX::OverlayUI::ToastCorner::BottomLeft;
  case 3:
    return DolphinNX::OverlayUI::ToastCorner::BottomRight;
  default:
    return DolphinNX::OverlayUI::ToastCorner::TopLeft;
  }
}

static ControllerEmu::Attachments* GetWiimoteAttachments(WiimoteEmu::Wiimote* wiimote)
{
  return static_cast<ControllerEmu::Attachments*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
}

static void SetGameCubePortEnabled(Core::System& system, unsigned player, bool enabled)
{
  const SerialInterface::SIDevices device =
      enabled ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE;

  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), device);
  system.GetSerialInterface().ChangeDevice(Config::Get(Config::GetInfoForSIDevice(player)),
                                           player);

  if (auto* gc_pad = static_cast<GCPad*>(Pad::GetConfig()->GetController(player)))
    gc_pad->UpdateReferences(g_controller_interface);
}

static bool SetBoolSetting(ControllerEmu::ControlGroup* group, std::string_view name, bool value)
{
  if (!group)
    return false;

  for (const auto& setting : group->numeric_settings)
  {
    if (setting->GetType() != ControllerEmu::SettingType::Bool ||
        std::string_view(setting->GetININame()) != name)
    {
      continue;
    }

    static_cast<ControllerEmu::NumericSetting<bool>*>(setting.get())->SetValue(value);
    return true;
  }

  return false;
}

static void SetWiimoteOrientation(WiimoteEmu::Wiimote* wiimote, bool sideways, bool upright)
{
  auto* options = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Options);
  SetBoolSetting(options, WiimoteEmu::Wiimote::SIDEWAYS_OPTION, sideways);
  SetBoolSetting(options, WiimoteEmu::Wiimote::UPRIGHT_OPTION, upright);
}

static bool ControlExpressionEquals(const ControllerEmu::ControlGroup* group, size_t index,
                                    std::string_view expression);

static bool HasEmptyClassicControllerDefaults(WiimoteEmu::Wiimote* wiimote)
{
  const auto* buttons = wiimote->GetClassicGroup(WiimoteEmu::ClassicGroup::Buttons);
  const auto* triggers = wiimote->GetClassicGroup(WiimoteEmu::ClassicGroup::Triggers);
  const auto* left_stick = wiimote->GetClassicGroup(WiimoteEmu::ClassicGroup::LeftStick);
  const auto* right_stick = wiimote->GetClassicGroup(WiimoteEmu::ClassicGroup::RightStick);

  return ControlExpressionEquals(buttons, 0, "") &&
         ControlExpressionEquals(buttons, 1, "") &&
         ControlExpressionEquals(triggers, 0, "") &&
         ControlExpressionEquals(triggers, 1, "") &&
         ControlExpressionEquals(left_stick, 0, "") &&
         ControlExpressionEquals(right_stick, 0, "");
}

static bool EnsureClassicControllerDefaults(WiimoteEmu::Wiimote* wiimote)
{
  if (!HasEmptyClassicControllerDefaults(wiimote))
    return false;

  auto* attachments = GetWiimoteAttachments(wiimote);
  if (!attachments)
    return false;

  const auto& attachment_list = attachments->GetAttachmentList();
  const size_t classic_index = static_cast<size_t>(WiimoteEmu::ExtensionNumber::CLASSIC);
  if (classic_index >= attachment_list.size())
    return false;

  attachment_list[classic_index]->LoadDefaults();
  return true;
}

static void SetWiimoteEnabled(unsigned player, bool enabled, WiimoteEmu::ExtensionNumber extension,
                              bool sideways, bool upright)
{
  auto* wiimote = static_cast<WiimoteEmu::Wiimote*>(Wiimote::GetConfig()->GetController(player));
  if (!wiimote)
    return;

  if (auto* attachments = GetWiimoteAttachments(wiimote))
    attachments->SetSelectedAttachment(extension);
  const bool repaired_classic_defaults =
      extension == WiimoteEmu::ExtensionNumber::CLASSIC && EnsureClassicControllerDefaults(wiimote);
  SetWiimoteOrientation(wiimote, sideways, upright);

  const WiimoteSource wiimote_source = enabled ? WiimoteSource::Emulated : WiimoteSource::None;
  Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), wiimote_source);
  WiimoteCommon::OnSourceChanged(player, wiimote_source);
  wiimote->UpdateReferences(g_controller_interface);
  if (repaired_classic_defaults)
    Wiimote::GetConfig()->SaveConfig();
}

static bool ApplyWiiControllerMode(unsigned player, WiiControllerMode mode, bool show_toast)
{
  if (Pad::GetConfig()->ControllersNeedToBeCreated() ||
      Wiimote::GetConfig()->ControllersNeedToBeCreated())
  {
    return false;
  }

  Core::System& system = Core::System::GetInstance();

  switch (mode)
  {
  case WiiControllerMode::GameCube:
    SetWiimoteEnabled(player, false, WiimoteEmu::ExtensionNumber::NONE, false, false);
    SetGameCubePortEnabled(system, player, true);
    break;

  case WiiControllerMode::WiimoteVertical:
    SetGameCubePortEnabled(system, player, false);
    SetWiimoteEnabled(player, true, WiimoteEmu::ExtensionNumber::NONE, false, false);
    break;

  case WiiControllerMode::WiimoteHorizontal:
    SetGameCubePortEnabled(system, player, false);
    SetWiimoteEnabled(player, true, WiimoteEmu::ExtensionNumber::NONE, true, false);
    break;

  case WiiControllerMode::WiimoteNunchuk:
    SetGameCubePortEnabled(system, player, false);
    SetWiimoteEnabled(player, true, WiimoteEmu::ExtensionNumber::NUNCHUK, false, false);
    break;

  case WiiControllerMode::WiimoteClassic:
    SetGameCubePortEnabled(system, player, false);
    SetWiimoteEnabled(player, true, WiimoteEmu::ExtensionNumber::CLASSIC, false, false);
    break;
  }

  INFO_LOG_FMT(COMMON, "DolphinNX P{} Wii controller mode: {}", player + 1,
               GetWiiControllerModeName(mode));
  if (show_toast)
  {
    DolphinNX::OverlayUI::ShowToast(std::string("P") + std::to_string(player + 1) + " - " +
                                        GetWiiControllerModeName(mode),
                                    GetToastCornerForPlayer(player));
  }
  return true;
}

static bool ApplyGameCubeControllerMode(unsigned player)
{
  if (Pad::GetConfig()->ControllersNeedToBeCreated())
    return false;

  SetGameCubePortEnabled(Core::System::GetInstance(), player, true);
  return true;
}

static WiiControllerMode GetNextWiiControllerMode(WiiControllerMode mode)
{
  switch (mode)
  {
  case WiiControllerMode::GameCube:
    return WiiControllerMode::WiimoteVertical;
  case WiiControllerMode::WiimoteVertical:
    return WiiControllerMode::WiimoteHorizontal;
  case WiiControllerMode::WiimoteHorizontal:
    return WiiControllerMode::WiimoteNunchuk;
  case WiiControllerMode::WiimoteNunchuk:
    return WiiControllerMode::WiimoteClassic;
  case WiiControllerMode::WiimoteClassic:
    return WiiControllerMode::GameCube;
  }

  return WiiControllerMode::WiimoteNunchuk;
}

static void HandleWiiControllerModeToggle(unsigned player)
{
  WiiControllerPlayerState& player_state = s_wii_controller_states[player];
  Core::System& system = Core::System::GetInstance();
  if (!system.IsWii())
  {
    player_state.toggle_held = false;
    player_state.mode_applied = false;
    if (!player_state.gamecube_mode_applied)
      player_state.gamecube_mode_applied = ApplyGameCubeControllerMode(player);
    return;
  }

  player_state.gamecube_mode_applied = false;

  if (player != 0 && !padIsConnected(&s_pads[player]))
  {
    player_state.toggle_held = false;
    return;
  }

  if (!player_state.mode_applied)
  {
    player_state.mode_applied = ApplyWiiControllerMode(player, player_state.mode, false);
    if (!player_state.mode_applied)
      return;
  }

  if (ShouldBlockGameplayInput())
  {
    player_state.toggle_held = false;
    return;
  }

  const u64 held = padGetButtons(&s_pads[player]);
  const bool toggle_combo =
      (held & HidNpadButton_ZL) != 0 && (held & HidNpadButton_Minus) != 0;

  if (!toggle_combo)
  {
    player_state.toggle_held = false;
    return;
  }

  if (player_state.toggle_held)
    return;

  player_state.toggle_held = true;
  const WiiControllerMode next_mode = GetNextWiiControllerMode(player_state.mode);
  if (ApplyWiiControllerMode(player, next_mode, true))
    player_state.mode = next_mode;
}

class Device : public ciface::Core::Device
{
private:
  class Button : public ciface::Core::Device::Input
  {
  public:
    Button(unsigned port, u64 mask, const char* name) : m_port(port), m_mask(mask), m_name(name) {}
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      if (ShouldBlockGameplayInput())
        return 0.0;
      return (padGetButtons(&s_pads[m_port]) & m_mask) ? 1.0 : 0.0;
    }

  private:
    const unsigned m_port;
    const u64 m_mask;
    const char* m_name;
  };

  class Axis : public ciface::Core::Device::Input
  {
  public:
    Axis(unsigned port, int stick_index, int axis, s32 range, const char* name)
        : m_port(port), m_stick(stick_index), m_axis(axis), m_range(range), m_name(name)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      if (ShouldBlockGameplayInput())
        return 0.0;
      HidAnalogStickState pos = padGetStickPos(&s_pads[m_port], m_stick);
      s32 val = (m_axis == 0) ? pos.x : pos.y;
      return std::max(0.0, static_cast<double>(val) / m_range);
    }

  private:
    const unsigned m_port;
    const int m_stick;
    const int m_axis;
    const s32 m_range;
    const char* m_name;
  };

  class MotionAxis : public ciface::Core::Device::Input
  {
  public:
    enum class Sensor
    {
      Accelerometer,
      Gyroscope
    };

    enum class Axis
    {
      X,
      Y,
      Z
    };

    MotionAxis(unsigned port, Sensor sensor, Axis axis, double scale, const char* name)
        : m_port(port), m_sensor(sensor), m_axis(axis), m_scale(scale), m_name(name)
    {
    }

    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      if (ShouldBlockGameplayInput() || !s_sixaxis[m_port].started)
        return 0.0;

      const HidVector& vector = m_sensor == Sensor::Accelerometer ?
                                    s_sixaxis[m_port].state.acceleration :
                                    s_sixaxis[m_port].state.angular_velocity;

      float value = 0.0f;
      switch (m_axis)
      {
      case Axis::X:
        value = vector.x;
        break;
      case Axis::Y:
        value = vector.y;
        break;
      case Axis::Z:
        value = vector.z;
        break;
      }

      const double sensor_scale =
          m_sensor == Sensor::Accelerometer ? MathUtil::GRAVITY_ACCELERATION : 1.0;
      return std::max(0.0, static_cast<double>(value) * m_scale * sensor_scale);
    }

  private:
    const unsigned m_port;
    const Sensor m_sensor;
    const Axis m_axis;
    const double m_scale;
    const char* m_name;
  };

public:
  Device(unsigned port) : m_port(port)
  {
    AddInput(new Button(m_port, HidNpadButton_A, "A"));
    AddInput(new Button(m_port, HidNpadButton_B, "B"));
    AddInput(new Button(m_port, HidNpadButton_X, "X"));
    AddInput(new Button(m_port, HidNpadButton_Y, "Y"));
    AddInput(new Button(m_port, HidNpadButton_L, "L"));
    AddInput(new Button(m_port, HidNpadButton_R, "R"));
    AddInput(new Button(m_port, HidNpadButton_ZL, "Z"));
    AddInput(new Button(m_port, HidNpadButton_ZR, "R2"));
    AddInput(new Button(m_port, HidNpadButton_Plus, "Start"));
    AddInput(new Button(m_port, HidNpadButton_Minus, "Select"));
    AddInput(new Button(m_port, HidNpadButton_Up, "Up"));
    AddInput(new Button(m_port, HidNpadButton_Down, "Down"));
    AddInput(new Button(m_port, HidNpadButton_Left, "Left"));
    AddInput(new Button(m_port, HidNpadButton_Right, "Right"));
    AddInput(new Button(m_port, HidNpadButton_StickL, "L3"));
    AddInput(new Button(m_port, HidNpadButton_StickR, "R3"));

    // Left analog stick (main stick)
    AddInput(new Axis(m_port, 0, 0, -32768, "X0-"));
    AddInput(new Axis(m_port, 0, 0, 32767, "X0+"));
    AddInput(new Axis(m_port, 0, 1, -32768, "Y0-"));
    AddInput(new Axis(m_port, 0, 1, 32767, "Y0+"));

    // Right analog stick (C-stick)
    AddInput(new Axis(m_port, 1, 0, -32768, "X1-"));
    AddInput(new Axis(m_port, 1, 0, 32767, "X1+"));
    AddInput(new Axis(m_port, 1, 1, -32768, "Y1-"));
    AddInput(new Axis(m_port, 1, 1, 32767, "Y1+"));

    // Motion source for Wiimote IMU defaults.
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::Z, -1.0,
                            "Accel Up"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::Z, 1.0,
                            "Accel Down"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::X, 1.0,
                            "Accel Left"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::X, -1.0,
                            "Accel Right"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::Y, -1.0,
                            "Accel Forward"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Accelerometer, MotionAxis::Axis::Y, 1.0,
                            "Accel Backward"));

    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::X, 1.0,
                            "Gyro Pitch Up"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::X, -1.0,
                            "Gyro Pitch Down"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::Y, -1.0,
                            "Gyro Roll Left"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::Y, 1.0,
                            "Gyro Roll Right"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::Z, 1.0,
                            "Gyro Yaw Left"));
    AddInput(new MotionAxis(m_port, MotionAxis::Sensor::Gyroscope, MotionAxis::Axis::Z, -1.0,
                            "Gyro Yaw Right"));
  }

  ciface::Core::DeviceRemoval UpdateInput() override
  {
    return ciface::Core::DeviceRemoval::Keep;
  }

  std::string GetName() const override { return "Joypad"; }
  std::string GetSource() const override { return source; }

private:
  unsigned m_port;
};

class AnalogDevice : public ciface::Core::Device
{
private:
  class Axis : public ciface::Core::Device::Input
  {
  public:
    Axis(unsigned port, int stick_index, int axis, s32 range, const char* name)
        : m_port(port), m_stick(stick_index), m_axis(axis), m_range(range), m_name(name)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      if (ShouldBlockGameplayInput())
        return 0.0;
      HidAnalogStickState pos = padGetStickPos(&s_pads[m_port], m_stick);
      s32 val = (m_axis == 0) ? pos.x : pos.y;
      return std::max(0.0, static_cast<double>(val) / m_range);
    }

  private:
    const unsigned m_port;
    const int m_stick;
    const int m_axis;
    const s32 m_range;
    const char* m_name;
  };

public:
  AnalogDevice(unsigned port) : m_port(port)
  {
    AddInput(new Axis(m_port, 0, 0, -32768, "X0-"));
    AddInput(new Axis(m_port, 0, 0, 32767, "X0+"));
    AddInput(new Axis(m_port, 0, 1, -32768, "Y0-"));
    AddInput(new Axis(m_port, 0, 1, 32767, "Y0+"));
    AddInput(new Axis(m_port, 1, 0, -32768, "X1-"));
    AddInput(new Axis(m_port, 1, 0, 32767, "X1+"));
    AddInput(new Axis(m_port, 1, 1, -32768, "Y1-"));
    AddInput(new Axis(m_port, 1, 1, 32767, "Y1+"));
  }

  ciface::Core::DeviceRemoval UpdateInput() override
  {
    return ciface::Core::DeviceRemoval::Keep;
  }

  std::string GetName() const override { return "Analog"; }
  std::string GetSource() const override { return source; }

private:
  unsigned m_port;
};

static FILE* s_input_log = nullptr;
static u32 s_dual_joycon_assignment_update = 0;

static bool ControlExpressionEquals(const ControllerEmu::ControlGroup* group, size_t index,
                                    std::string_view expression)
{
  return group && index < group->controls.size() &&
         group->controls[index]->control_ref->GetExpression() == expression;
}

static bool DoubleSettingEquals(const ControllerEmu::ControlGroup* group, std::string_view name,
                                double value)
{
  if (!group)
    return false;

  for (const auto& setting : group->numeric_settings)
  {
    if (setting->GetType() != ControllerEmu::SettingType::Double ||
        std::string_view(setting->GetININame()) != name)
    {
      continue;
    }

    const auto* numeric_setting =
        static_cast<const ControllerEmu::NumericSetting<double>*>(setting.get());
    return numeric_setting->IsSimpleValue() &&
           std::abs(numeric_setting->GetValue() - value) < 0.01;
  }

  return false;
}

static ciface::Core::DeviceQualifier GetSwitchDefaultDevice(unsigned player)
{
  return ciface::Core::DeviceQualifier(source, static_cast<int>(player), "Joypad");
}

static bool IsWrongSwitchDefaultDevice(const ciface::Core::DeviceQualifier& device,
                                       unsigned player)
{
  return device.source != source || device.cid != static_cast<int>(player) ||
         device.name != "Joypad";
}

template <typename Controller>
static void SetSwitchDefaultDevice(Controller* controller, unsigned player)
{
  controller->SetDefaultDevice(GetSwitchDefaultDevice(player));
}

static bool HasLegacyKeyboardDefaults(GCPad* pad)
{
  const auto* buttons = pad->GetGroup(PadGroup::Buttons);
  const auto* triggers = pad->GetGroup(PadGroup::Triggers);

  return ControlExpressionEquals(buttons, 0, "`X`") &&
         ControlExpressionEquals(buttons, 1, "`Z`") &&
         ControlExpressionEquals(buttons, 2, "`C`") &&
         ControlExpressionEquals(buttons, 3, "`S`") &&
         ControlExpressionEquals(buttons, 4, "`D`") &&
         ControlExpressionEquals(triggers, 0, "`Q`") &&
         ControlExpressionEquals(triggers, 1, "`W`");
}

static bool HasLegacySwitchDefaults(GCPad* pad)
{
  const auto* buttons = pad->GetGroup(PadGroup::Buttons);
  const auto* dpad = pad->GetGroup(PadGroup::DPad);
  const auto* main_stick = pad->GetGroup(PadGroup::MainStick);
  const auto* c_stick = pad->GetGroup(PadGroup::CStick);
  const auto* triggers = pad->GetGroup(PadGroup::Triggers);

  return ControlExpressionEquals(buttons, 0, "`A`") &&
         ControlExpressionEquals(buttons, 1, "`Y`") &&
         ControlExpressionEquals(buttons, 2, "`X`") &&
         ControlExpressionEquals(buttons, 3, "`B`") &&
         ControlExpressionEquals(buttons, 4, "`Z`") &&
         ControlExpressionEquals(buttons, 5, "`Start`") &&
         ControlExpressionEquals(dpad, 0, "`Up`") &&
         ControlExpressionEquals(dpad, 1, "`Down`") &&
         ControlExpressionEquals(dpad, 2, "`Left`") &&
         ControlExpressionEquals(dpad, 3, "`Right`") &&
         ControlExpressionEquals(main_stick, 0, "`Y0+`") &&
         ControlExpressionEquals(main_stick, 1, "`Y0-`") &&
         ControlExpressionEquals(main_stick, 2, "`X0-`") &&
         ControlExpressionEquals(main_stick, 3, "`X0+`") &&
         ControlExpressionEquals(c_stick, 0, "`Y1+`") &&
         ControlExpressionEquals(c_stick, 1, "`Y1-`") &&
         ControlExpressionEquals(c_stick, 2, "`X1-`") &&
         ControlExpressionEquals(c_stick, 3, "`X1+`") &&
         ControlExpressionEquals(triggers, 0, "`L`") &&
         ControlExpressionEquals(triggers, 1, "`R`") &&
         ControlExpressionEquals(triggers, 2, "`L`") &&
         ControlExpressionEquals(triggers, 3, "`R`");
}

static bool HasLegacyKeyboardDefaults(WiimoteEmu::Wiimote* wiimote)
{
  const auto* buttons = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons);
  const auto* nunchuk_stick = wiimote->GetNunchukGroup(WiimoteEmu::NunchukGroup::Stick);
  const auto* nunchuk_buttons = wiimote->GetNunchukGroup(WiimoteEmu::NunchukGroup::Buttons);

  return ControlExpressionEquals(buttons, 0, "`Click 0`") &&
         ControlExpressionEquals(buttons, 1, "`Click 1`") &&
         ControlExpressionEquals(nunchuk_stick, 0, "W") &&
         ControlExpressionEquals(nunchuk_stick, 1, "S") &&
         ControlExpressionEquals(nunchuk_stick, 2, "A") &&
         ControlExpressionEquals(nunchuk_stick, 3, "D") &&
         ControlExpressionEquals(nunchuk_buttons, 0, "Control_L") &&
         ControlExpressionEquals(nunchuk_buttons, 1, "Shift_L");
}

static bool HasLegacySwitchDefaults(WiimoteEmu::Wiimote* wiimote)
{
  const auto* buttons = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Buttons);
  const auto* shake = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Shake);
  const auto* point = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Point);
  const auto* dpad = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::DPad);
  const auto* imu_point = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::IMUPoint);
  const auto* imu_accel =
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::IMUAccelerometer);
  const auto* imu_gyro = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::IMUGyroscope);

  const bool switch_buttons =
      ControlExpressionEquals(buttons, 0, "`A`") &&
      ControlExpressionEquals(buttons, 1, "`R2`") &&
      ControlExpressionEquals(buttons, 2, "`X`") &&
      ControlExpressionEquals(buttons, 3, "`B`") &&
      ControlExpressionEquals(buttons, 4, "`Select`") &&
      ControlExpressionEquals(buttons, 5, "`Start`") &&
      ControlExpressionEquals(buttons, 6, "`L3`") &&
      ControlExpressionEquals(shake, 0, "`R3`") &&
      ControlExpressionEquals(shake, 1, "`R3`") &&
      ControlExpressionEquals(shake, 2, "`R3`") &&
      ControlExpressionEquals(point, 0, "`Y1+`") &&
      ControlExpressionEquals(point, 1, "`Y1-`") &&
      ControlExpressionEquals(point, 2, "`X1-`") &&
      ControlExpressionEquals(point, 3, "`X1+`") &&
      ControlExpressionEquals(dpad, 0, "`Up`") &&
      ControlExpressionEquals(dpad, 1, "`Down`") &&
      ControlExpressionEquals(dpad, 2, "`Left`") &&
      ControlExpressionEquals(dpad, 3, "`Right`");

  const bool empty_motion =
      ControlExpressionEquals(imu_accel, 0, "") &&
      ControlExpressionEquals(imu_accel, 1, "") &&
      ControlExpressionEquals(imu_accel, 2, "") &&
      ControlExpressionEquals(imu_accel, 3, "") &&
      ControlExpressionEquals(imu_accel, 4, "") &&
      ControlExpressionEquals(imu_accel, 5, "") &&
      ControlExpressionEquals(imu_gyro, 0, "") &&
      ControlExpressionEquals(imu_gyro, 1, "") &&
      ControlExpressionEquals(imu_gyro, 2, "") &&
      ControlExpressionEquals(imu_gyro, 3, "") &&
      ControlExpressionEquals(imu_gyro, 4, "") &&
      ControlExpressionEquals(imu_gyro, 5, "");
  const bool old_sensor_box =
      ControlExpressionEquals(imu_point, 0, "") ||
      DoubleSettingEquals(imu_point, "Total Yaw", 25.0) ||
      DoubleSettingEquals(imu_point, "Horizontal FOV", 42.0);

  return switch_buttons && (empty_motion || old_sensor_box);
}

static void ILOG(const char* fmt, ...)
{
  // if (!s_input_log)
  //   s_input_log = fopen("sdmc:/dolphin-nx-input.log", "w");
  if (!s_input_log)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(s_input_log, fmt, args);
  va_end(args);
  fflush(s_input_log);
}

static void EnsureDualJoyConAssignment();

static void ConfigurePad()
{
  ILOG("padConfigureInput...\n");
  padConfigureInput(MAX_SWITCH_PLAYERS, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical);
  ILOG("padInitialize...\n");
  padInitialize(&s_pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
  padInitialize(&s_pads[1], HidNpadIdType_No2);
  padInitialize(&s_pads[2], HidNpadIdType_No3);
  padInitialize(&s_pads[3], HidNpadIdType_No4);
  EnsureDualJoyConAssignment();
}

static bool HasSingleJoyConStyle(u32 style_set, HidNpadStyleTag style)
{
  return (style_set & style) != 0;
}

static void EnsureDualJoyConAssignment()
{
  const u32 no1_style = hidGetNpadStyleSet(HidNpadIdType_No1);
  const u32 no2_style = hidGetNpadStyleSet(HidNpadIdType_No2);

  const bool no1_left = HasSingleJoyConStyle(no1_style, HidNpadStyleTag_NpadJoyLeft);
  const bool no1_right = HasSingleJoyConStyle(no1_style, HidNpadStyleTag_NpadJoyRight);
  const bool no2_left = HasSingleJoyConStyle(no2_style, HidNpadStyleTag_NpadJoyLeft);
  const bool no2_right = HasSingleJoyConStyle(no2_style, HidNpadStyleTag_NpadJoyRight);
  const bool split_pair = (no1_left && no2_right) || (no1_right && no2_left);

  if (!split_pair)
    return;

  hidSetNpadJoyAssignmentModeDual(HidNpadIdType_No1);
  hidSetNpadJoyAssignmentModeDual(HidNpadIdType_No2);

  if (split_pair)
    hidMergeSingleJoyAsDualJoy(HidNpadIdType_No1, HidNpadIdType_No2);
}

static void StopSixAxisSensor(unsigned player)
{
  SixAxisPlayerState& sensor = s_sixaxis[player];
  if (!sensor.started)
    return;

  for (int i = 0; i < sensor.handle_count; ++i)
    hidStopSixAxisSensor(sensor.handles[i]);

  sensor = {};
}

static void StopSixAxisSensors()
{
  for (unsigned player = 0; player < MAX_SWITCH_PLAYERS; ++player)
    StopSixAxisSensor(player);
}

static bool ResolveSixAxisSource(unsigned player, HidNpadIdType* out_id,
                                 HidNpadStyleTag* out_style)
{
  const PadState& pad = s_pads[player];
  const u32 style_set = padGetStyleSet(&pad);

  if (player == 0 && padIsHandheld(&pad) && (style_set & HidNpadStyleTag_NpadHandheld))
  {
    *out_id = HidNpadIdType_Handheld;
    *out_style = HidNpadStyleTag_NpadHandheld;
    return true;
  }

  const HidNpadIdType npad_id = s_player_npad_ids[player];
  if (!padIsNpadActive(&pad, npad_id))
    return false;

  static constexpr HidNpadStyleTag styles[] = {
      HidNpadStyleTag_NpadFullKey,
      HidNpadStyleTag_NpadJoyDual,
      HidNpadStyleTag_NpadJoyLeft,
      HidNpadStyleTag_NpadJoyRight,
  };

  for (const HidNpadStyleTag style : styles)
  {
    if (style_set & style)
    {
      *out_id = npad_id;
      *out_style = style;
      return true;
    }
  }

  return false;
}

static bool EnsureSixAxisSensor(unsigned player)
{
  SixAxisPlayerState& sensor = s_sixaxis[player];
  HidNpadIdType id = s_player_npad_ids[player];
  HidNpadStyleTag style = HidNpadStyleTag_NpadFullKey;
  if (!ResolveSixAxisSource(player, &id, &style))
  {
    StopSixAxisSensor(player);
    return false;
  }

  if (sensor.started && sensor.id == id && sensor.style == style)
    return true;

  StopSixAxisSensor(player);

  HidSixAxisSensorHandle handles[MAX_SIXAXIS_HANDLES] = {};
  const int handle_count = style == HidNpadStyleTag_NpadJoyDual ? MAX_SIXAXIS_HANDLES : 1;
  const Result handle_rc = hidGetSixAxisSensorHandles(handles, handle_count, id, style);
  if (R_FAILED(handle_rc))
  {
    ILOG("hidGetSixAxisSensorHandles(id=%d style=0x%x) failed rc=0x%x\n", static_cast<int>(id),
         static_cast<unsigned>(style), static_cast<unsigned>(handle_rc));
    return false;
  }

  for (int i = 0; i < handle_count; ++i)
  {
    const Result start_rc = hidStartSixAxisSensor(handles[i]);
    if (R_FAILED(start_rc))
    {
      for (int started = 0; started < i; ++started)
        hidStopSixAxisSensor(handles[started]);

      ILOG("hidStartSixAxisSensor(id=%d style=0x%x index=%d) failed rc=0x%x\n",
           static_cast<int>(id), static_cast<unsigned>(style), i,
           static_cast<unsigned>(start_rc));
      return false;
    }
  }

  for (int i = 0; i < handle_count; ++i)
    sensor.handles[i] = handles[i];
  sensor.handle_count = handle_count;
  sensor.id = id;
  sensor.style = style;
  sensor.started = true;
  ILOG("Started six-axis sensor (player=%u id=%d style=0x%x handles=%d)\n", player + 1,
       static_cast<int>(id), static_cast<unsigned>(style), handle_count);
  return true;
}

static HidSixAxisSensorHandle GetActiveSixAxisSensorHandle(unsigned player)
{
  const SixAxisPlayerState& sensor = s_sixaxis[player];
  if (sensor.style == HidNpadStyleTag_NpadJoyDual && sensor.handle_count >= 2)
  {
    const u32 attributes = padGetAttributes(&s_pads[player]);
    if ((attributes & HidNpadAttribute_IsRightConnected) != 0)
      return sensor.handles[1];
  }

  return sensor.handles[0];
}

void Init(const WindowSystemInfo& wsi)
{
  ILOG("Input::Init start\n");
  for (WiiControllerPlayerState& player_state : s_wii_controller_states)
    player_state = {};
  for (SixAxisPlayerState& sensor_state : s_sixaxis)
    sensor_state = {};
  s_dual_joycon_assignment_update = 0;

  const Result hid_rc = hidInitialize();
  s_hid_initialized = R_SUCCEEDED(hid_rc);
  ILOG("hidInitialize rc=0x%x sharedmem=%p\n", hid_rc, hidGetSharedmemAddr());

  // Configure Switch input for standard controllers
  if (s_hid_initialized && hidGetSharedmemAddr())
  {
    ConfigurePad();
    s_pad_initialized = true;
    ILOG("pad init done\n");
  }
  else
  {
    ILOG("pad init skipped: HID sharedmem unavailable\n");
  }

  ILOG("g_controller_interface.Initialize...\n");
  g_controller_interface.Initialize(wsi);
  ILOG("g_controller_interface.Initialize done\n");

  ILOG("Adding devices...\n");
  for (unsigned port = 0; port < 4; port++)
  {
    ILOG("  Creating Device port %u\n", port);
    auto dev = std::make_shared<Device>(port);
    ILOG("  Device created, calling AddDevice\n");
    g_controller_interface.AddDevice(std::move(dev));
    ILOG("  AddDevice port %u Device done\n", port);

    ILOG("  Creating AnalogDevice port %u\n", port);
    auto adev = std::make_shared<AnalogDevice>(port);
    ILOG("  AnalogDevice created, calling AddDevice\n");
    g_controller_interface.AddDevice(std::move(adev));
    ILOG("  AddDevice port %u AnalogDevice done\n", port);
  }
  ILOG("Devices added\n");

  ILOG("GCAdapter::Init...\n");
  GCAdapter::Init();
  ILOG("GCAdapter::Init done\n");

  ILOG("Pad::Initialize...\n");
  Pad::Initialize();
  bool repaired_gc_defaults = false;
  for (unsigned i = 0; i < MAX_SWITCH_PLAYERS; ++i)
  {
    if (auto* gc_pad = static_cast<GCPad*>(Pad::GetConfig()->GetController(i)))
    {
      const auto& default_device = gc_pad->GetDefaultDevice();
      const bool wrong_device = IsWrongSwitchDefaultDevice(default_device, i);
      const bool legacy_keyboard_defaults = HasLegacyKeyboardDefaults(gc_pad);
      const bool legacy_switch_defaults = HasLegacySwitchDefaults(gc_pad);

      if (wrong_device || legacy_keyboard_defaults || legacy_switch_defaults)
      {
        ILOG("Repairing GCPad%u defaults for Switch (device=%s, keyboard_legacy=%d, "
             "switch_legacy=%d)\n",
             i + 1, default_device.ToString().c_str(), legacy_keyboard_defaults,
             legacy_switch_defaults);
        if (legacy_keyboard_defaults || legacy_switch_defaults)
          gc_pad->LoadDefaults(g_controller_interface);
        SetSwitchDefaultDevice(gc_pad, i);
        gc_pad->UpdateReferences(g_controller_interface);
        repaired_gc_defaults = true;
      }
    }
  }
  if (repaired_gc_defaults)
    Pad::GetConfig()->SaveConfig();
  ILOG("Pad::Initialize done\n");

  ILOG("Pad::InitializeGBA...\n");
  Pad::InitializeGBA();
  ILOG("Pad::InitializeGBA done\n");

  ILOG("Keyboard::Initialize...\n");
  Keyboard::Initialize();
  ILOG("Keyboard::Initialize done\n");

  ILOG("FreeLook::Initialize...\n");
  FreeLook::Initialize();
  ILOG("FreeLook::Initialize done\n");

  ILOG("Wiimote::Initialize...\n");
  Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
  bool repaired_wiimote_defaults = false;
  for (int i = WIIMOTE_CHAN_0; i < MAX_WIIMOTES; ++i)
  {
    if (auto* wiimote =
            static_cast<WiimoteEmu::Wiimote*>(Wiimote::GetConfig()->GetController(i)))
    {
      const auto& default_device = wiimote->GetDefaultDevice();
      const bool wrong_device = IsWrongSwitchDefaultDevice(default_device, i);
      const bool legacy_keyboard_defaults = HasLegacyKeyboardDefaults(wiimote);
      const bool legacy_switch_defaults = HasLegacySwitchDefaults(wiimote);

      if (wrong_device || legacy_keyboard_defaults || legacy_switch_defaults)
      {
        ILOG("Repairing Wiimote%d defaults for Switch (device=%s, keyboard_legacy=%d, "
             "switch_legacy=%d)\n",
             i + 1, default_device.ToString().c_str(), legacy_keyboard_defaults,
             legacy_switch_defaults);
        if (legacy_keyboard_defaults || legacy_switch_defaults)
          wiimote->LoadDefaults(g_controller_interface);
        SetSwitchDefaultDevice(wiimote, i);
        wiimote->UpdateReferences(g_controller_interface);
        repaired_wiimote_defaults = true;
      }
    }
  }
  if (repaired_wiimote_defaults)
    Wiimote::GetConfig()->SaveConfig();
  ILOG("Wiimote::Initialize done\n");

  ILOG("Input::Init complete\n");
}

void Update()
{
  if (!s_pad_initialized || !hidGetSharedmemAddr())
    return;

  if (s_dual_joycon_assignment_update == 0)
    EnsureDualJoyConAssignment();
  s_dual_joycon_assignment_update = (s_dual_joycon_assignment_update + 1) % 60;

  for (unsigned player = 0; player < MAX_SWITCH_PLAYERS; ++player)
    padUpdate(&s_pads[player]);

  for (unsigned player = 0; player < MAX_SWITCH_PLAYERS; ++player)
    HandleWiiControllerModeToggle(player);

  for (unsigned player = 0; player < MAX_SWITCH_PLAYERS; ++player)
  {
    if (!EnsureSixAxisSensor(player))
    {
      s_sixaxis[player].state = {};
      continue;
    }

    HidSixAxisSensorState state = {};
    if (hidGetSixAxisSensorStates(GetActiveSixAxisSensorHandle(player), &state, 1) > 0)
      s_sixaxis[player].state = state;
    else
      s_sixaxis[player].state = {};
  }
}

PadState* GetPad()
{
  return s_pad_initialized ? &s_pads[0] : nullptr;
}

void Shutdown()
{
  Wiimote::ResetAllWiimotes();
  Wiimote::Shutdown();
  Pad::Shutdown();
  Pad::ShutdownGBA();
  Keyboard::Shutdown();
  FreeLook::Shutdown();
  g_controller_interface.Shutdown();
  StopSixAxisSensors();
  if (s_hid_initialized)
  {
    hidExit();
    s_hid_initialized = false;
  }
  s_pad_initialized = false;
  if (s_input_log)
  {
    fclose(s_input_log);
    s_input_log = nullptr;
  }
}

}  // namespace Input
}  // namespace DolphinNX
