/// @file Discs.h
/// @brief Multi-disc discovery for the overlay's Change Disc menu.
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace DolphinNX::Discs
{
struct DiscEntry
{
    std::string displayName;
    std::string romPath;
};

/// The discs of the game at @p currentPath, sorted by disc number; empty when
/// it is a single-disc game. Paths use forward slashes.
std::vector<DiscEntry> ScanDiscs(std::string currentPath);

/// @p path with backslashes turned into forward slashes, as ScanDiscs reports.
std::string NormalizeDiscPath(const std::string &path);
}  // namespace DolphinNX::Discs
