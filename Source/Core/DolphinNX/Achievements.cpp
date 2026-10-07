// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Achievements.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <json.hpp>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "DolphinNX/Audio.h"
#include "TicoOverlayHost.h"
#include "overlay/overlay_ui.h"

namespace DolphinNX::Achievements
{
namespace
{
constexpr const char* kAccountsPath = "sdmc:/tico/config/accounts.jsonc";
constexpr std::size_t kMaxNotifications = 8;

struct BadgePixels
{
  std::vector<u8> rgba;
  int width = 0;
  int height = 0;
};

class RAHost final : public IOverlayRAHost
{
public:
  std::mutex& Mutex() override { return m_mutex; }
  std::vector<RANotification>& Notifications() override { return m_notifications; }
  RAAlertPosition AlertPosition() const override { return m_position; }
  ImTextureID IconTexture() const override { return m_icon; }
  void SetIconTexture(ImTextureID texture) override { m_icon = texture; }
  ImTextureID BadgeTexture(const std::string& badge) const override
  {
    const auto it = m_badges.find(badge);
    return it != m_badges.end() ? it->second : 0;
  }

  void SetPosition(RAAlertPosition position) { m_position = position; }

  // From AchievementManager's threads: one toast per message. A message with
  // only a badge (the game's, as a game starts) lends it to the next one.
  void Push(std::string message, u32 duration_ms,
            const VideoCommon::CustomTextureData::ArraySlice::Level* icon)
  {
    std::string badge;
    if (icon && icon->width != 0 && icon->height != 0 &&
        icon->format == AbstractTextureFormat::RGBA8)
    {
      BadgePixels pixels;
      pixels.width = static_cast<int>(icon->width);
      pixels.height = static_cast<int>(icon->height);
      const u32 row_length = icon->row_length != 0 ? icon->row_length : icon->width;
      pixels.rgba.resize(static_cast<std::size_t>(icon->width) * icon->height * 4);
      for (u32 y = 0; y < icon->height; ++y)
      {
        std::copy_n(icon->data.data() + static_cast<std::size_t>(y) * row_length * 4,
                    static_cast<std::size_t>(icon->width) * 4,
                    pixels.rgba.data() + static_cast<std::size_t>(y) * icon->width * 4);
      }
      std::lock_guard lock(m_pending_mutex);
      badge = "badge" + std::to_string(m_next_badge++);
      m_pending_badges.emplace(badge, std::move(pixels));
    }

    std::lock_guard lock(m_mutex);
    if (message.empty())
    {
      if (!badge.empty())
        m_carried_badge = badge;
      return;
    }
    if (badge.empty())
      badge = std::exchange(m_carried_badge, {});

    RANotification notification;
    notification.title = "RetroAchievements";
    notification.description = std::move(message);
    notification.badge_name = badge.empty() ? "ra_icon" : badge;
    notification.duration = std::clamp(static_cast<float>(duration_ms) / 1000.0f, 2.0f, 6.0f);
    if (m_notifications.size() >= kMaxNotifications)
      m_notifications.erase(m_notifications.begin());
    m_notifications.push_back(std::move(notification));
  }

  void Upload(IOverlayHost& host)
  {
    std::map<std::string, BadgePixels> pending;
    {
      std::lock_guard lock(m_pending_mutex);
      pending.swap(m_pending_badges);
    }
    for (auto& [name, pixels] : pending)
    {
      if (const ImTextureID texture =
              host.CreateTextureRGBA(pixels.rgba.data(), pixels.width, pixels.height))
      {
        m_badges.emplace(name, texture);
      }
    }
  }

  bool HasNotifications()
  {
    std::lock_guard lock(m_mutex);
    return !m_notifications.empty();
  }

private:
  std::mutex m_mutex;
  std::vector<RANotification> m_notifications;
  std::string m_carried_badge;
  RAAlertPosition m_position = RAAlertPosition::TopRight;
  ImTextureID m_icon = 0;
  // drawing thread only
  std::unordered_map<std::string, ImTextureID> m_badges;
  std::mutex m_pending_mutex;
  std::map<std::string, BadgePixels> m_pending_badges;
  u64 m_next_badge = 0;
};

RAHost s_host;

// tico's sound effects setting (audio.jsonc)
bool TicoSoundsEnabled()
{
  std::string text;
  if (!File::ReadFileToString("sdmc:/tico/config/audio.jsonc", text))
    return false;
  const nlohmann::json root = nlohmann::json::parse(text, nullptr, false, true);
  if (!root.is_object() || !root.contains("sound_enabled"))
    return false;
  const nlohmann::json& value = root["sound_enabled"];
  return value.is_boolean() ? value.get<bool>() :
                              value.is_string() && value.get<std::string>() == "true";
}

// An unlock or a mastered game gets tico's trophy sound.
void PlayTrophySoundFor(const std::string& message)
{
  if ((message.starts_with("Unlocked: ") || message.starts_with("Congratulations")) &&
      TicoSoundsEnabled())
  {
    DolphinNX::Audio::PlayEffect("romfs:/assets/trophy.wav");
  }
}
bool s_started = false;
bool s_hardcore = false;
std::string s_token;

nlohmann::json ReadAccounts()
{
  std::string text;
  if (!File::ReadFileToString(kAccountsPath, text))
    return nlohmann::json::object();
  nlohmann::json root = nlohmann::json::parse(text, nullptr, false, true);
  return root.is_object() ? root : nlohmann::json::object();
}

RAAlertPosition ParsePosition(const std::string& position)
{
  if (position == "top_left")
    return RAAlertPosition::TopLeft;
  if (position == "bottom_left")
    return RAAlertPosition::BottomLeft;
  if (position == "bottom_right")
    return RAAlertPosition::BottomRight;
  return RAAlertPosition::TopRight;
}

// The token a password login got, back into tico's account so the next game
// (in any core) logs in with it.
void SaveToken(const std::string& token)
{
  nlohmann::json root = ReadAccounts();
  root["ra_token"] = token;
  if (!File::WriteStringToFile(kAccountsPath, root.dump(4)))
    WARN_LOG_FMT(ACHIEVEMENTS, "Could not save the RetroAchievements token to {}", kAccountsPath);
}
}  // namespace

void Start()
{
  const nlohmann::json account = ReadAccounts();
  const bool enabled = account.value("ra_enabled", false);
  const std::string username = account.value("ra_username", std::string());
  const std::string password = account.value("ra_password", std::string());
  s_token = account.value("ra_token", std::string());

  Config::SetBase(Config::RA_ENABLED, enabled && !username.empty());
  if (!enabled || username.empty())
    return;

  Config::SetBase(Config::RA_USERNAME, username);
  Config::SetBase(Config::RA_API_TOKEN, s_token);
  Config::SetBase(Config::RA_HARDCORE_ENABLED, account.value("ra_hardcore_mode", false));
  s_host.SetPosition(ParsePosition(account.value("ra_alert_position", std::string("top_right"))));

  AchievementManager& manager = AchievementManager::GetInstance();
  manager.SetMessageSink([](std::string message, u32 duration_ms,
                            const VideoCommon::CustomTextureData::ArraySlice::Level* icon) {
    PlayTrophySoundFor(message);
    s_host.Push(std::move(message), duration_ms, icon);
  });
  manager.Init(nullptr);
  if (s_token.empty() && !password.empty())
    manager.Login(password);
  s_started = true;
  INFO_LOG_FMT(ACHIEVEMENTS, "RetroAchievements on for {} (hardcore {})", username,
               Config::Get(Config::RA_HARDCORE_ENABLED));
}

void Update()
{
  if (!s_started)
    return;

  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore != s_hardcore)
  {
    s_hardcore = hardcore;
    SwitchFrontend::OverlayUI::SetHardcoreMode(hardcore);
  }

  const std::string token = Config::Get(Config::RA_API_TOKEN);
  if (!token.empty() && token != s_token)
  {
    s_token = token;
    SaveToken(token);
  }
}

void Shutdown()
{
  if (!s_started)
    return;
  AchievementManager& manager = AchievementManager::GetInstance();
  manager.SetMessageSink({});
  manager.Shutdown();
  s_started = false;
}

IOverlayRAHost* Host()
{
  return s_started ? &s_host : nullptr;
}

void UploadBadges(IOverlayHost& host)
{
  if (s_started)
    s_host.Upload(host);
}

bool HasNotifications()
{
  return s_started && s_host.HasNotifications();
}
}  // namespace DolphinNX::Achievements
