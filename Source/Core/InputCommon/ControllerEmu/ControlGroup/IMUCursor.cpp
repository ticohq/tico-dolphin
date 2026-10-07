// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerEmu/ControlGroup/IMUCursor.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
IMUCursor::IMUCursor(std::string name_, std::string ui_name_)
    : ControlGroup(
          std::move(name_), std::move(ui_name_), GroupType::IMUCursor,
#ifdef ANDROID
          // Enabling this on Android devices which have an accelerometer and gyroscope prevents
          // touch controls from being used for pointing, and touch controls generally work better
          ControlGroup::DefaultValue::Disabled)
#else
          ControlGroup::DefaultValue::Enabled)
#endif
{
  AddInput(Translatability::Translate, _trans("Recenter"));

  // Default values chosen to reach screen edges in most games including the Wii Menu.

  AddSetting(&m_yaw_setting,
             // i18n: Refers to an amount of rotational movement about the "yaw" axis.
             {_trans("Total Yaw"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to emulated wii remote movements.
              _trans("Clamping of rotation about the yaw axis.")},
             25, 0, 360);

  AddSetting(&m_accel_weight_setting,
             {// i18n: Percentage value of accelerometer data (complementary filter coefficient).
              _trans("Accelerometer Influence"),
              // i18n: The symbol/abbreviation for percent.
              _trans("%"),
              // i18n: Refers to a setting controling the influence of accelerometer data.
              _trans("Influence of accelerometer data on pitch and roll. Higher values will reduce "
                     "drift at the cost of noise. Consider values between 1% and 3%.")},
             2, 0, 100);

  AddSetting(&m_mount_pitch_setting,
             {// i18n: Refers to the fixed angle at which a controller sits in a mount/holder.
              _trans("Mount Pitch"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to a controller being held at an angle inside an accessory.
              _trans("Upward tilt of the device relative to the direction it aims. Use this when "
                     "the device sits in a holder at an angle, e.g. a Joy-Con in the grip of a "
                     "gun shell rather than along its barrel.")},
             0, -180, 180);

  AddSetting(&m_mount_roll_setting,
             {// i18n: Refers to the fixed angle at which a controller sits in a mount/holder.
              _trans("Mount Roll"),
              // i18n: The symbol/abbreviation for degrees (unit of angular measure).
              _trans("°"),
              // i18n: Refers to a controller being held at an angle inside an accessory.
              _trans("Leftward rotation of the device around the direction it aims. Use this when "
                     "the device sits sideways in its holder.")},
             0, -180, 180);
}

ControlState IMUCursor::GetTotalYaw() const
{
  return m_yaw_setting.GetValue() * MathUtil::TAU / 360;
}

void IMUCursor::SetTotalYawDegrees(ControlState degrees)
{
  m_yaw_setting.SetValue(degrees);
}

ControlState IMUCursor::GetAccelWeight() const
{
  return m_accel_weight_setting.GetValue() / 100;
}

void IMUCursor::SetAccelWeightPercent(ControlState percent)
{
  m_accel_weight_setting.SetValue(percent);
}

Common::Quaternion IMUCursor::GetMountRotation() const
{
  // Wii Remote axes: +X points left, +Y points backward, +Z points up.
  // A device tilted upward by "pitch" needs to be rotated back down by the same amount, hence the
  // negated angles. Pitch and roll are treated as the components of a rotation vector rather than
  // as sequential Euler angles: its axis then always lies in the XY plane, so the correction can
  // never introduce a yaw of its own. Euler angles would flip roll to 180° past a 90° pitch, which
  // is exactly the range a pistol grip sits in.
  const auto pitch = float(m_mount_pitch_setting.GetValue() * MathUtil::TAU / 360);
  const auto roll = float(m_mount_roll_setting.GetValue() * MathUtil::TAU / 360);

  return Common::Quaternion::RotateXYZ({-pitch, -roll, 0});
}

bool IMUCursor::CalibrateMountFromAccelerometer(const Common::Vec3& accel)
{
  // Gravity measured while the mount is level and aimed at the screen tells us exactly how the
  // device is seated: the shortest rotation bringing that vector back onto +Z is the mount
  // rotation. Being the shortest one, it rotates about a horizontal axis and adds no yaw.
  const auto up = accel.Normalized();
  const auto angle = std::acos(std::clamp(up.z, -1.f, 1.f));
  const auto axis = up.Cross({0, 0, 1});
  const auto axis_length = axis.Length();

  // Degenerate when already upright or exactly inverted, where no axis is meaningful.
  const auto rotation = axis_length ? axis / axis_length * angle : Common::Vec3{-angle, 0, 0};

  const double pitch = -rotation.x * 360 / MathUtil::TAU;
  const double roll = -rotation.y * 360 / MathUtil::TAU;

  // Sub-degree differences are sensor noise between two calibrations of the same mount.
  constexpr double MIN_CHANGE_DEGREES = 1.0;
  const bool changed = std::abs(pitch - m_mount_pitch_setting.GetValue()) >= MIN_CHANGE_DEGREES ||
                       std::abs(roll - m_mount_roll_setting.GetValue()) >= MIN_CHANGE_DEGREES;

  m_mount_pitch_setting.SetValue(pitch);
  m_mount_roll_setting.SetValue(roll);
  return changed;
}

}  // namespace ControllerEmu
