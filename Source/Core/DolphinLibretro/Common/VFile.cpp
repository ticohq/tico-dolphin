#include <algorithm>
#include <string>
#include <filesystem>
#include <cstdio>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <stack>

#include "VFile.h"
#include "Globals.h"
#include "Common/DirectIOFile.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/CommonPaths.h"

#include <libretro.h>
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/Log.h"

#ifdef _WIN32
#undef stat
#endif

#define LIBRETRO_VFS_INTERFACE_VERSION 4

namespace Libretro
{
namespace VFile
{
retro_vfs_interface* vfs_interface = nullptr;
static bool initialized = false;
static bool is_enabled = true;

void Init()
{
  is_enabled = Libretro::GetOption<bool>(Options::retroarch_core::ENABLE_LIBRETRO_VFS, /*def=*/true);

  if (!is_enabled)
    return;

  if (initialized)
    return;

  retro_vfs_interface_info vfs_info{};
  vfs_info.required_interface_version = LIBRETRO_VFS_INTERFACE_VERSION;
  vfs_info.iface = nullptr;

  if (Libretro::environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info))
  {
    if (vfs_info.iface != nullptr)
    {
        vfs_interface = vfs_info.iface;
        initialized = true;
    }
  }

  if (!vfs_interface)
  {
    WARN_LOG_FMT(BOOT, "LibRetro VFS V4 not available, using standard file I/O");
  }
}

bool HasVFS()
{
  return is_enabled && initialized && vfs_interface;
}

// Dolphin expects to be able to use "/" (DIR_SEP) everywhere.
// RetroArch uses the OS separator.
// Convert between them when switching between systems.
std::string NormalizePath(const std::string& path)
{
  std::string newPath = path;
#ifdef _MSC_VER
  namespace fs = std::filesystem;
  constexpr fs::path::value_type os_separator = fs::path::preferred_separator;
  static_assert(os_separator == DIR_SEP_CHR || os_separator == '\\',
                "Unsupported path separator");
  if (os_separator != DIR_SEP_CHR)
    std::replace(newPath.begin(), newPath.end(), '\\', DIR_SEP_CHR);
#endif
  return newPath;
}

std::string DenormalizePath(const std::string& path)
{
  std::string newPath = path;
#ifdef _MSC_VER
  namespace fs = std::filesystem;
  constexpr fs::path::value_type os_separator = fs::path::preferred_separator;
  static_assert(os_separator == DIR_SEP_CHR || os_separator == '\\',
                "Unsupported path separator");
  if (os_separator != DIR_SEP_CHR)
    std::replace(newPath.begin(), newPath.end(), DIR_SEP_CHR, '\\');
#endif
  return newPath;
}

// ============================================================================
// File / DirectIOFile VFS Implementation
// ============================================================================
bool Exists(const std::string& path)
{
  int64_t size = -1;
  int flags = vfs_interface->stat_64(path.c_str(), &size);

  bool valid = (flags & RETRO_VFS_STAT_IS_VALID) != 0;

  DEBUG_LOG_FMT(COMMON, "Exists {}: {}", path, valid);

  return valid;
}

void FileInfo(std::filesystem::file_status& status, uintmax_t& size, bool& exists,
              const char* path)
{
  DEBUG_LOG_FMT(COMMON, "FileInfo {}", path);

  using namespace std;
  namespace fs = std::filesystem;

  // Defaults: not found
  exists = false;
  size = 0;
  status.type(fs::file_type::none);

  if (!vfs_interface)
    return;

  int64_t tmp_size = 0;
  int flags = vfs_interface->stat_64(path, &tmp_size);

  if (!(flags & RETRO_VFS_STAT_IS_VALID))
    return;

  exists = true;

  if (flags & RETRO_VFS_STAT_IS_DIRECTORY)
    status.type(fs::file_type::directory);
  else
  {
    status.type(fs::file_type::regular);
    size = static_cast<uintmax_t>(tmp_size);
  }
}

bool IsDirectory(const std::string& path)
{
  if (!vfs_interface)
    return false;

  int64_t size = -1;
  int flags = vfs_interface->stat_64(path.c_str(), &size);

  return (flags & RETRO_VFS_STAT_IS_VALID) && (flags & RETRO_VFS_STAT_IS_DIRECTORY);
}

bool IsFile(const std::string& path)
{
  if (!vfs_interface)
    return false;

  int64_t size = -1;
  int flags = vfs_interface->stat_64(path.c_str(), &size);

  return (flags & RETRO_VFS_STAT_IS_VALID) && !(flags & RETRO_VFS_STAT_IS_DIRECTORY);
}

u64 GetSize(const std::string& path)
{
  DEBUG_LOG_FMT(COMMON, "GetSize {}", path);

  if (!vfs_interface)
    return 0;

  int64_t size = 0;

  int flags = vfs_interface->stat_64(path.c_str(), &size);

  if (flags & RETRO_VFS_STAT_IS_VALID)
  {
    DEBUG_LOG_FMT(COMMON, "GetSize returning {}", static_cast<u64>(size));
    return static_cast<u64>(size);
  }

  return 0;
}

u64 GetSize(::retro_vfs_file_handle* handle) 
{
  DEBUG_LOG_FMT(COMMON, "GetSize (VFS) handle");

  if (!vfs_interface || !handle)
    return 0;

  int64_t sz = vfs_interface->size(handle);
  return sz >= 0 ? static_cast<u64>(sz) : 0;
}

bool Delete(const std::string& path)
{
  DEBUG_LOG_FMT(COMMON, "Delete {}", path);

  if (!vfs_interface)
    return false;

  return vfs_interface->remove(path.c_str()) == 0;
}

bool CreateDir(const std::string& path)
{
  DEBUG_LOG_FMT(COMMON, "CreateDir {}", path);

  if (!vfs_interface)
    return false;

  return vfs_interface->mkdir(path.c_str()) == 0;
}

bool DeleteDir(const std::string& path, bool recursive)
{
  DEBUG_LOG_FMT(COMMON, "DeleteDir {} recursive={}", path, recursive);

  if (!vfs_interface)
    return false;

  if (!recursive)
  {
    retro_vfs_dir_handle* dir = vfs_interface->opendir(path.c_str(), false);
    if (dir)
    {
      if (vfs_interface->closedir)
        vfs_interface->closedir(dir);
      if (vfs_interface->remove)
        return vfs_interface->remove(path.c_str()) == 0;
    }
    return false;
  }

  // Recursive delete
  retro_vfs_dir_handle* dir = vfs_interface->opendir(path.c_str(), false);
  if (!dir)
    return false;

  bool ok = true;

  while (vfs_interface->readdir(dir))
  {
    const char* name = vfs_interface->dirent_get_name(dir);
    if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    std::string child_path = path + "/" + name;
    if (vfs_interface->dirent_is_dir(dir))
    {
      if (!DeleteDir(child_path, true))
        ok = false;
    }
    else
    {
      if (vfs_interface->remove(child_path.c_str()) != 0)
        ok = false;
    }
  }

  vfs_interface->closedir(dir);

  if (vfs_interface->remove(path.c_str()) != 0)
    ok = false;

  return ok;
}

bool Rename(const std::string& src, const std::string& dst)
{
  DEBUG_LOG_FMT(COMMON, "Rename {} {}", src, dst);

  if (!vfs_interface)
    return false;

  // IOS_FS: Failed to rename temporary FST file otherwise
  if (Exists(dst))
  {
    if(!Delete(dst))
      WARN_LOG_FMT(COMMON, "Rename failed src: {} dest: {}", src, dst);
  }

  return vfs_interface->rename(src.c_str(), dst.c_str()) == 0;
}

bool CopyRegularFile(std::string_view src,
                     std::string_view dst,
                     bool overwrite_existing)
{
  DEBUG_LOG_FMT(COMMON, "CopyRegularFile {} {} overwrite={}", src, dst, overwrite_existing);

  if (!vfs_interface)
    return false;

  std::string src_str(src);
  std::string dst_str(dst);

  // Open source for reading
  retro_vfs_file_handle* in = vfs_interface->open(src_str.c_str(),
      RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if (!in)
    return false;

  // If not overwriting, check existence first
  if (!overwrite_existing && Exists(dst_str))
  {
    vfs_interface->close(in);
    return false;
  }

  // Open destination for writing (will truncate if exists)
  retro_vfs_file_handle* out = vfs_interface->open(dst_str.c_str(),
      RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if (!out)
  {
    vfs_interface->close(in);
    return false;
  }

  // Copy in chunks
  static constexpr size_t CHUNK = 64 * 1024;
  std::vector<uint8_t> buf(CHUNK);
  bool ok = true;

  for (;;)
  {
    int64_t r = vfs_interface->read(in, buf.data(), buf.size());
    if (r < 0) { ok = false; break; }
    if (r == 0) break; // EOF
    int64_t w = vfs_interface->write(out, buf.data(), static_cast<uint64_t>(r));
    if (w != r) { ok = false; break; }
  }

  vfs_interface->close(out);
  vfs_interface->close(in);
  return ok;
}

bool CreateEmptyFile(const std::string& path)
{
  DEBUG_LOG_FMT(COMMON, "CreateEmptyFile {}", path);

  if (!vfs_interface)
    return false;

  retro_vfs_file_handle* f = vfs_interface->open(path.c_str(),
    RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if (!f) return false;

  return vfs_interface->close(f) == 0;
}

bool WriteStringToFile(const std::string& filename, std::string_view str)
{
  DEBUG_LOG_FMT(COMMON, "WriteStringToFile {}: {}", filename, str);

  if (!vfs_interface)
    return false;

  retro_vfs_file_handle* f = vfs_interface->open(filename.c_str(),
      RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if (!f)
    return false;

  bool ok = (vfs_interface->write(f, str.data(), str.size()) == (int64_t)str.size());
  vfs_interface->close(f);
  return ok;
}

bool ReadFileToString(const std::string& filename, std::string& str)
{
  DEBUG_LOG_FMT(COMMON, "ReadFileToString {}: {}", filename, str);

  if (!vfs_interface)
    return false;

  retro_vfs_file_handle* f = vfs_interface->open(filename.c_str(),
      RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if (!f)
    return false;

  int64_t size = vfs_interface->size(f);
  if (size <= 0)
  {
    vfs_interface->close(f);
    return false;
  }

  str.resize(size);
  int64_t read = vfs_interface->read(f, str.data(), size);
  vfs_interface->close(f);

  return read == size;
}

std::vector<std::string> ReadLines(const std::string& filename)
{
  DEBUG_LOG_FMT(COMMON, "ReadLines {}", filename);

  std::vector<std::string> lines;

  if (!vfs_interface)
    return lines;

  retro_vfs_file_handle* f =
    vfs_interface->open(filename.c_str(),
                        RETRO_VFS_FILE_ACCESS_READ,
                        RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if (!f)
  {
    ERROR_LOG_FMT(COMMON, "ReadLines {} failed to open", filename.c_str());
    return lines;
  }

  int64_t size = vfs_interface->size(f);
  if (size <= 0)
  {
    vfs_interface->close(f);
    return lines;
  }

  std::string buffer(size, '\0');
  int64_t read = vfs_interface->read(f, buffer.data(), size);
  vfs_interface->close(f);

  if (read != size)
    return lines;

  std::stringstream ss(buffer);
  std::string line;

  while (std::getline(ss, line))
    lines.push_back(line);

  return lines;
}

bool CreateDirs(std::string_view path)
{
  DEBUG_LOG_FMT(COMMON, "CreateDirs {}", path);

  if (!vfs_interface)
    return false;

  if (path.empty())
    return false;

  std::string accum;
  size_t i = 0;

  if (path[0] == '/')
    accum = "/";

  while (i < path.size())
  {
    size_t slash = path.find('/', i);
    std::string token = (slash == std::string::npos) ? 
                        std::string(path.substr(i)) : 
                        std::string(path.substr(i, slash - i));

    if (!token.empty() && token != ".")
    {
      if (!accum.empty() && accum.back() != '/')
        accum.push_back('/');
      accum += token;

      int rc = vfs_interface->mkdir(accum.c_str());
      if (rc != 0 && rc != -2) // -2 means already exists
        return false;
    }

    if (slash == std::string::npos)
      break;

    i = slash + 1;
  }

  return true;
}

bool CreateFullPath(std::string_view fullPath)
{
  DEBUG_LOG_FMT(COMMON, "CreateFullPath {}", fullPath);

  if (fullPath.empty())
    return false;

  size_t last = fullPath.rfind('/');
  if (last == std::string::npos)
    return CreateDirs(".");

  return CreateDirs(fullPath.substr(0, last));
}

::File::FSTEntry ScanDirectoryTree(const std::string& directory, bool recursive)
{
  DEBUG_LOG_FMT(COMMON, "ScanDirectoryTree {} {}", directory, recursive);

  ::File::FSTEntry parent_entry;
  parent_entry.physicalName = directory;
  parent_entry.isDirectory  = IsDirectory(directory);
  parent_entry.size = 0;

  if (!vfs_interface)
    return parent_entry;

  retro_vfs_dir_handle* dir = vfs_interface->opendir(directory.c_str(), false);
  if (!dir)
    return parent_entry;

  auto calc_dir_size = [](::File::FSTEntry& entry) {
    entry.size += entry.children.size();
    for (auto& child : entry.children)
      if (child.isDirectory)
        entry.size += child.size;
  };

  if (recursive)
  {
    std::stack<std::string> dir_stack;
    dir_stack.push(directory);

    while (!dir_stack.empty())
    {
      std::string current_path = dir_stack.top();
      dir_stack.pop();

      DEBUG_LOG_FMT(COMMON, "ScanDirectoryTree (recursive) {}", current_path);

      if (current_path.empty())
        continue;

      retro_vfs_dir_handle* d = vfs_interface->opendir(current_path.c_str(), false);
      if (!d)
        continue;

      // Find the entry object corresponding to current_path
      std::function<::File::FSTEntry*(::File::FSTEntry&)> findEntry =
        [&](::File::FSTEntry& e) -> ::File::FSTEntry* {
          if (e.physicalName == current_path)
            return &e;
          for (auto& child : e.children)
          {
            if (auto* found = findEntry(child))
              return found;
          }
          return nullptr;
        };

      ::File::FSTEntry* parent = findEntry(parent_entry);
      if (!parent)
      {
        vfs_interface->closedir(d);
        continue;
      }

      while (vfs_interface->readdir(d))
      {
        const char* name = vfs_interface->dirent_get_name(d);
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
          continue;

        ::File::FSTEntry entry;
        entry.virtualName  = name;
        entry.physicalName = current_path + "/" + name;
        entry.isDirectory  = vfs_interface->dirent_is_dir(d);
        entry.size         = 0;

        parent->children.push_back(entry);

        if (entry.isDirectory)
          dir_stack.push(entry.physicalName);
      }

      vfs_interface->closedir(d);
      calc_dir_size(*parent);
    }
  }
  else
  {
    while (vfs_interface->readdir(dir))
    {
      const char* name = vfs_interface->dirent_get_name(dir);
      if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        continue;

      ::File::FSTEntry entry;
      entry.virtualName  = name;
      entry.physicalName = directory + "/" + name;
      entry.isDirectory  = vfs_interface->dirent_is_dir(dir);
      entry.size         = 0;

      parent_entry.children.push_back(entry);
    }
    calc_dir_size(parent_entry);
  }

  vfs_interface->closedir(dir);

  return parent_entry;
}

// Move a file or directory to a new location, overwriting if it exists.
bool MoveWithOverwrite(const std::string& src, const std::string& dst)
{
  if (!vfs_interface)
    return false;

  // Try direct rename first
  if (vfs_interface->rename(src.c_str(), dst.c_str()) == 0)
    return true;

  // rename failed, try fallbacks

  // Check if src is a directory
  int64_t size = -1;
  int flags = vfs_interface->stat_64(src.c_str(), &size);
  bool is_dir = (flags & RETRO_VFS_STAT_IS_VALID) && (flags & RETRO_VFS_STAT_IS_DIRECTORY);

  if (!is_dir)
  {
    // src is a file: copy + delete
    if (!CopyRegularFile(src, dst, true)) // overwrite_existing = true
      return false;
    if (vfs_interface->remove(src.c_str()) != 0)
      return false;
    return true;
  }

  // src is a directory: recurse into it
  retro_vfs_dir_handle* dir = vfs_interface->opendir(src.c_str(), false);
  if (!dir)
    return false;

  bool ok = true;

  while (vfs_interface->readdir(dir))
  {
    const char* name = vfs_interface->dirent_get_name(dir);
    if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    std::string child_src = src + "/" + name;
    std::string child_dst = dst + "/" + name;

    if (!MoveWithOverwrite(child_src, child_dst))
    {
      ok = false;
      break;
    }
  }

  vfs_interface->closedir(dir);

  // Remove the source directory itself
  if (ok)
  {
    if (vfs_interface->remove(src.c_str()) != 0)
      ok = false;
  }

  return ok;
}

// ============================================================================
// IOFile VFS Implementation
// ============================================================================
::retro_vfs_file_handle* Open(const std::string& filename, const char openmode[])
{
  DEBUG_LOG_FMT(COMMON, "IOFile::Open {} {}", filename, openmode);

  if (!vfs_interface)
    return nullptr;

  unsigned mode = 0;
  unsigned hints = RETRO_VFS_FILE_ACCESS_HINT_NONE;
  
  bool read = false;
  bool write = false;
  bool append = false;
  bool plus = false;
  
  for (const char* p = openmode; *p; ++p)
  {
    switch (*p)
    {
    case 'r': read = true; break;
    case 'w': write = true; break;
    case 'a': append = true; break;
    case '+': plus = true; break;
    case 'b': break;
    }
  }
  if (read && !write && !plus)
  {
    // "r" or "rb" - read only
    mode = RETRO_VFS_FILE_ACCESS_READ;
  }
  else if (write && !read && !plus && !append)
  {
    // "w" or "wb" - write only, truncate
    mode = RETRO_VFS_FILE_ACCESS_WRITE;
  }
  else if (append && !plus)
  {
    // "a" or "ab" - append only
    mode = RETRO_VFS_FILE_ACCESS_WRITE;
  }
  else if (read && plus)
  {
    // "r+" or "r+b" - read and write, file must exist
    mode = RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
  }
  else if (write && plus)
  {
    // "w+" or "w+b" - read and write, truncate
    mode = RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE;
  }
  else if (append && plus)
  {
    // "a+" or "a+b" - read and append
    mode = RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE;
  }
  else
  {
    // Default to read
    mode = RETRO_VFS_FILE_ACCESS_READ;
  }
  
  ::retro_vfs_file_handle* handle = vfs_interface->open(filename.c_str(), mode, hints);
  
  if (handle)
  {
    // Handle write mode - truncate to 0
    if (write && !plus && !append)
    {
      vfs_interface->truncate(handle, 0);
    }
    
    // Handle append mode - seek to end
    if (append)
    {
      vfs_interface->seek(handle, 0, RETRO_VFS_SEEK_POSITION_END);
    }
  }
  
  return handle;
}

// helper: return int status, 0 = success, nonzero = error
int Close(::retro_vfs_file_handle* handle)
{
  DEBUG_LOG_FMT(COMMON, "IOFileClose");

  if (!vfs_interface || !handle)
    return -1;

  return vfs_interface->close(handle);
}

s64 Seek(::retro_vfs_file_handle* handle, s64 offset, ::File::SeekOrigin origin)
{
  int whence = RETRO_VFS_SEEK_POSITION_START;

  switch (origin)
  {
  case ::File::SeekOrigin::Current:
    whence = static_cast<int>(::File::SeekOrigin::Current);
    break;
  case ::File::SeekOrigin::End:
    whence = static_cast<int>(::File::SeekOrigin::End);
    break;
  case ::File::SeekOrigin::Begin:
  default:
    whence = static_cast<int>(::File::SeekOrigin::Begin);
    break;
  }
  
  return Libretro::VFile::Seek(handle, offset, whence);
}

s64 Seek(::retro_vfs_file_handle* handle, s64 offset, int origin)
{
  DEBUG_LOG_FMT(COMMON, "IOFileSeek {} {}", offset, origin);

  if (!vfs_interface || !handle)
    return -1;

  int whence = RETRO_VFS_SEEK_POSITION_START;

  if (origin == SEEK_CUR)
    whence = RETRO_VFS_SEEK_POSITION_CURRENT;
  else if (origin == SEEK_END)
    whence = RETRO_VFS_SEEK_POSITION_END;

  return vfs_interface->seek(handle, offset, whence);
}

s64 Tell(::retro_vfs_file_handle* handle)
{
  DEBUG_LOG_FMT(COMMON, "IOFileTell");

  if (!vfs_interface || !handle)
    return UINT64_MAX;
  
  int64_t pos = vfs_interface->tell(handle);
  return pos >= 0 ? static_cast<u64>(pos) : UINT64_MAX;
}

int Flush(::retro_vfs_file_handle* handle)
{
  DEBUG_LOG_FMT(COMMON, "IOFileFlush");

  if (!vfs_interface || !handle)
    return -1;

  return vfs_interface->flush(handle);
}

s64 Resize(::retro_vfs_file_handle* handle, u64 size)
{
  DEBUG_LOG_FMT(COMMON, "IOFileResize {}", size);

  if (!vfs_interface || !handle)
    return -1;

  return vfs_interface->truncate(handle, size);
}

u64 ReadBytes(::retro_vfs_file_handle* handle, void* data, size_t length)
{
  DEBUG_LOG_FMT(COMMON, "IOFileReadBytes {}", length);

  if (!vfs_interface || !handle)
    return 0;
  
  int64_t result = vfs_interface->read(handle, data, length);
  return result >= 0 ? static_cast<u64>(result) : 0;
}

u64 WriteBytes(::retro_vfs_file_handle* handle, const void* data, size_t length)
{
  DEBUG_LOG_FMT(COMMON, "IOFileWriteBytes {}", length);

  if (!vfs_interface || !handle)
    return 0;
  
  int64_t result = vfs_interface->write(handle, data, length);
  return result >= 0 ? static_cast<u64>(result) : 0;
}

// Helper for DirectIOFile::Open
bool Open(const std::string& path,
                ::File::AccessMode access_mode,
                ::File::OpenMode open_mode,
                retro_vfs_file_handle*& out_handle,
                unsigned& out_mode,
                unsigned& out_hints,
                u64& out_offset)
{
  unsigned mode = 0;

  if (access_mode == ::File::AccessMode::Read)
    mode = RETRO_VFS_FILE_ACCESS_READ;
  else if (access_mode == ::File::AccessMode::Write)
    mode = RETRO_VFS_FILE_ACCESS_WRITE;
  else // ReadAndWrite
    mode = RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

  unsigned hints = RETRO_VFS_FILE_ACCESS_HINT_NONE;

  if (open_mode == ::File::OpenMode::Truncate)
  {
    // VFS doesn't have explicit truncate - we'll handle it after opening
    mode |= RETRO_VFS_FILE_ACCESS_WRITE;
  }
  else if (open_mode == ::File::OpenMode::Create)
  {
    if (Exists(path))
      return false;
  }

  retro_vfs_file_handle* handle = vfs_interface->open(path.c_str(), mode, hints);
  if (!handle)
    return false;

  if (open_mode == ::File::OpenMode::Truncate)
  {
    if (vfs_interface->truncate(handle, 0) != 0)
    {
      vfs_interface->close(handle);
      return false;
    }
  }

  out_handle = handle;
  out_mode   = mode;
  out_hints  = hints;
  out_offset = 0;

  return true;
}

::retro_vfs_file_handle* Open(const std::string& filename, unsigned mode, unsigned hints)
{
  DEBUG_LOG_FMT(COMMON, "IOFile::Open {} {} {}", filename, mode, hints);

  if (!vfs_interface)
    return nullptr;
  
  ::retro_vfs_file_handle* handle = vfs_interface->open(filename.c_str(), mode, hints);
  
  return handle;
}

s64 Truncate(retro_vfs_file_handle* vfs_handle, const u64 size)
{
  if (!vfs_interface || !vfs_handle)
    return 0;

  return vfs_interface->truncate(vfs_handle, size);
}

std::string GetPath(::retro_vfs_file_handle* handle)
{
  if (!vfs_interface || !handle)
    return std::string();

  return Libretro::VFile::vfs_interface->get_path(handle);
}

} // namespace File
} // namespace Libretro
