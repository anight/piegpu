/*
 * assert.h for FAAD2 on Circle, found before Circle's: Circle's includes
 * circle/macros.h, whose ALIGN(n) replaces FAAD2's ALIGN (common.h). FAAD2's
 * assertions are off, as with NDEBUG.
 */
#ifndef GPU_AUDIO_FAAD2_COMPAT_ASSERT_H
#define GPU_AUDIO_FAAD2_COMPAT_ASSERT_H
#define assert(e)	((void) 0)
#endif
