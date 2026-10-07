/// @file Riivolution.h
/// @brief Riivolution mods for Wii games: the Mods menu and the patches at boot.
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "overlay/overlay_ui.h"

struct BootParameters;

// Mods go in sdmc:/tico/mods/wii as they would on a Wii's SD card: the
// riivolution folder with the patch XMLs, and the mod's own files beside it.
// The choices are kept in riivolution/config/<GAMEID>.xml, Riivolution's own
// format, so they carry over to a Wii or to Dolphin on PC.
namespace DolphinNX::Riivolution
{
/// The Mods menu's rows for the running game: each patch's options and their
/// choices, under their section; a note when there are none.
std::vector<SwitchFrontend::OverlayUI::ModMenuEntry> List();

/// Steps an option's choice (Disabled, then each choice) and saves it.
bool Step(int source_index, int direction);

/// Adds the patches whose options are on to a disc's boot.
void AddPatches(BootParameters& boot);
}  // namespace DolphinNX::Riivolution
