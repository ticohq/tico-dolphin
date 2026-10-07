// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// SFML's OS detection has no Horizon support and errors out on anything it does not recognise.
// Rather than make a soft fork of SFML, this should be enough to make it happy.

#pragma once

#define __unix__ 1
#define __linux__ 1

#include_next <SFML/Config.hpp>

#undef __linux__
#undef __unix__

#undef SFML_SYSTEM_LINUX
#define SFML_SYSTEM_HORIZON
