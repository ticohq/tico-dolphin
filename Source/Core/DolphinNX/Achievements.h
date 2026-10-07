// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class IOverlayHost;
class IOverlayRAHost;

// RetroAchievements through Dolphin's AchievementManager, with tico's account
// (sdmc:/tico/config/accounts.jsonc) and tico's toasts instead of Dolphin's OSD.
namespace DolphinNX::Achievements
{
// Before the game boots: when RetroAchievements is on in tico, logs in with
// the account's token (or its password, saving the token it gets back).
void Start();
// Every main-loop pass: the menu follows hardcore mode; a new token goes back to tico.
void Update();
void Shutdown();

// The overlay's source of RetroAchievements toasts.
IOverlayRAHost* Host();
// On the drawing thread: textures for the badges of queued toasts.
void UploadBadges(IOverlayHost& host);
bool HasNotifications();
}  // namespace DolphinNX::Achievements
