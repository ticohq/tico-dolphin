// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

#ifdef __LIBRETRO__
#include "Common/Logging/Log.h"
#include "DolphinLibretro/Common/VFile.h"
#endif

namespace File
{
enum class SeekOrigin
{
  Begin,
  Current,
  End,
};

enum class SharedAccess
{
  Default,
  Read,
};

// simple wrapper for cstdlib file functions to
// hopefully will make error checking easier
// and make forgetting an fclose() harder
class IOFile
{
public:
  IOFile();
  IOFile(std::FILE* file);
  IOFile(const std::string& filename, const char openmode[],
         SharedAccess sh = SharedAccess::Default);

  ~IOFile();

  IOFile(const IOFile&) = delete;
  IOFile& operator=(const IOFile&) = delete;

  IOFile(IOFile&& other) noexcept;
  IOFile& operator=(IOFile&& other) noexcept;

  void Swap(IOFile& other) noexcept;

  bool Open(const std::string& filename, const char openmode[],
            SharedAccess sh = SharedAccess::Default);
  bool Close();

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool ReadArray(T* elements, size_t count, size_t* num_read = nullptr)
  {
#ifdef __LIBRETRO__
    if (Libretro::VFile::HasVFS())
    {
      if (!IsOpen())
      {
        m_good = false;
        return m_good;
      }

      int64_t bytes_read = Libretro::VFile::ReadBytes(m_vfs_handle,
                                                     elements,
                                                     static_cast<uint64_t>(count * sizeof(T)));

      size_t read_count = (bytes_read >= 0) ? static_cast<size_t>(bytes_read) / sizeof(T) : 0;

      if (read_count != count)
        m_good = false;

      if (num_read)
        *num_read = read_count;

      return m_good;
    }
#endif
    size_t read_count = 0;
    if (!IsOpen() || count != (read_count = std::fread(elements, sizeof(T), count, m_file)))
      m_good = false;

    if (num_read)
      *num_read = read_count;

    return m_good;
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool WriteArray(const T* elements, size_t count)
  {
  #ifdef __LIBRETRO__
    if (Libretro::VFile::HasVFS())
    {
      if (!IsOpen())
      {
        m_good = false;
        return m_good;
      }

      const uint64_t bytes_to_write = static_cast<uint64_t>(count * sizeof(T));
      int64_t bytes_written = Libretro::VFile::WriteBytes(m_vfs_handle, elements, bytes_to_write);

      if (bytes_written < 0)
      {
        m_good = false;
        return m_good;
      }

      size_t write_count = static_cast<size_t>(bytes_written) / sizeof(T);
      if (write_count != count)
      {
        m_good = false;
        return m_good;
      }

      return m_good;
    }
  #endif

    if (!IsOpen() || count != std::fwrite(elements, sizeof(T), count, m_file))
      m_good = false;

    return m_good;
  }

  template <typename T, std::size_t N>
  bool ReadArray(std::array<T, N>* elements, size_t* num_read = nullptr)
  {
    return ReadArray(elements->data(), elements->size(), num_read);
  }

  template <typename T, std::size_t N>
  bool WriteArray(const std::array<T, N>& elements)
  {
    return WriteArray(elements.data(), elements.size());
  }

  bool ReadBytes(void* data, size_t length) { return ReadArray(static_cast<char*>(data), length); }

  bool WriteBytes(const void* data, size_t length)
  {
    return WriteArray(static_cast<const char*>(data), length);
  }

  bool WriteString(std::string_view str) { return WriteBytes(str.data(), str.size()); }

  bool IsOpen() const
  {
#ifdef __LIBRETRO__
    if (Libretro::VFile::HasVFS())
      return nullptr != m_vfs_handle;
#endif
    return nullptr != m_file;
  }
  // m_good is set to false when a read, write or other function fails
  bool IsGood() const { return m_good; }
  explicit operator bool() const { return IsGood() && IsOpen(); }
  std::FILE* GetHandle()
  {
#ifdef __LIBRETRO__
    if (Libretro::VFile::HasVFS())
      ERROR_LOG_FMT(COMMON, "VFS: Attempt to use std::FILE which should not happen in VFS mode");
#endif
    return m_file; 
  }
#ifdef __LIBRETRO__
  retro_vfs_file_handle* GetVFSHandle() { return m_vfs_handle; }
#endif
  void SetHandle(std::FILE* file);

  bool Seek(s64 offset, SeekOrigin origin);
  u64 Tell() const;
  u64 GetSize() const;
  bool Resize(u64 size);
  bool Flush();

  // clear error state
  void ClearError()
  {
    m_good = true;
#ifdef __LIBRETRO__
    if (Libretro::VFile::HasVFS())
      return;
#endif
    if (IsOpen())
      std::clearerr(m_file);
  }

private:
  std::FILE* m_file;
  bool m_good;

#ifdef __LIBRETRO__
  retro_vfs_file_handle* m_vfs_handle = nullptr;
#endif
};

}  // namespace File
