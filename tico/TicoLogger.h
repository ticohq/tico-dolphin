/// @file Logger.h
#pragma once
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace Tico {
/// Generic line-oriented log sink injected into the emulator-agnostic layer
/// (Tico::Main / Tico::CoreRuntime). the frontend wraps the project Logger in
/// one of these so the generic code need not know about the macro logger.
using LogCallback = std::function<void(const std::string &)>;
}  // namespace Tico

class Logger {
public:
  enum class Level { DEBUG, INFO, WARNING, ERROR };
  enum Flags : uint32_t {
    None = 0,
    SwitchOnly = 1 << 0 // Log only on Switch
  };

  static Logger &Instance() {
    static Logger instance;
    return instance;
  }

  // Enable/disable categories at runtime
  void EnableCategory(const std::string &category, bool enabled = true) {
    m_categoryStates[category] = enabled;
  }

  void DisableCategory(const std::string &category) {
    m_categoryStates[category] = false;
  }

  bool IsCategoryEnabled(const std::string &category) const {
    auto it = m_categoryStates.find(category);
    if (it != m_categoryStates.end()) {
      return it->second;
    }
    return true; // Default: enabled if not explicitly set
  }

  void Log(Level level, const char *category, uint32_t flags,
           const char *format, ...) {
#ifdef DISABLE_LOGGING
    return;
#endif
    // Check flags
    if (flags & SwitchOnly) {
#ifndef __SWITCH__
      return; // Skip if not on Switch
#endif
    }

    // Check if category is enabled
    if (!IsCategoryEnabled(category)) {
      return;
    }

#ifdef __SWITCH__
    if (!m_file) {
      m_file = fopen("sdmc:/tico/debug/dolphin.txt", "a");
    }
    if (!m_file)
      return;

    fprintf(m_file, "[%s][%s] ", LevelToString(level), category);

    va_list args;
    va_start(args, format);
    vfprintf(m_file, format, args);
    va_end(args);

    fprintf(m_file, "\n");
    fflush(m_file); // Ensure writes in case of crash
#else
    printf("[%s][%s] ", LevelToString(level), category);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
#endif
  }

  // Set minimum log level
  void SetMinLevel(Level level) { m_minLevel = level; }

  bool ShouldLog(Level level) const {
#ifdef DISABLE_LOGGING
    return false;
#endif
    return static_cast<int>(level) >= static_cast<int>(m_minLevel);
  }

  ~Logger() {
#ifdef __SWITCH__
    if (m_file) {
      fclose(m_file);
    }
#endif
  }

private:
  Logger() : m_minLevel(Level::DEBUG) {
#ifdef __SWITCH__
    m_file = nullptr;
#endif
    // Initialize default category states
    InitializeCategories();
  }

  void InitializeCategories() {
    // Configure which categories are enabled by default
    // You can change these at compile time or runtime
#ifdef _DEBUG
    m_categoryStates["EGL"] = true;
    m_categoryStates["CORE"] = true;
    m_categoryStates["RENDER"] = false; // Disable noisy render logs
    m_categoryStates["INPUT"] = true;
    m_categoryStates["AUDIO"] = true;
    m_categoryStates["LOADER"] = true;
    m_categoryStates["EMULATOR"] = true;
    m_categoryStates["HOME"] = true;
#else
    // Release build: only errors and warnings
    m_categoryStates["EGL"] =
        true; // Enabled for now based on user debugging needs
    m_categoryStates["CORE"] = true;
    m_categoryStates["RENDER"] = false;
    m_categoryStates["INPUT"] = false;
    m_categoryStates["AUDIO"] = false;
    m_categoryStates["LOADER"] = true;
    m_categoryStates["EMULATOR"] = true;
    m_categoryStates["HOME"] = true;
#endif
  }

  const char *LevelToString(Level level) const {
    switch (level) {
    case Level::DEBUG:
      return "DEBUG";
    case Level::INFO:
      return "INFO";
    case Level::WARNING:
      return "WARN";
    case Level::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
    }
  }

  std::unordered_map<std::string, bool> m_categoryStates;
  Level m_minLevel;

#ifdef __SWITCH__
  FILE *m_file;
#endif
};

// Convenience macros with category filtering built-in
#ifdef DISABLE_LOGGING
#define LOG_DEBUG(cat, ...)                                                    \
  do {                                                                         \
  } while (0)
#define LOG_INFO(cat, ...)                                                     \
  do {                                                                         \
  } while (0)
#define LOG_WARN(cat, ...)                                                     \
  do {                                                                         \
  } while (0)
#define LOG_ERROR(cat, ...)                                                    \
  do {                                                                         \
  } while (0)
#define LOG_DEBUG_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
  } while (0)
#define LOG_INFO_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
  } while (0)
#define LOG_WARN_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
  } while (0)
#define LOG_ERROR_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
  } while (0)
#else
#define LOG_DEBUG(cat, ...)                                                    \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::DEBUG)) {                  \
      Logger::Instance().Log(Logger::Level::DEBUG, cat, Logger::None,          \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_INFO(cat, ...)                                                     \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::INFO)) {                   \
      Logger::Instance().Log(Logger::Level::INFO, cat, Logger::None,           \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_WARN(cat, ...)                                                     \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::WARNING)) {                \
      Logger::Instance().Log(Logger::Level::WARNING, cat, Logger::None,        \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_ERROR(cat, ...)                                                    \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::ERROR)) {                  \
      Logger::Instance().Log(Logger::Level::ERROR, cat, Logger::None,          \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

// Macros with flags support
#define LOG_DEBUG_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::DEBUG)) {                  \
      Logger::Instance().Log(Logger::Level::DEBUG, cat, flags, __VA_ARGS__);   \
    }                                                                          \
  } while (0)

#define LOG_INFO_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::INFO)) {                   \
      Logger::Instance().Log(Logger::Level::INFO, cat, flags, __VA_ARGS__);    \
    }                                                                          \
  } while (0)

#define LOG_WARN_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::WARNING)) {                \
      Logger::Instance().Log(Logger::Level::WARNING, cat, flags, __VA_ARGS__); \
    }                                                                          \
  } while (0)

#define LOG_ERROR_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::ERROR)) {                  \
      Logger::Instance().Log(Logger::Level::ERROR, cat, flags, __VA_ARGS__);   \
    }                                                                          \
  } while (0)
#endif

// Category-specific convenience macros (optional)
#define LOG_EGL(...) LOG_DEBUG("EGL", __VA_ARGS__)
#define LOG_CORE(...) LOG_DEBUG("CORE", __VA_ARGS__)
#define LOG_RENDER(...) LOG_DEBUG("RENDER", __VA_ARGS__)
#define LOG_INPUT(...) LOG_DEBUG("INPUT", __VA_ARGS__)
#define LOG_AUDIO(...) LOG_DEBUG("AUDIO", __VA_ARGS__)
