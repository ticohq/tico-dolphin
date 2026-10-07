/// @file Saves.h
/// @brief Game saves in tico's saves folder, as the other cores keep them.
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "DiscIO/RiivolutionPatcher.h"

// GameCube memory cards live in sdmc:/tico/saves/gc and each Wii game's save in
// sdmc:/tico/saves/wii/<GAMEID>, in Dolphin's own formats. The Wii's system files
// (settings, installed WADs) stay in Dolphin's NAND under tico/system/gc.
namespace DolphinNX::Saves
{
/// Moves saves from where earlier versions kept them, once. Never overwrites.
void MigrateGameCubeSaves();

/// Points the GameCube memory cards at tico's saves folder (as config defaults,
/// so a raw override in dolphin.jsonc still wins).
void ApplyGameCubeCardPaths();

/// The running Wii title's save folder as a NAND redirect. On a game's first
/// start its save is copied there from the NAND. Call once the title is known.
std::optional<DiscIO::Riivolution::SavegameRedirect> WiiSaveRedirect();

/// Imports a data.bin (a Wii's SD card export) for the running title from
/// sdmc:/tico/saves/wii/import, keeping the save it replaces. Call once the
/// redirect is in place.
void ImportWiiSaves();
}  // namespace DolphinNX::Saves
