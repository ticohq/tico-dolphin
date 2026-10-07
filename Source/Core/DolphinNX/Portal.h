/// @file Portal.h
/// @brief The emulated Skylanders portal's figures, for the quick menu's Portal screen.
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "overlay/overlay_ui.h"

// Figures are 1 KB dumps of the toys (.sky, .bin, .dump), from
// sdmc:/tico/skylanders and from Emulanders' sdmc:/emulanders/figures, with
// their subfolders. A figure saves its progress into its own file, as the toy
// does, so the same file goes on playing in Dolphin on PC or in Emulanders.
namespace DolphinNX::Portal
{
/// The Portal screen's rows: each slot and the figure on it.
std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> List();

/// Puts the next (or previous) figure on a slot, or takes it off.
bool Step(int slot, int direction);
}  // namespace DolphinNX::Portal
