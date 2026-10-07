// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

/// @file Discs.cpp
/// @brief Finds the other discs of a multi-disc game for the overlay's Change
/// Disc menu: the entries of an .m3u, or sibling "(Disc N)" images.

#include "DolphinNX/Discs.h"

#include "overlay/translation_manager.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <map>

namespace DolphinNX::Discs
{
std::string NormalizeDiscPath(const std::string &path)
{
    std::string result = path;
    for (char &c : result)
    {
        if (c == '\\')
            c = '/';
    }
    return result;
}

namespace
{

std::string DiscLabel()
{
    const std::string label = SwitchFrontend::OverlayTranslation::tr("emulator_disc");
    return label == "emulator_disc" ? "Disc" : label;
}

int GetDiscExtensionPriority(const std::string &ext)
{
    // GameCube and Wii images Dolphin reads, most compact first
    if (ext == ".rvz") return 1;
    if (ext == ".iso") return 2;
    if (ext == ".gcm") return 3;
    if (ext == ".ciso") return 4;
    if (ext == ".gcz") return 5;
    if (ext == ".wia") return 6;
    if (ext == ".wbfs") return 7;
    if (ext == ".chd") return 8;
    return 99;
}

}  // namespace

std::vector<DiscEntry> ScanDiscs(std::string currentPath)
{
    std::vector<DiscEntry> discs;
    if (currentPath.empty()) return discs;

    // Strip quotes if present
    if (currentPath.front() == '"' && currentPath.back() == '"')
    {
        currentPath = currentPath.substr(1, currentPath.size() - 2);
    }

    currentPath = NormalizeDiscPath(currentPath);

    std::string dirname;
    std::string basename;
    size_t lastSlash = currentPath.find_last_of("/\\");
    if (lastSlash != std::string::npos)
    {
        dirname = currentPath.substr(0, lastSlash);
        basename = currentPath.substr(lastSlash + 1);
    }
    else
    {
        dirname = ".";
        basename = currentPath;
    }

    std::string lowerBase = basename;
    std::transform(lowerBase.begin(), lowerBase.end(), lowerBase.begin(), ::tolower);

    // M3U Parsing
    if (lowerBase.length() >= 4 && lowerBase.substr(lowerBase.length() - 4) == ".m3u")
    {
        FILE *fp = fopen(currentPath.c_str(), "r");
        if (fp)
        {
            char line[1024];
            int discIndex = 1;
            while (fgets(line, sizeof(line), fp))
            {
                std::string strLine(line);
                strLine.erase(strLine.find_last_not_of(" \n\r\t") + 1);
                size_t startpos = strLine.find_first_not_of(" \n\r\t");
                if (std::string::npos != startpos)
                    strLine = strLine.substr(startpos);
                else
                    strLine.clear();

                if (strLine.empty() || strLine[0] == '#') continue;

                std::string discRelPath = dirname + "/" + strLine;
                std::string normalizedPath = NormalizeDiscPath(discRelPath);

                DiscEntry entry;
                entry.displayName = DiscLabel() + " " + std::to_string(discIndex);
                entry.romPath = normalizedPath;
                discs.push_back(entry);
                discIndex++;
            }
            fclose(fp);
            return discs;
        }
    }

    // Check for disc pattern in current filename
    const char *keywords[] = {"disc", "disk", "cd"};
    bool foundPat = false;
    for (const char *kw : keywords)
    {
        for (int n = 1; n <= 10; n++)
        {
            std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
            if (lowerBase.find(pattern) != std::string::npos)
            {
                foundPat = true;
                break;
            }
        }
        if (foundPat) break;
    }

    if (!foundPat) return discs;

    // Extract prefix (everything before first parenthesis)
    size_t firstParen = lowerBase.find('(');
    std::string prefix = lowerBase;
    if (firstParen != std::string::npos)
        prefix = lowerBase.substr(0, firstParen);
    prefix.erase(prefix.find_last_not_of(" \n\r\t") + 1);

    // Determine directories to scan
    std::vector<std::string> scanDirs;
    scanDirs.push_back(dirname);

    std::string lowerDir = dirname;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
    bool isNested = false;
    for (const char *kw : keywords)
    {
        if (lowerDir.find(kw) != std::string::npos)
        {
            isNested = true;
            break;
        }
    }
    if (isNested)
        scanDirs.push_back(dirname + "/..");

    // Scan directories
    std::map<std::string, DiscEntry> bestDiscs;

    for (const auto &scanDir : scanDirs)
    {
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(scanDir.c_str())) != NULL)
        {
            while ((ent = readdir(dir)) != NULL)
            {
                std::string filename = ent->d_name;
                if (filename == "." || filename == "..") continue;

                std::string currentFileDir = scanDir;
                bool isSubDirItem = false;

                if (ent->d_type == DT_DIR)
                {
                    currentFileDir = scanDir + "/" + filename;
                    isSubDirItem = true;
                }

                DIR *subDir = NULL;
                struct dirent *subEnt = NULL;
                bool hasSubDir = false;

                if (isSubDirItem)
                {
                    subDir = opendir(currentFileDir.c_str());
                    if (subDir)
                    {
                        hasSubDir = true;
                        subEnt = readdir(subDir);
                    }
                    else
                        continue;
                }
                else
                {
                    subEnt = ent;
                }

                while (subEnt != NULL)
                {
                    std::string actualFilename = subEnt->d_name;
                    if (actualFilename == "." || actualFilename == "..")
                    {
                        if (hasSubDir) { subEnt = readdir(subDir); continue; }
                        else break;
                    }

                    std::string lowerFilename = actualFilename;
                    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);

                    std::string extFound = "";
                    size_t lastDot = lowerFilename.find_last_of('.');
                    if (lastDot != std::string::npos)
                        extFound = lowerFilename.substr(lastDot);

                    int priority = GetDiscExtensionPriority(extFound);

                    // Strip extension for prefix comparison
                    std::string lowerNameNoExt = lowerFilename;
                    if (lastDot != std::string::npos) lowerNameNoExt = lowerFilename.substr(0, lastDot);

                    // Must start with prefix (not just contain it)
                    if (priority <= 8 && lowerNameNoExt.find(prefix) == 0)
                    {
                        // Reject extra title words between prefix and first '('
                        std::string afterPrefix = lowerNameNoExt.substr(prefix.size());

                        // Remove anything inside [] entirely
                        int bracketDepth = 0;
                        std::string cleanAfterPrefix;
                        for (size_t i = 0; i < afterPrefix.length(); i++)
                        {
                            if (afterPrefix[i] == '[') bracketDepth++;
                            else if (afterPrefix[i] == ']') bracketDepth = std::max(0, bracketDepth - 1);
                            else if (bracketDepth == 0) cleanAfterPrefix += afterPrefix[i];
                        }

                        size_t parenPos = cleanAfterPrefix.find('(');
                        std::string beforeParen = (parenPos != std::string::npos)
                                                      ? cleanAfterPrefix.substr(0, parenPos)
                                                      : cleanAfterPrefix;
                        beforeParen.erase(beforeParen.find_last_not_of(" \t") + 1);
                        beforeParen.erase(0, beforeParen.find_first_not_of(" \t"));

                        if (beforeParen.empty())
                        {
                            bool foundDiscForFile = false;
                            for (const char *kw : keywords)
                            {
                                for (int n = 1; n <= 10; n++)
                                {
                                    std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
                                    if (lowerFilename.find(pattern) != std::string::npos)
                                    {
                                        DiscEntry entry;
                                        entry.displayName = DiscLabel() + " " + std::to_string(n);
                                        entry.romPath = NormalizeDiscPath(currentFileDir + "/" + actualFilename);

                                        if (bestDiscs.find(entry.displayName) == bestDiscs.end())
                                        {
                                            bestDiscs[entry.displayName] = entry;
                                        }
                                        else
                                        {
                                            std::string existingExt = "";
                                            std::string existingLower = bestDiscs[entry.displayName].romPath;
                                            std::transform(existingLower.begin(), existingLower.end(), existingLower.begin(), ::tolower);
                                            size_t extDot = existingLower.find_last_of('.');
                                            if (extDot != std::string::npos) existingExt = existingLower.substr(extDot);

                                            if (priority < GetDiscExtensionPriority(existingExt))
                                                bestDiscs[entry.displayName] = entry;
                                        }
                                        foundDiscForFile = true;
                                        break;
                                    }
                                }
                                if (foundDiscForFile) break;
                            }
                        }
                    }

                    if (hasSubDir)
                        subEnt = readdir(subDir);
                    else
                        break;
                }

                if (hasSubDir && subDir)
                    closedir(subDir);
            }
            closedir(dir);
        }
    }

    for (const auto &pair : bestDiscs)
        discs.push_back(pair.second);

    // Sort by display name
    std::sort(discs.begin(), discs.end(), [](const DiscEntry &a, const DiscEntry &b) {
        return a.displayName < b.displayName;
    });

    return discs;
}
}  // namespace DolphinNX::Discs
