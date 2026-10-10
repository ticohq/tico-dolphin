/// @file TicoSession.h
/// @brief A core's view of who is playing, from the session tico seals right
/// before launching it (TicoSecure). Header-only; cores vendor it with
/// TicoSecure.h and nlohmann/json.
///
/// Fields (JSON, version 1):
///   user_id, user_name          the console account (32 hex digits)
///   avatar_path                 the user's custom avatar file, or empty
///   settings                    tico's display, audio and general settings
///                               (the user's), which live sealed in tico.vault
///   saves_path, states_path     that user's folders
///   saves_base, states_base     tico's shared folders they sit in
///   inherits_shared             this user got the data from before per-user
///                               folders: a core moves its own old saves
///                               (Dolphin's tico/system/gc, ...) only for them
///   ra_enabled, ra_username,
///   ra_token, ra_hardcore_mode  RetroAchievements (a token, never a password)
///   ra_badges                   false: pop-ups use a placeholder, no badge
#pragma once

#include "TicoSecure.h"

// A core that keeps nlohmann/json elsewhere includes it before this file.
#ifndef INCLUDE_NLOHMANN_JSON_HPP_
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include <json.hpp> // Cores vendor it flat
#endif
#endif

#include <cstdio>
#include <string>
#include <sstream>
#include <map>
#include <vector>

namespace tico {

struct Session {
  bool valid = false; ///< False when there is none or it cannot be opened
  std::string userId, userName, avatarPath;
  std::map<std::string, std::string> settings; ///< Name -> JSON text
  std::string savesPath, statesPath; ///< The user's, with a trailing '/'
  std::string savesBase, statesBase; ///< tico's, which the user's sit in
  bool raEnabled = false, raHardcore = false;
  bool raBadges = true; ///< tico fetched badges; false: show a placeholder
  bool inheritsShared = true; ///< A core's old saves are this user's to move
  std::string raUsername, raToken;

#ifdef __SWITCH__
  /// @brief userId as an account service id.
  bool AccountId(AccountUid &uid) const {
    if (userId.size() != 32)
      return false;
    unsigned long long hi = 0, lo = 0;
    if (std::sscanf(userId.c_str(), "%16llx%16llx", &hi, &lo) != 2)
      return false;
    uid.uid[0] = hi;
    uid.uid[1] = lo;
    return accountUidIsValid(&uid);
  }
#endif
};

/// @brief The session for this run, read once.
inline const Session &CurrentSession() {
  static Session session = [] {
    Session s;
    std::string text;
    if (!secure::ReadSession(text))
      return s;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object())
      return s;
    auto str = [&](const char *key) {
      return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
    };
    auto flag = [&](const char *key) {
      return j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
    };
    s.valid = true;
    s.userId = str("user_id");
    s.userName = str("user_name");
    s.avatarPath = str("avatar_path");
    if (j.contains("settings") && j["settings"].is_object())
      for (const auto &item : j["settings"].items())
        if (item.value().is_object())
          s.settings[item.key()] = item.value().dump();
    s.savesPath = str("saves_path");
    s.statesPath = str("states_path");
    s.savesBase = str("saves_base");
    s.statesBase = str("states_base");
    s.raEnabled = flag("ra_enabled");
    s.raHardcore = flag("ra_hardcore_mode");
    s.raBadges = !(j.contains("ra_badges") && j["ra_badges"].is_boolean()) || flag("ra_badges");
    s.inheritsShared =
        !(j.contains("inherits_shared") && j["inherits_shared"].is_boolean()) || flag("inherits_shared");
    s.raUsername = str("ra_username");
    s.raToken = str("ra_token");
    return s;
  }();
  return session;
}

/// @name tico's settings
/// "display", "audio", "general": the current user's, from the session. They
/// live sealed in tico.vault; with no session (an older tico), the files
/// sdmc:/tico/config/<name>.jsonc they were kept in before.
/// @{
inline std::string SettingsText(const char *name) {
  const Session &s = CurrentSession();
  const auto it = s.settings.find(name);
  if (it != s.settings.end())
    return it->second;
#ifdef __SWITCH__
  const std::string path = std::string("sdmc:/tico/config/") + name + ".jsonc";
#else
  const std::string path = std::string("tico/config/") + name + ".jsonc";
#endif
  std::string text;
  if (FILE *file = std::fopen(path.c_str(), "rb")) {
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
      text.append(buffer, n);
    std::fclose(file);
  }
  return text;
}

/// @brief Which of tico's settings @p path names (".../config/general.jsonc"
/// and so on), or nullptr for any other file.
inline const char *SettingsNameFor(const std::string &path) {
  for (const char *name : {"general", "display", "audio"}) {
    const std::string tail = std::string("config/") + name + ".jsonc";
    if (path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0)
      return name;
  }
  return nullptr;
}

/// @brief A file's text, where tico's settings files come from the session
/// (SettingsText) and any other path from the card.
inline bool ReadConfigFile(const std::string &path, std::string &out) {
  if (const char *name = SettingsNameFor(path)) {
    out = SettingsText(name);
    return !out.empty();
  }
  out.clear();
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    out.append(buffer, n);
  std::fclose(file);
  return true;
}

/// @brief As a core's ReadWholeFile(path, content): false when there is none.
inline bool ReadSettings(const char *name, std::string &out) {
  out = SettingsText(name);
  return !out.empty();
}

/// @brief In place of std::ifstream on a path that may be one of tico's
/// settings files.
class ConfigFileStream : public std::istringstream {
public:
  explicit ConfigFileStream(const std::string &path) {
    std::string text;
    m_open = ReadConfigFile(path, text);
    str(text);
  }
  bool is_open() const { return m_open; }
  bool good() const { return m_open && std::istringstream::good(); }
  void close() {}

private:
  bool m_open = false;
};

/// @brief In place of std::ifstream on one of tico's settings files.
class SettingsStream : public std::istringstream {
public:
  explicit SettingsStream(const char *name) : std::istringstream(SettingsText(name)) {
    m_open = !str().empty();
  }
  bool is_open() const { return m_open; }
  bool good() const { return m_open && std::istringstream::good(); }
  void close() {}

private:
  bool m_open = false;
};
/// @}

/// @brief The saves (or states) folder to use, given the one a core would
/// use on its own: tico's default, or a custom folder from the core's
/// settings. Every user has <folder>/users/<id>/ in either: tico's folder
/// becomes the user's (as tico computed it), a custom folder gets the same
/// users/<id>/ part. Before tico has moved the shared data (and with no
/// session) the folder is used as it is. Always ends in '/'.
inline std::string UserContentRoot(std::string folder, bool saves) {
  auto slash = [](std::string p) {
    if (!p.empty() && p.back() != '/')
      p += '/';
    return p;
  };
  folder = slash(folder);
  const Session &s = CurrentSession();
  const std::string user = slash(saves ? s.savesPath : s.statesPath);
  if (!s.valid || user.empty())
    return folder;
  const std::string base = slash(saves ? s.savesBase : s.statesBase);
#ifdef __SWITCH__
  const std::string fallback = saves ? "sdmc:/tico/saves/" : "sdmc:/tico/states/";
#else
  const std::string fallback = saves ? "tico/saves/" : "tico/states/";
#endif
  if (folder == fallback || folder == base)
    return user;
  // The user's part of tico's folder: "users/<id>/", or nothing before the move
  const std::string part =
      !base.empty() && user.compare(0, base.size(), base) == 0 ? user.substr(base.size()) : "";
  return folder + part;
}

/// @brief Where a core may look for the player's custom avatar, given its
/// usual list: with a session, only the current user's own file (no file:
/// none, and the core shows their console account's picture); without one,
/// the usual list.
template <typename List> inline std::vector<const char *> AvatarCandidates(const List &usual) {
  std::vector<const char *> out;
  const Session &s = CurrentSession();
  if (!s.valid) {
    for (const char *path : usual)
      out.push_back(path);
  } else if (!s.avatarPath.empty()) {
    out.push_back(s.avatarPath.c_str());
  }
  return out;
}
/// @}

} // namespace tico
