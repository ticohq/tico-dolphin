/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/*
 * Writes held back from the SD card. Every write to the card is a request to
 * the FS service, which runs on the system core; a game that saves the way
 * LithTech games do, a value at a time, sends tens of thousands of them.
 * F.E.A.R.'s checkpoint at the start of a level is 262 KB, and writing it kept
 * the game's own cores idle while the system core sat at half its time.
 *
 * A window of the file, up to 1 MB, is kept here with the bytes the program
 * wrote into it and the file position it sees. Writes inside the window or
 * right after it stay here, and so does a seek, so a save that goes back to
 * fill in a length and carries on after it never reaches the card on its own:
 * LithTech's does, every few values. Sending the window out writes only the
 * part written since the last time and keeps the window, so the runtime's
 * periodic flush does not cut a save in two and send every length that falls
 * before the cut on its own. The caller drops the window before anything that
 * reads the file or moves its position on the card: a read, a seek from the
 * end, ftruncate, fsync or closing it, and opening, renaming or deleting it.
 * It only holds writes for a file open once and not for appending (sd_cache.c).
 *
 * Pure logic, so tests/sd_write_buffer.c can drive it on the host.
 */
#ifndef WINE_NX_SD_WRITE_BUFFER_H
#define WINE_NX_SD_WRITE_BUFFER_H

#include <stdlib.h>
#include <string.h>

#define SD_WRITE_BUFFER_SIZE (1024 * 1024)
/* A write this large is worth a request of its own, and goes straight out. */
#define SD_WRITE_DIRECT      (64 * 1024)

struct sd_write_buffer
{
    void *key;               /* libnx's per-open file data */
    long long start;         /* where in the file data[0] goes */
    size_t len;              /* bytes in the window, 0 for none */
    size_t dirty_from;       /* the part not on the card yet, */
    size_t dirty_to;         /* from data + dirty_from to data + dirty_to */
    long long pos;           /* the file position the program sees, while len */
    char *data;              /* SD_WRITE_BUFFER_SIZE bytes */
    struct sd_write_buffer *next;
};

/* Writes size bytes at offset in the file under key. The bytes written, or -1. */
typedef long long (*sd_write_out_fn)( void *ctx, void *key, long long offset, const char *buf, size_t size );

static inline struct sd_write_buffer *sd_write_find( struct sd_write_buffer *list, void *key )
{
    for (; list; list = list->next) if (list->key == key) return list;
    return NULL;
}

static inline int sd_write_dirty( const struct sd_write_buffer *wb )
{
    return wb && wb->dirty_to > wb->dirty_from;
}

/* Where the next plain write to the file lands: the position kept here while
 * there is a window, or where the card says the file position is. */
static inline long long sd_write_position( const struct sd_write_buffer *wb, long long card_position )
{
    return wb && wb->len ? wb->pos : card_position;
}

/* A seek while there is a window moves only the position kept here. */
static inline void sd_write_seek( struct sd_write_buffer *wb, long long pos )
{
    wb->pos = pos;
}

/* Sends what was written since the last time and keeps the window. 0, or -1
 * when the card took less than all of it: what was not written is dropped, as
 * a later write cannot make up for it and the program is told at the call
 * that sent it out. */
static inline int sd_write_flush( struct sd_write_buffer *wb, sd_write_out_fn out, void *ctx )
{
    size_t done;

    if (!sd_write_dirty( wb )) return 0;
    for (done = wb->dirty_from; done < wb->dirty_to;)
    {
        long long ret = out( ctx, wb->key, wb->start + (long long)done, wb->data + done, wb->dirty_to - done );

        if (ret <= 0)
        {
            wb->dirty_from = wb->dirty_to = 0;
            return -1;
        }
        done += (size_t)ret;
    }
    wb->dirty_from = wb->dirty_to = 0;
    return 0;
}

/* Forgets the window, once it is on the card. */
static inline void sd_write_drop( struct sd_write_buffer *wb )
{
    if (!wb) return;
    wb->len = 0;
    wb->dirty_from = wb->dirty_to = 0;
}

/* Takes a write of len bytes at pos, the file's position, for the file under
 * key. Returns 1 when it is held; 0 when the caller has to write it itself at
 * pos, with the window already sent out and dropped, so the bytes reach the
 * card in the order they were written; -1 when sending the window failed. */
static inline int sd_write_take( struct sd_write_buffer **list, void *key, long long pos,
                                 const char *ptr, size_t len, sd_write_out_fn out, void *ctx )
{
    struct sd_write_buffer *wb = sd_write_find( *list, key );
    size_t at, end;

    if (!len) return 0;
    if (wb && wb->len &&
        (len >= SD_WRITE_DIRECT || pos < wb->start || pos > wb->start + (long long)wb->len ||
         pos - wb->start + (long long)len > SD_WRITE_BUFFER_SIZE))
    {
        int failed = sd_write_flush( wb, out, ctx );

        sd_write_drop( wb );
        if (failed) return -1;
    }
    if (len >= SD_WRITE_DIRECT) return 0;

    if (!wb)
    {
        /* Without memory the write goes out as it always did. */
        if (!(wb = calloc( 1, sizeof(*wb) ))) return 0;
        if (!(wb->data = malloc( SD_WRITE_BUFFER_SIZE )))
        {
            free( wb );
            return 0;
        }
        wb->key = key;
        wb->next = *list;
        *list = wb;
    }
    if (!wb->len) wb->start = pos;
    at = (size_t)(pos - wb->start);
    end = at + len;
    memcpy( wb->data + at, ptr, len );
    if (end > wb->len) wb->len = end;
    if (!sd_write_dirty( wb ))
    {
        wb->dirty_from = at;
        wb->dirty_to = end;
    }
    else
    {
        if (at < wb->dirty_from) wb->dirty_from = at;
        if (end > wb->dirty_to) wb->dirty_to = end;
    }
    wb->pos = pos + (long long)len;
    return 1;
}

/* Frees the buffer of a file being closed; what it held has to be sent first. */
static inline void sd_write_forget( struct sd_write_buffer **list, void *key )
{
    struct sd_write_buffer **prev, *wb;

    for (prev = list; (wb = *prev); prev = &wb->next)
    {
        if (wb->key != key) continue;
        *prev = wb->next;
        free( wb->data );
        free( wb );
        return;
    }
}

#endif
