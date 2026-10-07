/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
/* From wine-nx (horizon-wine/source), for tico's cores. Built with
 * SD_CACHE_STATS_AND_HELD_WRITES 0 it caches reads only: stat results and held
 * writes are only right while every change on the card goes through the sdmc
 * device, and a program that also calls fsdev* directly breaks that. */
#ifndef SD_CACHE_STATS_AND_HELD_WRITES
#define SD_CACHE_STATS_AND_HELD_WRITES 1
#endif
/*
 * The SD card read cache (sd_read_cache.h), installed over libnx's sdmc device
 * before the runtime opens a file on it. Everything that reads the card goes
 * through it: Wine's file system calls, image mapping, NLS files and fonts.
 *
 * Files are tracked by libnx's per-open data, so a file opened before the
 * cache was installed passes straight through. Data is copied to the caller
 * after the lock is released: a fault on the caller's buffer must not happen
 * while the cache is locked.
 *
 * Small writes are held back and sent together (sd_write_buffer.h). Reading,
 * opening, renaming or deleting a file sends what is held for it first; stat
 * and fstat add it to the size. The runtime sends everything out every 200 ms
 * and before it leaves.
 */
#include <switch.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/iosupport.h>
#include <unistd.h>

#include "sd_read_cache.h"
#include "sd_stat_cache.h"
#include "sd_write_buffer.h"

/* Reported by the runtime's [PROGRESS] line. */
unsigned int wine_nx_sd_reads;         /* read requests sent to the FS service */
unsigned int wine_nx_sd_hits;          /* reads the cache served without one */
unsigned long long wine_nx_sd_read_ns; /* time spent in those requests */
unsigned long long wine_nx_sd_bytes;   /* bytes those requests brought back */
unsigned int wine_nx_sd_writes;        /* write requests sent to the FS service */
unsigned int wine_nx_sd_writes_held;   /* the program's writes held back to go with others */
unsigned long long wine_nx_sd_write_ns; /* time spent in write requests */
unsigned int wine_nx_sd_stats;         /* stat and lstat the card was asked for */
unsigned int wine_nx_sd_stat_hits;     /* stat and lstat answered from sd_stat_cache.h */
/* Why held writes went out: a write elsewhere or too large for the buffer,
 * a read or open of the file, a seek from its end, closing it, the timer. */
unsigned int wine_nx_sd_flush_jump, wine_nx_sd_flush_path, wine_nx_sd_flush_end,
             wine_nx_sd_flush_close, wine_nx_sd_flush_timer;

extern void wine_nx_runtime_trace( const char *msg );

static const devoptab_t *sd_cache_base;
static devoptab_t sd_cache_device;
static pthread_mutex_t sd_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct sd_cache_file *sd_cache_files;
static u64 sd_cache_settled_at;
/* Whole chunks on every miss (fill_min 0). Readahead from 16 KB halved the
 * bytes The Sims 2 took from the card but nearly tripled the requests, and on
 * the hardware a request costs 0.83 ms before its transfer at 42 MB/s: card
 * time went from 41.7 to 39.3 s, which is not worth a request every few
 * kilobytes. The game rereads close to what it just read more than the
 * replay in tests/sd_read_cache.c does. */
static struct sd_cache_pool sd_cache_pool = { .max = SD_CACHE_POOL_MIN, .cap = SD_CACHE_POOL_MAX };
static int sd_cache_off;
static struct sd_write_buffer *sd_write_buffers;  /* under sd_cache_mutex */
static int sd_write_hold;  /* every way of seeing a file sends held writes first */
static struct sd_stat_cache sd_stat_cache;        /* under sd_cache_mutex */
static unsigned int sd_stat_generation;           /* moves on every change, under sd_cache_mutex */

/* A change to path on the card. Called with the lock held. */
static void sd_stat_changed( const char *path )
{
    sd_stat_generation++;
    if (path) sd_stat_forget( &sd_stat_cache, path );
    else sd_stat_forget_all( &sd_stat_cache );
}

struct sd_cache_fill_ctx
{
    struct _reent *r;
    void *fd;
};

static ssize_t sd_cache_base_read( struct _reent *r, void *fd, char *ptr, size_t len )
{
    u64 start = armGetSystemTick();
    ssize_t ret = sd_cache_base->read_r( r, fd, ptr, len );

    if (ret < 0 && r->_errno == EIO && sd_cache_base->seek_r && sd_cache_base->fstat_r)
    {
        int error = r->_errno;
        off_t offset = sd_cache_base->seek_r( r, fd, 0, SEEK_CUR );
        struct stat st;

        if (offset >= 0 && !sd_cache_base->fstat_r( r, fd, &st ) && offset >= st.st_size)
            ret = 0;
        else
            r->_errno = error;
    }

    __atomic_add_fetch( &wine_nx_sd_reads, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_sd_read_ns, armTicksToNs( armGetSystemTick() - start ), __ATOMIC_RELAXED );
    if (ret > 0) __atomic_add_fetch( &wine_nx_sd_bytes, (unsigned long long)ret, __ATOMIC_RELAXED );
    return ret;
}

/* The game's own memory comes first: the cache grows into the heap left free
 * above this floor, and only into part of it, so that a game whose appetite
 * grows later still finds room. mallinfo walks the heap, so the size is
 * settled a few times a minute and outside the lock. */
#define SD_CACHE_FREE_FLOOR_MB 512
#define SD_CACHE_FREE_SHARE    3     /* of what is free above the floor */
#define SD_CACHE_STEP          128   /* chunks, 16 MB: what makes a move worth it */

static unsigned int sd_cache_room(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    struct mallinfo heap = mallinfo();
    unsigned long long heap_size = (unsigned long long)(fake_heap_end - fake_heap_start);
    /* What malloc holds unused plus what it has not taken from the heap yet,
     * and the chunks the cache is already holding, which it can keep. */
    unsigned long long free_mb = (heap.fordblks + (heap_size > heap.arena ? heap_size - heap.arena : 0)) >> 20;
    unsigned long long room;
    unsigned long long chunks;
    unsigned int used;

    pthread_mutex_lock( &sd_cache_mutex );
    used = sd_cache_pool.used;
    pthread_mutex_unlock( &sd_cache_mutex );
    room = free_mb + (((unsigned long long)used * SD_CACHE_CHUNK) >> 20);

    if (room <= SD_CACHE_FREE_FLOOR_MB) return SD_CACHE_POOL_MIN;
    chunks = (room - SD_CACHE_FREE_FLOOR_MB) / SD_CACHE_FREE_SHARE * (1024 * 1024 / SD_CACHE_CHUNK);
    if (chunks < SD_CACHE_POOL_MIN) return SD_CACHE_POOL_MIN;
    if (chunks > SD_CACHE_POOL_MAX) return SD_CACHE_POOL_MAX;
    return (unsigned int)chunks;
}

/* Called before the lock is taken, from the read path. */
static void sd_cache_resize(void)
{
    u64 now = armGetSystemTick();
    u64 settled_at = __atomic_load_n( &sd_cache_settled_at, __ATOMIC_RELAXED );
    unsigned int target;

    if (settled_at && armTicksToNs( now - settled_at ) < 5000000000ull) return;
    target = sd_cache_room();
    pthread_mutex_lock( &sd_cache_mutex );
    if (__atomic_load_n( &sd_cache_settled_at, __ATOMIC_RELAXED ) != settled_at)
    {
        pthread_mutex_unlock( &sd_cache_mutex );
        return;
    }
    __atomic_store_n( &sd_cache_settled_at, now, __ATOMIC_RELAXED );
    if ((target > sd_cache_pool.max ? target - sd_cache_pool.max : sd_cache_pool.max - target) >= SD_CACHE_STEP)
    {
        sd_cache_pool.max = target;
        sd_cache_trim( &sd_cache_pool );
    }
    pthread_mutex_unlock( &sd_cache_mutex );
}

size_t wine_nx_sd_cache_reclaim( size_t bytes )
{
    size_t released = 0;

    if (!bytes || pthread_mutex_trylock( &sd_cache_mutex )) return 0;
    released = sd_cache_reclaim( &sd_cache_pool, bytes );
    if (released) __atomic_store_n( &sd_cache_settled_at, armGetSystemTick(), __ATOMIC_RELAXED );
    pthread_mutex_unlock( &sd_cache_mutex );
    return released;
}

/* Megabytes the cache is holding, for the runtime's [PROGRESS] line. */
unsigned int wine_nx_sd_cache_mb(void)
{
    unsigned int used;

    pthread_mutex_lock( &sd_cache_mutex );
    used = sd_cache_pool.used;
    pthread_mutex_unlock( &sd_cache_mutex );
    return (unsigned int)(((unsigned long long)used * SD_CACHE_CHUNK) >> 20);
}

static long long sd_cache_fill( void *ctx, long long offset, char *buf, size_t size )
{
    struct sd_cache_fill_ctx *fill = ctx;

    if (sd_cache_base->seek_r( fill->r, fill->fd, offset, SEEK_SET ) == -1) return -1;
    return sd_cache_base_read( fill->r, fill->fd, buf, size );
}

static ssize_t sd_cache_base_write( struct _reent *r, void *fd, const char *ptr, size_t len )
{
    u64 start = armGetSystemTick();
    ssize_t ret = sd_cache_base->write_r( r, fd, ptr, len );

    __atomic_add_fetch( &wine_nx_sd_writes, 1, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_sd_write_ns, armTicksToNs( armGetSystemTick() - start ), __ATOMIC_RELAXED );
    return ret;
}

/* ctx is the _reent of the thread sending the bytes out. */
static long long sd_cache_write_out( void *ctx, void *key, long long offset, const char *buf, size_t size )
{
    const struct sd_cache_file *file = sd_cache_find( sd_cache_files, key );

    sd_stat_changed( file ? file->path : NULL );
    if (sd_cache_base->seek_r( ctx, key, offset, SEEK_SET ) == -1) return -1;
    return sd_cache_base_write( ctx, key, buf, size );
}

/* Sends what is held for one file. Called with the lock held. The failure of
 * bytes a program was already told were written can only show here. */
static int sd_cache_flush_locked( struct _reent *r, struct sd_write_buffer *wb )
{
    int err = r->_errno, failed;
    long long pos;

    if (!wb || !wb->len) return 0;
    pos = wb->pos;
    r->_errno = 0;
    failed = sd_write_flush( wb, sd_cache_write_out, r );
    if (failed && !r->_errno) r->_errno = EIO;
    err = failed ? r->_errno : err;
    /* The window goes, and the card's position becomes the one the program saw. */
    sd_write_drop( wb );
    sd_cache_base->seek_r( r, wb->key, pos, SEEK_SET );
    r->_errno = err;
    if (!failed) return 0;
    wine_nx_runtime_trace( "[FS] held writes to a file on the card failed" );
    return -1;
}

/* Sends what was written since the last time and keeps the window: nothing
 * about the file changes for the program. Called with the lock held. */
static void sd_cache_send_locked( struct _reent *r, struct sd_write_buffer *wb )
{
    int err = r->_errno;

    if (!sd_write_dirty( wb )) return;
    wine_nx_sd_flush_timer++;
    if (sd_write_flush( wb, sd_cache_write_out, r ))
        wine_nx_runtime_trace( "[FS] held writes to a file on the card failed" );
    r->_errno = err;
}

static int sd_cache_flush_fd( struct _reent *r, void *fd )
{
    int ret;

    pthread_mutex_lock( &sd_cache_mutex );
    ret = sd_cache_flush_locked( r, sd_write_find( sd_write_buffers, fd ) );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

static void sd_cache_flush_all_locked( struct _reent *r )
{
    struct sd_write_buffer *wb;

    for (wb = sd_write_buffers; wb; wb = wb->next) sd_cache_send_locked( r, wb );
}

static void sd_cache_flush_all( struct _reent *r )
{
    if (!__atomic_load_n( &sd_write_buffers, __ATOMIC_RELAXED )) return;
    pthread_mutex_lock( &sd_cache_mutex );
    sd_cache_flush_all_locked( r );
    pthread_mutex_unlock( &sd_cache_mutex );
}

/* Sends what is held for a path, from every open of it. Called with the lock
 * held. Only the file concerned: a thread that keeps asking about some other
 * file must not send the bytes of a save being written a few at a time. */
static void sd_cache_flush_path_locked( struct _reent *r, const char *path )
{
    struct sd_write_buffer *wb;
    const struct sd_cache_file *file;

    for (wb = sd_write_buffers; wb; wb = wb->next)
    {
        if (!wb->len) continue;
        file = sd_cache_find( sd_cache_files, wb->key );
        if (!file || sd_cache_same_path( file->path, path ))
        {
            wine_nx_sd_flush_path++;
            sd_cache_flush_locked( r, wb );
        }
    }
}

static void sd_cache_flush_path( struct _reent *r, const char *path )
{
    if (!__atomic_load_n( &sd_write_buffers, __ATOMIC_RELAXED )) return;
    pthread_mutex_lock( &sd_cache_mutex );
    sd_cache_flush_path_locked( r, path );
    pthread_mutex_unlock( &sd_cache_mutex );
}

/* The size a file will have once what is held reaches the card. stat and
 * fstat say so instead of sending the bytes out: a game can ask for a file's
 * size after every few bytes it writes. */
static void sd_cache_held_size( struct stat *st, const struct sd_write_buffer *wb )
{
    long long end;

    if (!wb || !wb->len) return;
    end = wb->start + (long long)wb->len;
    if (end > (long long)st->st_size) st->st_size = (off_t)end;
}

/* Sends every held write out. The runtime calls it every 200 ms, so a file a
 * program leaves open is on the card soon after it stops writing, and before
 * it leaves. The windows stay: a save being written carries on in them. */
void wine_nx_sd_cache_flush(void)
{
    if (sd_cache_base) sd_cache_flush_all( _REENT );
}

static int sd_cache_open( struct _reent *r, void *fd, const char *path, int flags, int mode )
{
    int writable = (flags & O_ACCMODE) != O_RDONLY;
    struct sd_cache_file *file;

    /* The open may be of a file with bytes held, or may truncate one. */
    sd_cache_flush_path( r, path );
    if (sd_cache_base->open_r( r, fd, path, flags, mode ) == -1) return -1;
    pthread_mutex_lock( &sd_cache_mutex );
    if ((file = sd_cache_opened( &sd_cache_files, &sd_cache_pool, fd, path, writable )))
    {
        struct sd_cache_file *other;

        file->append = !!(flags & O_APPEND);
        /* Held writes stay one file's own business only while nothing else has
         * the path open: another open reads and writes at positions of its own.
         * Settled here, as a check on every write cost the main thread of
         * F.E.A.R. half its time while it saved. */
        for (other = file->next; other; other = other->next)
            if (sd_cache_same_path( other->path, path )) other->shared = file->shared = 1;
    }
    else if (writable)
    {
        /* Without a record of this writer, its readers cannot be told apart. */
        sd_cache_off = 1;
        for (file = sd_cache_files; file; file = file->next) sd_cache_drop( file, &sd_cache_pool );
    }
    if (flags & O_TRUNC) sd_cache_written( sd_cache_files, &sd_cache_pool, path );
    if (flags & (O_CREAT | O_TRUNC)) sd_stat_changed( path );
    pthread_mutex_unlock( &sd_cache_mutex );
    return 0;
}

static int sd_cache_close( struct _reent *r, void *fd )
{
    struct sd_write_buffer *wb;
    int flushed, ret;

    pthread_mutex_lock( &sd_cache_mutex );
    if ((wb = sd_write_find( sd_write_buffers, fd )) && wb->len) wine_nx_sd_flush_close++;
    flushed = sd_cache_flush_locked( r, wb );
    sd_write_forget( &sd_write_buffers, fd );
    sd_cache_closed( &sd_cache_files, &sd_cache_pool, fd );
    pthread_mutex_unlock( &sd_cache_mutex );
    ret = sd_cache_base->close_r( r, fd );
    return flushed ? -1 : ret;
}

/* A read that stops short of what was asked for, before the end of the file,
 * hands a program half of what it wanted with nothing to say so. Halo reports
 * that one of its files is missing or corrupted and never says which, so a card
 * that answers a request with less than it holds has to be visible. */
static void sd_cache_report_short_read( struct _reent *r, void *fd, long long got, size_t len )
{
    static unsigned int reported;
    const struct sd_cache_file *file;
    int failure = errno;
    off_t at, end;
    char message[384];

    if (reported >= 16) return;
    /* A file the program never opened for reading is the runtime's own stdout
     * or stderr, and refusing to read it is not the card holding data back. */
    if (got < 0 && (failure == EBADF || failure == EACCES || failure == EPERM)) return;
    if ((at = sd_cache_base->seek_r( r, fd, 0, SEEK_CUR )) == -1) return;
    if ((end = sd_cache_base->seek_r( r, fd, 0, SEEK_END )) == -1) return;
    sd_cache_base->seek_r( r, fd, at, SEEK_SET );
    /* The file really ending there is a short read of its own. */
    if (got >= 0 && (long long)at >= (long long)end) return;
    reported++;
    pthread_mutex_lock( &sd_cache_mutex );
    file = sd_cache_find( sd_cache_files, fd );
    snprintf( message, sizeof(message), "[FS] %s gave %lld of %u bytes, at %lld of %lld",
              file && file->path ? file->path : "a file on the card", got, (unsigned int)len,
              (long long)at - (got > 0 ? got : 0), (long long)end );
    pthread_mutex_unlock( &sd_cache_mutex );
    wine_nx_runtime_trace( message );
}

static ssize_t sd_cache_read_file( struct _reent *r, void *fd, char *ptr, size_t len )
{
    struct sd_cache_fill_ctx ctx = { r, fd };
    struct sd_cache_file *file;
    unsigned int fills = 0;
    long long got = SD_CACHE_BYPASS;
    char *copy;
    off_t pos;

    /* The bytes asked for may be held, in this file or another open of it. */
    if (__atomic_load_n( &sd_write_buffers, __ATOMIC_RELAXED ))
    {
        pthread_mutex_lock( &sd_cache_mutex );
        if ((file = sd_cache_find( sd_cache_files, fd ))) sd_cache_flush_path_locked( r, file->path );
        else sd_cache_flush_locked( r, sd_write_find( sd_write_buffers, fd ) );
        pthread_mutex_unlock( &sd_cache_mutex );
    }
    if (!len || len >= SD_CACHE_DIRECT || !(copy = malloc( len )))
    {
        ssize_t direct = sd_cache_base_read( r, fd, ptr, len );

        if (len && (size_t)direct != len) sd_cache_report_short_read( r, fd, direct, len );
        return direct;
    }

    if (!sd_cache_off) sd_cache_resize();
    pthread_mutex_lock( &sd_cache_mutex );
    if (!sd_cache_off && (file = sd_cache_find( sd_cache_files, fd )) && file->cacheable &&
        (pos = sd_cache_base->seek_r( r, fd, 0, SEEK_CUR )) != -1)
    {
        got = sd_cache_read( file, &sd_cache_pool, pos, copy, len, sd_cache_fill, &ctx, &fills );
        /* The file position ends after the bytes returned, as for a plain read. */
        sd_cache_base->seek_r( r, fd, got >= 0 ? pos + got : pos, SEEK_SET );
    }
    pthread_mutex_unlock( &sd_cache_mutex );

    if (got >= 0)
    {
        memcpy( ptr, copy, (size_t)got );
        if (!fills) __atomic_add_fetch( &wine_nx_sd_hits, 1, __ATOMIC_RELAXED );
    }
    free( copy );
    if (got == SD_CACHE_BYPASS)
    {
        ssize_t direct = sd_cache_base_read( r, fd, ptr, len );

        if ((size_t)direct != len) sd_cache_report_short_read( r, fd, direct, len );
        return direct;
    }
    if ((size_t)got != len) sd_cache_report_short_read( r, fd, got, len );
    return (ssize_t)got;  /* -1 keeps the errno of the failed request */
}

/* A write makes what the cache holds for that path out of date. Every write on
 * the card comes through here, so caching a file a program opened for writing
 * is safe: it reads its own bytes back from the card. */
static ssize_t sd_cache_write_file( struct _reent *r, void *fd, const char *ptr, size_t len )
{
    struct sd_write_buffer *wb;
    struct sd_cache_file *file;
    ssize_t ret;
    long long at = -1;
    int held = 0;

    /* Nothing to write is nothing to ask the card: F.E.A.R. does it often. */
    if (!len) return 0;
    pthread_mutex_lock( &sd_cache_mutex );
    wb = sd_write_find( sd_write_buffers, fd );
    if ((file = sd_cache_find( sd_cache_files, fd )))
    {
        /* Open once, only this open can hold chunks of the path. */
        if (file->shared) sd_cache_written( sd_cache_files, &sd_cache_pool, file->path );
        else sd_cache_drop( file, &sd_cache_pool );
        if (sd_write_hold && !file->append && !file->shared)
        {
            off_t card = wb && wb->len ? 0 : sd_cache_base->seek_r( r, fd, 0, SEEK_CUR );

            if (card != -1)
            {
                at = sd_write_position( wb, card );
                if (wb && wb->len && (len >= SD_WRITE_DIRECT || at < wb->start || at > wb->start + (long long)wb->len ||
                                      at - wb->start + (long long)len > SD_WRITE_BUFFER_SIZE))
                    wine_nx_sd_flush_jump++;
                held = sd_write_take( &sd_write_buffers, fd, at, ptr, len, sd_cache_write_out, r );
                if (held == -1)
                {
                    pthread_mutex_unlock( &sd_cache_mutex );
                    if (!r->_errno) r->_errno = EIO;
                    return -1;
                }
            }
        }
    }
    /* What is held has to reach the card before these bytes do, which then go
     * where the program's position is. */
    if (!held && sd_cache_flush_locked( r, wb ))
    {
        pthread_mutex_unlock( &sd_cache_mutex );
        return -1;
    }
    if (!held && wb && at != -1) sd_cache_base->seek_r( r, fd, at, SEEK_SET );
    if (!held) sd_stat_changed( file ? file->path : NULL );
    /* A file written a lot without the buffer says why, twice. */
    if (!held && file && (++file->direct_writes == 1000 || file->direct_writes == 20000))
    {
        char message[384];

        snprintf( message, sizeof(message), "[FS] %u writes straight to %s: last %u bytes%s%s%s",
                  file->direct_writes, file->path, (unsigned int)len, file->shared ? ", open more than once" : "",
                  file->append ? ", appending" : "", sd_write_hold ? "" : ", no buffer" );
        pthread_mutex_unlock( &sd_cache_mutex );
        wine_nx_runtime_trace( message );
    }
    else pthread_mutex_unlock( &sd_cache_mutex );
    if (held)
    {
        __atomic_add_fetch( &wine_nx_sd_writes_held, 1, __ATOMIC_RELAXED );
        return (ssize_t)len;
    }
    ret = sd_cache_base_write( r, fd, ptr, len );
    pthread_mutex_lock( &sd_cache_mutex );
    /* A reader may have refilled the cache while the write was in flight. */
    if ((file = sd_cache_find( sd_cache_files, fd )))
    {
        if (file->shared) sd_cache_written( sd_cache_files, &sd_cache_pool, file->path );
        else sd_cache_drop( file, &sd_cache_pool );
    }
    sd_stat_changed( file ? file->path : NULL );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

/* While bytes are held the file position is kept with them, and a seek from
 * the start or from where the file is only moves it: a save that goes back to
 * fill in a length stays in the buffer. From the end the size is the card's,
 * so what is held goes out first. */
static off_t sd_cache_seek( struct _reent *r, void *fd, off_t pos, int dir )
{
    struct sd_write_buffer *wb;

    pthread_mutex_lock( &sd_cache_mutex );
    wb = sd_write_find( sd_write_buffers, fd );
    if (wb && wb->len)
    {
        long long to = dir == SEEK_SET ? (long long)pos : dir == SEEK_CUR ? wb->pos + (long long)pos : -1;

        if (dir == SEEK_SET || dir == SEEK_CUR)
        {
            if (to >= 0) sd_write_seek( wb, to );
            pthread_mutex_unlock( &sd_cache_mutex );
            if (to < 0)
            {
                r->_errno = EINVAL;
                return -1;
            }
            return (off_t)to;
        }
        wine_nx_sd_flush_end++;
        if (sd_cache_flush_locked( r, wb ))
        {
            pthread_mutex_unlock( &sd_cache_mutex );
            return -1;
        }
    }
    pthread_mutex_unlock( &sd_cache_mutex );
    return sd_cache_base->seek_r( r, fd, pos, dir );
}

static int sd_cache_fstat( struct _reent *r, void *fd, struct stat *st )
{
    if (sd_cache_base->fstat_r( r, fd, st ) == -1) return -1;
    if (!__atomic_load_n( &sd_write_buffers, __ATOMIC_RELAXED )) return 0;
    pthread_mutex_lock( &sd_cache_mutex );
    sd_cache_held_size( st, sd_write_find( sd_write_buffers, fd ) );
    pthread_mutex_unlock( &sd_cache_mutex );
    return 0;
}

static int sd_cache_fsync( struct _reent *r, void *fd )
{
    if (sd_cache_flush_fd( r, fd )) return -1;
    return sd_cache_base->fsync_r( r, fd );
}

static void sd_cache_held_size_path( const char *path, struct stat *st )
{
    const struct sd_write_buffer *wb;
    const struct sd_cache_file *file;

    if (!__atomic_load_n( &sd_write_buffers, __ATOMIC_RELAXED )) return;
    pthread_mutex_lock( &sd_cache_mutex );
    for (wb = sd_write_buffers; wb; wb = wb->next)
        if (wb->len && (file = sd_cache_find( sd_cache_files, wb->key )) && sd_cache_same_path( file->path, path ))
            sd_cache_held_size( st, wb );
    pthread_mutex_unlock( &sd_cache_mutex );
}

/* Every 500th path stat-ed is named: the card answers each with up to five
 * requests, and a program that asks about the same files again and again is
 * worth a cache of its own. */
static void sd_cache_count_stat( const char *path )
{
    unsigned int n = __atomic_add_fetch( &wine_nx_sd_stats, 1, __ATOMIC_RELAXED );
    char message[384];

    if (n % 500) return;
    snprintf( message, sizeof(message), "[FS] stat %u: %s", n, path );
    wine_nx_runtime_trace( message );
}

/* A stat from the cache, or from the card and then kept. What held writes
 * will add is added either way. There are no links on the card, so stat and
 * lstat say the same. */
static int sd_cache_stat_path( struct _reent *r, const char *path, struct stat *st,
                               int (*base)( struct _reent *, const char *, struct stat * ) )
{
    unsigned int generation;
    int error, ret;

    pthread_mutex_lock( &sd_cache_mutex );
    if (sd_stat_lookup( &sd_stat_cache, path, st, &error ))
    {
        pthread_mutex_unlock( &sd_cache_mutex );
        __atomic_add_fetch( &wine_nx_sd_stat_hits, 1, __ATOMIC_RELAXED );
        if (error)
        {
            r->_errno = error;
            return -1;
        }
        sd_cache_held_size_path( path, st );
        return 0;
    }
    generation = sd_stat_generation;
    pthread_mutex_unlock( &sd_cache_mutex );

    sd_cache_count_stat( path );
    ret = base( r, path, st );
    pthread_mutex_lock( &sd_cache_mutex );
    /* Not if the card changed while it was asked. */
    if (generation == sd_stat_generation) sd_stat_store( &sd_stat_cache, path, st, ret ? r->_errno : 0 );
    pthread_mutex_unlock( &sd_cache_mutex );
    if (ret) return -1;
    sd_cache_held_size_path( path, st );
    return 0;
}

static int sd_cache_stat( struct _reent *r, const char *path, struct stat *st )
{
    return sd_cache_stat_path( r, path, st, sd_cache_base->stat_r );
}

static int sd_cache_lstat( struct _reent *r, const char *path, struct stat *st )
{
    return sd_cache_stat_path( r, path, st, sd_cache_base->lstat_r );
}

static int sd_cache_mkdir( struct _reent *r, const char *path, int mode )
{
    int ret = sd_cache_base->mkdir_r( r, path, mode );

    pthread_mutex_lock( &sd_cache_mutex );
    sd_stat_changed( path );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

static int sd_cache_rmdir( struct _reent *r, const char *path )
{
    int ret = sd_cache_base->rmdir_r( r, path );

    pthread_mutex_lock( &sd_cache_mutex );
    sd_stat_changed( NULL );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

static DIR_ITER *sd_cache_diropen( struct _reent *r, DIR_ITER *dir, const char *path )
{
    sd_cache_flush_all( r );
    return sd_cache_base->diropen_r( r, dir, path );
}

static int sd_cache_rename( struct _reent *r, const char *old_name, const char *new_name )
{
    int ret;

    pthread_mutex_lock( &sd_cache_mutex );
    sd_cache_flush_path_locked( r, old_name );
    sd_cache_flush_path_locked( r, new_name );
    sd_cache_forget_path( sd_cache_files, &sd_cache_pool, old_name );
    sd_cache_forget_path( sd_cache_files, &sd_cache_pool, new_name );
    pthread_mutex_unlock( &sd_cache_mutex );
    ret = sd_cache_base->rename_r( r, old_name, new_name );
    /* Everything under a directory renamed moves with it. */
    pthread_mutex_lock( &sd_cache_mutex );
    sd_stat_changed( NULL );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

static int sd_cache_unlink( struct _reent *r, const char *name )
{
    int ret;

    pthread_mutex_lock( &sd_cache_mutex );
    sd_cache_flush_path_locked( r, name );
    sd_cache_forget_path( sd_cache_files, &sd_cache_pool, name );
    pthread_mutex_unlock( &sd_cache_mutex );
    ret = sd_cache_base->unlink_r( r, name );
    pthread_mutex_lock( &sd_cache_mutex );
    sd_stat_changed( name );
    pthread_mutex_unlock( &sd_cache_mutex );
    return ret;
}

static int sd_cache_ftruncate( struct _reent *r, void *fd, off_t len )
{
    struct sd_cache_file *file;

    pthread_mutex_lock( &sd_cache_mutex );
    if (sd_cache_flush_locked( r, sd_write_find( sd_write_buffers, fd ) ))
    {
        pthread_mutex_unlock( &sd_cache_mutex );
        return -1;
    }
    if ((file = sd_cache_find( sd_cache_files, fd ))) sd_cache_forget_path( sd_cache_files, &sd_cache_pool, file->path );
    sd_stat_changed( file ? file->path : NULL );
    pthread_mutex_unlock( &sd_cache_mutex );
    return sd_cache_base->ftruncate_r( r, fd, len );
}

/* Replace the sdmc device in place: its index is the default device, which
 * paths without a device name rely on. Returns 0 when there is no sdmc. */
int wine_nx_sd_cache_install(void)
{
    int device = FindDevice( "sdmc:" );

    if (device < 0 || !devoptab_list[device] || sd_cache_base) return 0;
    sd_cache_base = devoptab_list[device];
    sd_cache_device = *sd_cache_base;
    sd_cache_device.open_r = sd_cache_open;
    sd_cache_device.close_r = sd_cache_close;
    sd_cache_device.read_r = sd_cache_read_file;
    if (sd_cache_base->write_r) sd_cache_device.write_r = sd_cache_write_file;
    /* Holding writes back needs every way of seeing a file to send them first. */
    if (SD_CACHE_STATS_AND_HELD_WRITES &&
        sd_cache_base->write_r && sd_cache_base->seek_r && sd_cache_base->fstat_r && sd_cache_base->stat_r)
    {
        sd_write_hold = 1;
        sd_cache_device.seek_r = sd_cache_seek;
        sd_cache_device.fstat_r = sd_cache_fstat;
        sd_cache_device.stat_r = sd_cache_stat;
        if (sd_cache_base->lstat_r) sd_cache_device.lstat_r = sd_cache_lstat;
        if (sd_cache_base->fsync_r) sd_cache_device.fsync_r = sd_cache_fsync;
        if (sd_cache_base->diropen_r) sd_cache_device.diropen_r = sd_cache_diropen;
        /* A stat kept is only right while every change on the card is seen. */
        if (sd_cache_base->mkdir_r) sd_cache_device.mkdir_r = sd_cache_mkdir;
        if (sd_cache_base->rmdir_r) sd_cache_device.rmdir_r = sd_cache_rmdir;
    }
    if (sd_cache_base->rename_r) sd_cache_device.rename_r = sd_cache_rename;
    if (sd_cache_base->unlink_r) sd_cache_device.unlink_r = sd_cache_unlink;
    if (sd_cache_base->ftruncate_r) sd_cache_device.ftruncate_r = sd_cache_ftruncate;
    devoptab_list[device] = &sd_cache_device;
    return 1;
}
