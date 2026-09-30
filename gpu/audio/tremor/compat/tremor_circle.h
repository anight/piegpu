/*
 * tremor_circle.h - what Tremor and libogg need of a C library on Circle,
 * which has none (force-included, CMakeLists.txt): toupper as a function
 * (newlib's <ctype.h> makes it a macro on its _ctype_ table, which Circle
 * hasn't), in tremor_circle.c with labs and memchr.
 */
#ifndef GPU_AUDIO_TREMOR_COMPAT_TREMOR_CIRCLE_H
#define GPU_AUDIO_TREMOR_COMPAT_TREMOR_CIRCLE_H

#include <ctype.h>
#undef toupper
int toupper (int c);

#endif
