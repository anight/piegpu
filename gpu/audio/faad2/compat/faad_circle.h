/*
 * faad_circle.h - what FAAD2 needs of a C library on Circle, which has none
 * (force-included by the Makefile): its three diagnostics to stderr are
 * dropped; qsort and abs are in faad_circle.c.
 */
#ifndef GPU_AUDIO_FAAD2_COMPAT_FAAD_CIRCLE_H
#define GPU_AUDIO_FAAD2_COMPAT_FAAD_CIRCLE_H

#include <stdio.h>
#undef fprintf
#define fprintf(...)	((void) 0)

#endif
