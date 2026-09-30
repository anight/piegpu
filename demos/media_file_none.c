/*
 * media_file_none.c - no file linked in (hosts/web: the page puts the file at
 * PGPU_MEDIA_PATH in the program's file system before it starts)
 */
#include <stdint.h>

const uint8_t media_file[1] = {0}, media_file_end[1] = {0};
