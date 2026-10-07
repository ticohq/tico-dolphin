// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "imgui.h"

class IOverlayHost;

// RetroAchievements toasts (session start, unlocks, leaderboards), drawn over
// the game and the menu alike. The host owns the queue and the badge textures.
namespace SwitchFrontend::RAAlerts {

// Uploads the generic RA icon (romfs:/assets/ra.svg) once the overlay backend
// can create textures. Cheap to call every frame until it succeeds.
void EnsureIcon(IOverlayHost* host);

// True while any toast is queued.
bool HasAlerts(IOverlayHost* host);

// Advances and draws the queued toasts. `description_font` falls back to the
// default font when null.
void Render(IOverlayHost* host, ImDrawList* dl, ImVec2 display_size, float delta_time,
            ImFont* description_font);

} // namespace SwitchFrontend::RAAlerts
