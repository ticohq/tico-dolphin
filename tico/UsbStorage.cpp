// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UsbStorage.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef __SWITCH__
#include <switch.h>
#include <usbhsfs.h>
#endif

namespace UsbStorage {
namespace {

constexpr const char *kTokenPrefix = "usb://";
// how long after Init() a missing drive is still waited for
constexpr std::uint64_t kSettleNs = 4'000'000'000ULL;

std::mutex s_mutex;
std::vector<Volume> s_volumes;
std::uint64_t s_generation = 0;
bool s_initialized = false;

#ifdef __SWITCH__
std::uint64_t s_init_tick = 0;

void HashBytes(std::uint64_t &hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}

void HashText(std::uint64_t &hash, const char *text, std::size_t max) {
    HashBytes(hash, text, strnlen(text, max));
    HashBytes(hash, "", 1);
}

template <typename T> void HashValue(std::uint64_t &hash, T value) {
    HashBytes(hash, &value, sizeof(value));
}

Volume MakeVolume(const UsbHsFsDevice &device) {
    std::uint64_t hash = 14695981039346656037ULL;
    HashValue(hash, device.vid);
    HashValue(hash, device.pid);
    HashText(hash, device.serial_number, sizeof(device.serial_number));
    if (!device.serial_number[0]) {
        // enclosures without a serial: model and size keep the id stable
        HashText(hash, device.manufacturer, sizeof(device.manufacturer));
        HashText(hash, device.product_name, sizeof(device.product_name));
        HashValue(hash, device.capacity);
    }
    HashValue(hash, device.lun);
    HashValue(hash, device.fs_idx);
    HashValue(hash, device.fs_type);

    Volume volume;
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(hash));
    volume.id = id;
    volume.root = device.name;
    if (!volume.root.empty() && volume.root.back() != '/')
        volume.root += '/';

    std::string name = device.product_name;
    if (name.empty())
        name = device.manufacturer;
    if (name.empty())
        name = "USB";
    char label[160];
    std::snprintf(label, sizeof(label), "%s (%s, %llu GB)", name.c_str(),
                  LIBUSBHSFS_FS_TYPE_STR(device.fs_type),
                  static_cast<unsigned long long>(device.capacity / 1000000000ULL));
    volume.label = label;
    return volume;
}

// Runs on libusbhsfs' thread whenever the set of mounted drives changes.
void OnDevicesChanged(const UsbHsFsDevice *devices, u32 count, void *) {
    std::vector<Volume> volumes;
    for (u32 i = 0; devices && i < count; ++i)
        if (devices[i].name[0])
            volumes.push_back(MakeVolume(devices[i]));
    std::lock_guard lock(s_mutex);
    s_volumes = std::move(volumes);
    ++s_generation;
}
#endif

// Splits usb://<id>/<rest> into id and rest.
bool SplitToken(const std::string &path, std::string &id, std::string &rest) {
    if (!IsToken(path))
        return false;
    const std::size_t start = std::strlen(kTokenPrefix);
    const std::size_t slash = path.find('/', start);
    id = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    rest = slash == std::string::npos ? std::string() : path.substr(slash + 1);
    return !id.empty();
}

bool FindVolume(const std::string &id, Volume &out) {
    std::lock_guard lock(s_mutex);
    for (const Volume &volume : s_volumes) {
        if (volume.id == id) {
            out = volume;
            return true;
        }
    }
    return false;
}

} // namespace

bool Init() {
#ifdef __SWITCH__
    {
        std::lock_guard lock(s_mutex);
        if (s_initialized)
            return true;
    }
    usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_ReplayJournal |
                                   UsbHsFsMountFlags_ShowHiddenFiles);
    if (R_FAILED(usbHsFsInitialize(0)))
        return false;
    {
        std::lock_guard lock(s_mutex);
        s_initialized = true;
        s_init_tick = armGetSystemTick();
    }
    usbHsFsSetPopulateCallback(OnDevicesChanged, nullptr);
    UsbHsFsDevice devices[16];
    OnDevicesChanged(devices, usbHsFsListMountedDevices(devices, 16), nullptr);
    return true;
#else
    return false;
#endif
}

void Shutdown() {
#ifdef __SWITCH__
    {
        std::lock_guard lock(s_mutex);
        if (!s_initialized)
            return;
        s_initialized = false;
        s_volumes.clear();
        ++s_generation;
    }
    usbHsFsSetPopulateCallback(nullptr, nullptr);
    usbHsFsExit();
#endif
}

std::vector<Volume> Volumes() {
    std::lock_guard lock(s_mutex);
    return s_volumes;
}

std::uint64_t Generation() {
    std::lock_guard lock(s_mutex);
    return s_generation;
}

bool IsToken(const std::string &path) {
    return path.rfind(kTokenPrefix, 0) == 0;
}

bool IsMountPath(const std::string &path) {
    if (path.rfind("ums", 0) != 0)
        return false;
    std::size_t i = 3;
    while (i < path.size() && path[i] >= '0' && path[i] <= '9')
        ++i;
    return i > 3 && i < path.size() && path[i] == ':';
}

std::string ToToken(const std::string &path) {
    if (!IsMountPath(path))
        return path;
    const std::size_t colon = path.find(':');
    const std::string root = path.substr(0, colon) + ":/";
    std::size_t rest = colon + 1;
    while (rest < path.size() && path[rest] == '/')
        ++rest;
    std::lock_guard lock(s_mutex);
    for (const Volume &volume : s_volumes)
        if (volume.root == root)
            return kTokenPrefix + volume.id + "/" + path.substr(rest);
    return path;
}

std::string Resolve(const std::string &path) {
    std::string id;
    std::string rest;
    if (!SplitToken(path, id, rest))
        return path;
    Volume volume;
#ifdef __SWITCH__
    // drives mount a little after Init(): give them time on the first lookups
    while (!FindVolume(id, volume)) {
        std::uint64_t init_tick;
        {
            std::lock_guard lock(s_mutex);
            if (!s_initialized)
                return {};
            init_tick = s_init_tick;
        }
        if (armTicksToNs(armGetSystemTick() - init_tick) > kSettleNs)
            return {};
        svcSleepThread(50'000'000ULL);
    }
    return volume.root + rest;
#else
    return FindVolume(id, volume) ? volume.root + rest : std::string();
#endif
}

std::string DisplayName(const std::string &path) {
    std::string id;
    std::string rest;
    if (SplitToken(path, id, rest)) {
        Volume volume;
        if (FindVolume(id, volume))
            return volume.label + "/" + rest;
        return "USB (" + id.substr(0, 6) + ")/" + rest;
    }
    if (IsMountPath(path)) {
        const std::size_t colon = path.find(':');
        const std::string root = path.substr(0, colon) + ":/";
        std::lock_guard lock(s_mutex);
        for (const Volume &volume : s_volumes)
            if (volume.root == root)
                return volume.label + path.substr(colon + 1);
    }
    return path;
}

} // namespace UsbStorage
