// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class IMUCursor : public ControlGroup
{
public:
  IMUCursor(std::string name, std::string ui_name);

  // Yaw movement in radians.
  ControlState GetTotalYaw() const;
  void SetTotalYawDegrees(ControlState degrees);

  ControlState GetAccelWeight() const;
  void SetAccelWeightPercent(ControlState percent);

  // Rotation that brings sensor data from the device's frame into the frame of whatever the
  // device is mounted on. e.g. a Joy-Con sitting in the grip of a light gun shell rather than
  // along its barrel.
  Common::Quaternion GetMountRotation() const;

  // Derives the mount pitch/roll from a gravity measurement taken while the mount is held level
  // and aimed at the screen. Yaw is not observable from gravity and is handled by "Recenter".
  // Returns whether the stored angles changed noticeably.
  bool CalibrateMountFromAccelerometer(const Common::Vec3& accel);

private:
  SettingValue<double> m_yaw_setting;
  SettingValue<double> m_accel_weight_setting;
  SettingValue<double> m_mount_pitch_setting;
  SettingValue<double> m_mount_roll_setting;
};
}  // namespace ControllerEmu
