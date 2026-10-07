// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

/// @file Library.cpp
/// @brief Lists the GameCube and Wii games found in tico's ROM bases
/// (<base>/<slug>/) and the module's own folders (tico_rom_folders in
/// dolphin.jsonc, edited from Settings > Library), as Flycast's library does.

#include "DolphinNX/Library.h"

#include "TicoUtils.h"
#include "UsbStorage.h"
#include "json.hpp"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <map>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

namespace DolphinNX::Library
{
namespace
{
struct Console
{
    const char *slug;
    const char *title;
    const char *detail;
    std::vector<const char *> extensions;
};

const std::vector<Console> &Consoles()
{
    static const std::vector<Console> consoles = {
        {"gc", "GameCube", "GC",
         {".iso", ".gcm", ".rvz", ".wbfs", ".wia", ".gcz", ".ciso", ".m3u"}},
        {"wii", "Wii", "Wii",
         {".iso", ".gcm", ".rvz", ".wbfs", ".wia", ".gcz", ".ciso", ".m3u", ".wad"}},
        {"triforce", "Triforce", "Triforce",
         {".bin", ".iso", ".gcm", ".rvz", ".gcz", ".ciso", ".wia"}},
    };
    return consoles;
}

std::function<void(const std::string &, const std::string &)> s_launch;
std::map<std::string, std::string> s_slugs; // path -> console it was listed under

std::string LowerExtension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return std::string();
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

std::string WithSlash(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/')
        path += '/';
    return path;
}

// tico's ROM bases (general.jsonc): the ROMs path, then the extra bases.
std::vector<std::string> TicoRomBases()
{
    std::vector<std::string> bases;
    std::ifstream file("sdmc:/tico/config/general.jsonc");
    const nlohmann::json j = file.good() ? nlohmann::json::parse(file, nullptr, false, true)
                                         : nlohmann::json();
    const std::string roms = j.is_object() ? j.value("roms_path", std::string()) : std::string();
    bases.push_back(WithSlash(roms.empty() ? "sdmc:/tico/roms/" : roms));
    if (j.is_object() && j.contains("rom_base_paths") && j["rom_base_paths"].is_array())
        for (const auto &base : j["rom_base_paths"])
            if (base.is_string() && !base.get<std::string>().empty())
                bases.push_back(WithSlash(base.get<std::string>()));
    return bases;
}

nlohmann::json ModuleRomFolders()
{
    const std::string text = OverlayConfig::GetConfigJson("tico_rom_folders");
    nlohmann::json j = text.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

std::vector<std::string> ModuleRomFolders(const std::string &slug)
{
    std::vector<std::string> folders;
    const nlohmann::json all = ModuleRomFolders();
    const auto it = all.find(slug);
    if (it != all.end() && it->is_array())
        for (const auto &entry : *it)
            if (entry.is_string() && !entry.get<std::string>().empty())
                folders.push_back(WithSlash(entry.get<std::string>()));
    return folders;
}

void SetModuleRomFolders(const std::string &slug, const std::vector<std::string> &folders)
{
    nlohmann::json all = ModuleRomFolders();
    if (folders.empty())
        all.erase(slug);
    else
        all[slug] = folders;
    OverlayConfig::SetConfigJson("tico_rom_folders", all.dump());
    OverlayConfig::SaveConfig();
}

// Every folder a console's games are read from; a USB folder only while its
// drive is connected.
std::vector<std::string> RomFoldersFor(const std::string &slug)
{
    std::vector<std::string> folders;
    auto add = [&](const std::string &folder) {
        const std::string mounted = UsbStorage::Resolve(folder);
        if (!mounted.empty() && std::find(folders.begin(), folders.end(), mounted) == folders.end())
            folders.push_back(mounted);
    };
    for (const std::string &base : TicoRomBases())
        add(base + slug + "/");
    for (const std::string &folder : ModuleRomFolders(slug))
        add(folder);
    return folders;
}

void ScanFolder(const std::string &dir, int depth, const Console &console, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = (dir.back() == '/' ? dir : dir + "/") + name;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN)
        {
            struct stat st;
            isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir)
        {
            if (depth > 0)
                ScanFolder(path, depth - 1, console, out);
            continue;
        }
        const std::string ext = LowerExtension(name);
        for (const char *known : console.extensions)
            if (ext == known)
                out.push_back(path);
    }
    closedir(d);
}

std::vector<OverlayUI::LibraryEntry> List()
{
    s_slugs.clear();
    std::vector<OverlayUI::LibraryEntry> entries;
    for (const Console &console : Consoles())
    {
        std::vector<std::string> games;
        for (const std::string &folder : RomFoldersFor(console.slug))
            ScanFolder(folder, 2, console, games);
        std::sort(games.begin(), games.end());
        games.erase(std::unique(games.begin(), games.end()), games.end());
        for (const std::string &path : games)
        {
            if (!s_slugs.emplace(path, console.slug).second)
                continue; // listed under the first console that has it
            const std::string filename = path.substr(path.find_last_of('/') + 1);
            std::string title = TicoUtils::GetCleanTitle(filename);
            if (title.empty())
                title = filename;
            entries.push_back({title, console.detail, path});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
    });
    return entries;
}
} // namespace

void Register(std::function<void(const std::string &path, const std::string &slug)> launch)
{
    s_launch = std::move(launch);

    OverlayUI::LibraryCallbacks library;
    library.list = [] { return List(); };
    library.launch = [](const std::string &path) {
        const auto it = s_slugs.find(path);
        if (s_launch)
            s_launch(path, it != s_slugs.end() ? it->second : std::string("gc"));
    };
    OverlayUI::SetLibraryCallbacks(std::move(library));

    OverlayUI::LibraryFolderCallbacks folders;
    folders.groups = [] {
        std::vector<OverlayUI::LibraryFolderGroup> groups;
        const std::vector<std::string> bases = TicoRomBases();
        for (const Console &console : Consoles())
        {
            OverlayUI::LibraryFolderGroup group;
            group.label = console.title;
            for (const std::string &base : bases)
                group.bases.push_back(base + console.slug + "/");
            group.folders = ModuleRomFolders(console.slug);
            groups.push_back(std::move(group));
        }
        return groups;
    };
    folders.set = [](int group, const std::vector<std::string> &paths) {
        if (group < 0 || group >= (int)Consoles().size())
            return;
        std::vector<std::string> normalized;
        for (const std::string &path : paths)
            normalized.push_back(WithSlash(path));
        SetModuleRomFolders(Consoles()[(size_t)group].slug, normalized);
    };
    OverlayUI::SetLibraryFolderCallbacks(std::move(folders));
}

void Unregister()
{
    OverlayUI::SetLibraryCallbacks({});
    OverlayUI::SetLibraryFolderCallbacks({});
    s_launch = nullptr;
}
}  // namespace DolphinNX::Library
