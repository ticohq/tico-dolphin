/* Copyright 2026 Dan | ticoverse.com. LGPL-2.1-or-later. */
/* The SD card read cache (sd_cache.c, from wine-nx): installed over libnx's
 * sdmc device, it keeps recently read 128 KB chunks of each open file in
 * memory, so small reads near each other cost one request to the card instead
 * of one each. Install it before opening the files it should cache. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 when installed, 0 when there is no sdmc device or it already is. */
int wine_nx_sd_cache_install(void);
/* Megabytes it is holding. */
unsigned int wine_nx_sd_cache_mb(void);

/* Requests sent to the card, the time and bytes they took, and reads served
 * from memory instead. */
extern unsigned int wine_nx_sd_reads;
extern unsigned long long wine_nx_sd_read_ns;
extern unsigned long long wine_nx_sd_bytes;
extern unsigned int wine_nx_sd_hits;

#ifdef __cplusplus
}
#endif
