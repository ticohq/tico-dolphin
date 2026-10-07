/* Copyright 2026 Dan | ticoverse.com. LGPL-2.1-or-later. */
/* What sd_cache.c asks of the program around it. */
#include <stdio.h>

void wine_nx_runtime_trace( const char *msg )
{
    fprintf( stderr, "%s\n", msg );
}
