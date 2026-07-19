// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace ControllerEmu
{
class ControlGroupContainer;
}

namespace DolphinNX::ControllerProfiles
{

// A profile describes one physical Switch controller (Pro, single Joy-Con, an NSO
// pad, ...) and how its inputs map onto each emulated Dolphin controller. See
// assets/config/profiles/dolphin/README.md in the Tico frontend for the format.

// Emulated controller a profile can be applied to. These match the "targets" keys
// in the profile JSON.
namespace Target
{
constexpr const char* GameCube = "gc";
constexpr const char* Wiimote = "wiimote";
// Held sideways the remote is a different controller under the thumbs, so it gets its
// own table rather than reusing Wiimote with an orientation flag.
constexpr const char* WiimoteSideways = "wiimote_sideways";
constexpr const char* Nunchuk = "nunchuk";
constexpr const char* Classic = "classic";
}  // namespace Target

// Resolves a configured profile id to a concrete one. "auto" (or an unknown id)
// resolves by reading hidGetNpadStyleSet() for the port and matching against each
// profile's "styles" list; everything else is returned as-is. Falls back to "pro"
// when nothing matches.
std::string Resolve(const std::string& configured, unsigned player);

// Applies a profile's target table to a controller's groups. Every mappable group
// for the target is cleared first, so the profile fully determines the mapping and
// nothing leaks in from Dolphin's built-in defaults. Controls the profile omits are
// left unbound, which is how a profile says "this pad physically cannot do that".
//
// The caller is responsible for UpdateReferences() afterwards.
//
// Returns false if the profile could not be loaded, or has no table for this
// target, in which case the controller is left untouched.
bool Apply(ControllerEmu::ControlGroupContainer* controller, const std::string& profile_id,
           const std::string& target);

// Drops the in-memory profile cache; the next Resolve()/Apply() re-reads from disk.
void Reload();

}  // namespace DolphinNX::ControllerProfiles
