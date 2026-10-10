/// @file Saves.h
/// @brief Game saves in tico's saves folder, as the other cores keep them.
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "DiscIO/RiivolutionPatcher.h"

// GameCube memory cards live in sdmc:/tico/saves/gc and each Wii game's save in
// sdmc:/tico/saves/wii/<GAMEID>, in Dolphin's own formats. The Wii's system files
// (settings, installed WADs) stay in Dolphin's NAND under tico/system/gc.
namespace DolphinNX::Saves
{
/// Called for each save moved (where it goes, how many are done, of how many),
/// then once more with an empty path when all are.
using MigrationProgress =
    std::function<void(const std::string& path, std::size_t done, std::size_t total)>;

/// Moves saves from where earlier versions kept them (GameCube memory cards, each
/// Wii game's save in the NAND), once. Never overwrites; @p progress is only
/// called when there is something to move.
void Migrate(const MigrationProgress& progress);

/// Points the GameCube memory cards at tico's saves folder (as config defaults,
/// so a raw override in dolphin.jsonc still wins).
void ApplyGameCubeCardPaths();

/// Points Dolphin's save states (and their pictures) at the current user's
/// states folder, states/gc or states/wii (TicoSession.h), now that the
/// platform is known. Dolphin kept every user's states in its own folder
/// before: they move once, for the account tico moved the shared data to.
void ApplyStatesFolder(bool wii);

/// The running Wii title's save folder as a NAND redirect. Call once the title
/// is known.
std::optional<DiscIO::Riivolution::SavegameRedirect> WiiSaveRedirect();

/// Imports a data.bin (a Wii's SD card export) for the running title from
/// sdmc:/tico/saves/wii/import, keeping the save it replaces. Call once the
/// redirect is in place.
void ImportWiiSaves();
}  // namespace DolphinNX::Saves
