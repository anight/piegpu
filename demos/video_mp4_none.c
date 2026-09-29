/*
 * video_mp4_none.c - no MP4 linked in (hosts/web: the page puts the file at
 * PGPU_VIDEO_PATH in the program's file system before it starts)
 */
#include <stdint.h>

const uint8_t video_mp4[1] = {0}, video_mp4_end[1] = {0};
