/*
 * media_reader.h - a file on the microSD card read ahead, for the media demo
 * (demos/media.c, PGPU_MEDIA_READER): the file in 256 KB blocks, loaded into
 * PSRAM by a task on core 1 ahead of where it's read, so that a read is a copy
 * from memory. The card has stretches of seconds in which every read takes
 * 25 ms or more (hosts/esp32p4/main/sdbench.c): small reads then starve the
 * demo, but 256 KB ones still come at 2.6 MB/s or more, above what a film
 * needs.
 */
#ifndef MEDIA_READER_H
#define MEDIA_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgpu.h"

/* the file: *read and *ctx read it (pgpu_read_t), *size its bytes; false if it
   can't be opened (or no memory) */
bool media_reader_open (const char *path, pgpu_read_t *read, void **ctx, uint64_t *size);

/* the numbers since the last call, as a line of text */
void media_reader_stats (void *ctx, char *line, size_t bytes);

#endif
