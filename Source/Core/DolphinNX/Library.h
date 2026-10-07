// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

// The game list shown when Dolphin starts without a game.
namespace DolphinNX::Library
{
// Registers the overlay's library (the games in tico's ROM folders and the
// module's own) and its Settings > Library folder editor. `launch` gets the
// chosen game and its console slug.
void Register(std::function<void(const std::string& path, const std::string& slug)> launch);
void Unregister();
}  // namespace DolphinNX::Library
