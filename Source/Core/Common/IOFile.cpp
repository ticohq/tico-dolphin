// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/IOFile.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include "Common/CommonFuncs.h"
#include "Common/StringUtil.h"
#else
#include <unistd.h>
#endif

#ifdef ANDROID
#include <algorithm>

#include "jni/AndroidCommon/AndroidCommon.h"
#endif

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"

#ifdef __LIBRETRO__
#include "DolphinLibretro/Common/VFile.h"
#endif

namespace File
{
IOFile::IOFile() : m_file(nullptr), m_good(true)
#ifdef __LIBRETRO__
, m_vfs_handle{nullptr}
#endif
{
}

IOFile::IOFile(std::FILE* file) : m_file(file), m_good(true)
{
#ifdef __LIBRETRO__
  m_vfs_handle = nullptr;

  if (Libretro::VFile::HasVFS())
    ERROR_LOG_FMT(COMMON, "VFS: Attempt to use IOFile::IOFile with a std::FILE which should not happen");
#endif
}

IOFile::IOFile(const std::string& filename, const char openmode[], SharedAccess sh)
    : m_file(nullptr), m_good(true)
#ifdef __LIBRETRO__
, m_vfs_handle{nullptr}
#endif
{
  Open(filename, openmode, sh);
}

IOFile::~IOFile()
{
  Close();
}

IOFile::IOFile(IOFile&& other) noexcept : m_file(nullptr), m_good(true)
#ifdef __LIBRETRO__
, m_vfs_handle{nullptr}
#endif
{
  Swap(other);
}

IOFile& IOFile::operator=(IOFile&& other) noexcept
{
  Swap(other);
  return *this;
}

void IOFile::Swap(IOFile& other) noexcept
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
    std::swap(m_vfs_handle, other.m_vfs_handle);
#endif
  std::swap(m_file, other.m_file);
  std::swap(m_good, other.m_good);
}

bool IOFile::Open(const std::string& filename, const char openmode[],
                  [[maybe_unused]] SharedAccess sh)
{
  Close();

#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    m_vfs_handle = Libretro::VFile::Open(filename, openmode);
    m_good = m_vfs_handle != nullptr;
    return m_good;
  }
#endif

#ifdef _WIN32
  if (sh == SharedAccess::Default)
  {
    m_good = _tfopen_s(&m_file, UTF8ToTStr(filename).c_str(), UTF8ToTStr(openmode).c_str()) == 0;
  }
  else if (sh == SharedAccess::Read)
  {
    m_file = _tfsopen(UTF8ToTStr(filename).c_str(), UTF8ToTStr(openmode).c_str(), SH_DENYWR);
    m_good = m_file != nullptr;
  }
#else
#ifdef ANDROID
  if (IsPathAndroidContent(filename))
    m_file = fdopen(OpenAndroidContent(filename, OpenModeToAndroid(openmode)), openmode);
  else
#endif
    m_file = std::fopen(filename.c_str(), openmode);

  m_good = m_file != nullptr;
#endif

  return m_good;
}

bool IOFile::Close()
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (!IsOpen() || 0 != Libretro::VFile::Close(m_vfs_handle))
      m_good = false;

    m_vfs_handle = nullptr;
    return m_good;
  }
#endif

  if (!IsOpen() || 0 != std::fclose(m_file))
    m_good = false;

  m_file = nullptr;
  return m_good;
}

void IOFile::SetHandle(std::FILE* file)
{
  Close();
  ClearError();
  m_file = file;
}

u64 IOFile::GetSize() const
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (IsOpen())
      return Libretro::VFile::GetSize(m_vfs_handle);
    else
      return 0;
  }
#endif

  if (IsOpen())
    return File::GetSize(m_file);
  else
    return 0;
}

bool IOFile::Seek(s64 offset, SeekOrigin origin)
{
  int fseek_origin;
  switch (origin)
  {
  case SeekOrigin::Begin:
    fseek_origin = SEEK_SET;
    break;
  case SeekOrigin::Current:
    fseek_origin = SEEK_CUR;
    break;
  case SeekOrigin::End:
    fseek_origin = SEEK_END;
    break;
  default:
    return false;
  }

#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (!IsOpen() || 0 != Libretro::VFile::Seek(m_vfs_handle, offset, fseek_origin))
      m_good = false;

    return m_good;
  }
#endif

  if (!IsOpen() || 0 != fseeko(m_file, offset, fseek_origin))
    m_good = false;

  return m_good;
}

u64 IOFile::Tell() const
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (IsOpen())
      return Libretro::VFile::Tell(m_vfs_handle);
    else
      return UINT64_MAX;
  }
#endif
  if (IsOpen())
    return ftello(m_file);
  else
    return UINT64_MAX;
}

bool IOFile::Flush()
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (!IsOpen() || 0 != Libretro::VFile::Flush(m_vfs_handle))
      m_good = false;

    return m_good;
  }
#endif
  if (!IsOpen() || 0 != std::fflush(m_file))
    m_good = false;

  return m_good;
}

bool IOFile::Resize(u64 size)
{
#ifdef __LIBRETRO__
  if (Libretro::VFile::HasVFS())
  {
    if (!IsOpen() || 0 != Libretro::VFile::Resize(m_vfs_handle, size))
      m_good = false;

    return m_good;
  }
#endif

#ifdef _WIN32
  if (!IsOpen() || 0 != _chsize_s(_fileno(m_file), size))
#else
  // TODO: handle 64bit and growing
  if (!IsOpen() || 0 != ftruncate(fileno(m_file), size))
#endif
    m_good = false;

  return m_good;
}

}  // namespace File
