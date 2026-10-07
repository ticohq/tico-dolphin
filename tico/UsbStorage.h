// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

/// USB drives (FAT32/exFAT, and NTFS read-only, through libusbhsfs + usbntfs)
/// as places games can live.
///
/// A drive is mounted as umsN:/, and N depends on the order drives were found,
/// so a folder on one is stored as usb://<volume-id>/<path>: the id comes from
/// the drive's serial number (or model and size when it has none), partition
/// and filesystem, and stays the same across reconnects. Resolve() turns such a
/// path back into the drive's current umsN:/ path wherever files are opened.
/// tico and the cores share this file, so the stored paths mean the same in both.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UsbStorage {

struct Volume {
    std::string id;    ///< stable volume id
    std::string root;  ///< current mount path, e.g. "ums0:/"
    std::string label; ///< e.g. "SanDisk Ultra (exFAT, 58 GB)"
};

/// Starts USB mass storage support; drives then mount in the background over
/// the next seconds. Safe to call more than once.
bool Init();
/// Unmounts every drive (flushing writes). Call before leaving the app.
void Shutdown();

/// The mounted drives.
std::vector<Volume> Volumes();
/// Changes whenever a drive is connected or removed.
std::uint64_t Generation();

/// True for usb://<id>/... paths.
bool IsToken(const std::string &path);
/// True for paths on a mounted drive (umsN:/...).
bool IsMountPath(const std::string &path);
/// umsN:/... on a known drive -> usb://<id>/...; anything else is returned as is.
std::string ToToken(const std::string &path);
/// usb://<id>/... -> the drive's current umsN:/... path, or "" when that drive
/// is not connected; anything else is returned as is. Right after Init() it
/// waits a few seconds for the drive to finish mounting.
std::string Resolve(const std::string &path);
/// How to show a path: the drive's name in place of usb://<id> or umsN:.
std::string DisplayName(const std::string &path);

} // namespace UsbStorage
