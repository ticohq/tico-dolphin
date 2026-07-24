#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include <libretro.h>
#include <vfs/vfs.h>
#include <vfs/vfs_implementation.h>

namespace File
{
enum class SeekOrigin;
enum class AccessMode;
enum class OpenMode;
}

namespace Libretro
{
namespace VFile
{
extern retro_vfs_interface* vfs_interface;

// Generic methods
void Init();
bool HasVFS();
std::string NormalizePath(const std::string& path);
std::string DenormalizePath(const std::string& path);

// FileUtil
bool Exists(const std::string& path);
void FileInfo(std::filesystem::file_status& status, uintmax_t& size, bool& exists,
              const char* path);
bool IsDirectory(const std::string& path);
bool IsFile(const std::string& path);
u64 GetSize(const std::string& path);
u64 GetSize(::retro_vfs_file_handle* handle);

bool Delete(const std::string& path);
bool CreateDir(const std::string& path);
bool DeleteDir(const std::string& path, bool recursive = false);
bool Rename(const std::string& src, const std::string& dst);
bool CopyRegularFile(std::string_view src, std::string_view dst, bool overwrite_existing = true);
bool CreateEmptyFile(const std::string& path);
bool WriteStringToFile(const std::string& filename, std::string_view str);
bool ReadFileToString(const std::string& filename, std::string& str);
bool CreateDirs(std::string_view path);
bool CreateFullPath(std::string_view fullPath);
::File::FSTEntry ScanDirectoryTree(const std::string& directory, bool recursive);
bool MoveWithOverwrite(const std::string& src, const std::string& dst);

// IOFile
::retro_vfs_file_handle* Open(const std::string& filename, const char openmode[]);
int Close(::retro_vfs_file_handle* handle);
s64 Seek(::retro_vfs_file_handle* handle, s64 offset, ::File::SeekOrigin origin);
s64 Seek(::retro_vfs_file_handle* handle, s64 offset, int origin);
s64 Tell(::retro_vfs_file_handle* handle);
s64 Resize(::retro_vfs_file_handle* handle, u64 size);
u64 ReadBytes(::retro_vfs_file_handle* handle, void* data, size_t length);
u64 WriteBytes(::retro_vfs_file_handle* handle, const void* data, size_t length);
int Flush(::retro_vfs_file_handle* handle);

// DirectIOFile
bool Open(const std::string& path, ::File::AccessMode access_mode, ::File::OpenMode open_mode,
          retro_vfs_file_handle*& out_handle,
          unsigned& out_mode,
          unsigned& out_hints,
          u64& out_offset);
::retro_vfs_file_handle* Open(const std::string& filename, unsigned mode, unsigned hints);

s64 Truncate(retro_vfs_file_handle* vfs_handle, const u64 size);

// IOS::HLE::FS
std::string GetPath(::retro_vfs_file_handle* handle);

std::vector<std::string> ReadLines(const std::string& filename);

} // namespace File
} // namespace Libretro
