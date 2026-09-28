/* sdcard.h - the dev kit's microSD slot (sdcard.c) */
#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>

/* power the card and mount its FAT at mount_point (e.g. "/sdcard") */
bool sdcard_mount (const char *mount_point);

#endif
