#pragma once

#include <libretro.h>
#include "Common/MsgHandler.h"

namespace Libretro
{
extern unsigned msg_interface_version;
namespace Log
{
void Init();
void Shutdown();
retro_log_level GetRetroLogLevel(Common::Log::LogLevel level);
retro_log_level GetRetroLogLevelForMsgType(Common::MsgType level);
void DoLogFrontEnd(retro_log_level level, const char* text, unsigned int duration);
void LogFrontEnd(Common::Log::LogLevel level, const char* text, unsigned int duration);
void LogFrontEnd(Common::MsgType style, const char* caption, const char* text, unsigned int duration);
}
}  // namespace Libretro
