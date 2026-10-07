/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * What stat said about paths on the SD card, and that a path is not there.
 * libnx answers a stat with up to five requests to the FS service (the entry
 * type, opening the file, its size, closing it, its times), all on the system
 * core. Wine looks a path up one directory at a time, and F.E.A.R. asks for
 * PunkBuster's pb/pbcl.dll and pb/pbclnew.dll, which are not there, many times
 * a second for as long as it runs: 340 stats a second, half the system core.
 *
 * Every change to the card goes through the same device (sd_cache.c), which
 * forgets a path when it is created, written, truncated or deleted, and
 * everything when a path is renamed or a directory removed, as the paths under
 * it change too.
 *
 * Pure logic, so tests/sd_stat_cache.c can drive it on the host.
 */
#ifndef WINE_NX_SD_STAT_CACHE_H
#define WINE_NX_SD_STAT_CACHE_H

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sd_read_cache.h"  /* sd_cache_same_path */

#define SD_STAT_SLOTS 512
#define SD_STAT_WAYS  4      /* slots a path may take, from its hash on */

struct sd_stat_entry
{
    char *path;              /* NULL for a free slot */
    unsigned int hash;
    int error;               /* 0, or ENOENT for a path that is not there */
    unsigned int used;       /* when it was last asked for */
    struct stat st;
};

struct sd_stat_cache
{
    struct sd_stat_entry slots[SD_STAT_SLOTS];
    unsigned int clock;
    unsigned int hits, misses;
};

/* The hash of a path as sd_cache_same_path compares it: without the device,
 * repeated or trailing slashes, and case. */
static inline unsigned int sd_stat_hash( const char *path )
{
    unsigned int hash = 2166136261u;

    if (!strncmp( path, "sdmc:", 5 )) path += 5;
    for (;;)
    {
        char c;

        while (path[0] == '/' && (path[1] == '/' || !path[1])) path++;
        if (!(c = *path++)) return hash;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash = (hash ^ (unsigned char)c) * 16777619u;
    }
}

/* Only whole paths: a relative one means something else in another directory. */
static inline int sd_stat_cacheable( const char *path )
{
    return path[0] == '/' || !strncmp( path, "sdmc:/", 6 );
}

/* A name the card's file system cannot hold, so the path is not there. Wine
 * looks for a reparse point as the name with '?' after it (file.c) every time
 * a file is missing, and the card answers that with an error of its own that
 * says nothing about whether the path exists, so it would be asked every time. */
static inline int sd_stat_impossible( const char *path )
{
    if (!strncmp( path, "sdmc:", 5 )) path += 5;
    return strpbrk( path, "?*<>|\"\\:" ) != NULL;
}

static inline struct sd_stat_entry *sd_stat_find( struct sd_stat_cache *cache, const char *path, unsigned int hash )
{
    unsigned int i;

    for (i = 0; i < SD_STAT_WAYS; i++)
    {
        struct sd_stat_entry *entry = &cache->slots[(hash + i) % SD_STAT_SLOTS];

        if (entry->path && entry->hash == hash && sd_cache_same_path( entry->path, path )) return entry;
    }
    return NULL;
}

/* 1 with what stat said, in *st or *error; 0 when the card has to be asked. */
static inline int sd_stat_lookup( struct sd_stat_cache *cache, const char *path, struct stat *st, int *error )
{
    struct sd_stat_entry *entry;

    if (sd_stat_impossible( path ))
    {
        cache->hits++;
        *error = ENOENT;
        return 1;
    }
    if (!sd_stat_cacheable( path ) || !(entry = sd_stat_find( cache, path, sd_stat_hash( path ) )))
    {
        cache->misses++;
        return 0;
    }
    entry->used = ++cache->clock;
    cache->hits++;
    *error = entry->error;
    if (!entry->error) *st = entry->st;
    return 1;
}

/* Keeps what the card said: a stat that worked, or a path that is not there.
 * Any other failure is asked again. */
static inline void sd_stat_store( struct sd_stat_cache *cache, const char *path, const struct stat *st, int error )
{
    struct sd_stat_entry *entry, *oldest = NULL;
    unsigned int hash, i;
    char *copy;

    if (!sd_stat_cacheable( path ) || (error && error != ENOENT)) return;
    hash = sd_stat_hash( path );
    if (!(entry = sd_stat_find( cache, path, hash )))
    {
        for (i = 0; i < SD_STAT_WAYS; i++)
        {
            struct sd_stat_entry *slot = &cache->slots[(hash + i) % SD_STAT_SLOTS];

            if (!slot->path) { oldest = slot; break; }
            if (!oldest || slot->used < oldest->used) oldest = slot;
        }
        if (!(copy = strdup( path ))) return;
        entry = oldest;
        free( entry->path );
        entry->path = copy;
        entry->hash = hash;
    }
    entry->error = error;
    if (!error) entry->st = *st;
    else memset( &entry->st, 0, sizeof(entry->st) );
    entry->used = ++cache->clock;
}

static inline void sd_stat_forget( struct sd_stat_cache *cache, const char *path )
{
    struct sd_stat_entry *entry;

    if (!path || !sd_stat_cacheable( path )) return;
    if (!(entry = sd_stat_find( cache, path, sd_stat_hash( path ) ))) return;
    free( entry->path );
    entry->path = NULL;
}

static inline void sd_stat_forget_all( struct sd_stat_cache *cache )
{
    unsigned int i;

    for (i = 0; i < SD_STAT_SLOTS; i++)
    {
        free( cache->slots[i].path );
        cache->slots[i].path = NULL;
    }
}

#endif
