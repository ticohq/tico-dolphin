/* usbntfs: read-only NTFS through a sector callback. MIT OR Apache-2.0. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Volume UsbNtfsVolume;
typedef struct Dir UsbNtfsDir;

typedef struct {
    uint64_t record; /* MFT record number: the file's handle for usbntfs_read */
    uint64_t size;
    uint64_t mtime;  /* 100 ns intervals since 1601-01-01 */
    bool is_dir;
} UsbNtfsStat;

/* Reads `count` sectors at `lba` into `buf`; true on success. */
typedef bool (*UsbNtfsReadSectors)(void *user, uint64_t lba, uint32_t count, uint8_t *buf);

/* Null when the volume is not readable NTFS. Not thread-safe: serialise calls. */
UsbNtfsVolume *usbntfs_mount(UsbNtfsReadSectors read, void *user, uint32_t sector_size, uint64_t size);
void usbntfs_unmount(UsbNtfsVolume *volume);

bool usbntfs_label(UsbNtfsVolume *volume, char *out, size_t cap);
void usbntfs_geometry(UsbNtfsVolume *volume, uint64_t *size, uint32_t *cluster);

/* Paths are /-separated from the volume root, matched case-insensitively. */
bool usbntfs_stat(UsbNtfsVolume *volume, const char *path, UsbNtfsStat *out);
bool usbntfs_stat_record(UsbNtfsVolume *volume, uint64_t record, UsbNtfsStat *out);
/* Bytes read (0 at the end), or -1 on error. */
int64_t usbntfs_read(UsbNtfsVolume *volume, uint64_t record, uint64_t offset, uint8_t *buf, size_t len);

UsbNtfsDir *usbntfs_opendir(UsbNtfsVolume *volume, const char *path);
bool usbntfs_readdir(UsbNtfsDir *dir, char *name, size_t cap, UsbNtfsStat *out);
void usbntfs_rewinddir(UsbNtfsDir *dir);
void usbntfs_closedir(UsbNtfsDir *dir);

#ifdef __cplusplus
}
#endif
