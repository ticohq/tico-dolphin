// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "overlay/overlay_ui.h"

// The running game's Gecko, Action Replay and patch codes for the overlay's
// Cheats menu. They live in the game's INI files (Dolphin's GameSettings and
// the user's own), which toggling saves to.
namespace DolphinNX::Cheats
{
// The menu's rows: every code, then one to download Gecko codes. When Enable
// Cheats is off, a single row saying so. Called when the menu opens.
std::vector<SwitchFrontend::OverlayUI::CheatMenuEntry> List();

// Turns a code on or off and saves it; the game picks it up once ApplyIfChanged runs.
bool Toggle(int source_index);

// From the emulation side (not the menu): reloads the codes into the running
// game if any changed. Safe to call every frame.
void ApplyIfChanged();

// Downloads this game's Gecko codes from the code server and adds the new ones.
// Blocking; returns the message to show.
std::string DownloadGeckoCodes();
}  // namespace DolphinNX::Cheats
