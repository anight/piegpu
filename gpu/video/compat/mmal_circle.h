/* mmal_circle.h - what the MMAL sources need beyond Circle's vcos port
   (forced into every MMAL source by the Makefile) */
#ifndef MMAL_CIRCLE_H
#define MMAL_CIRCLE_H

#ifndef PRIi64
#define PRIi64	"lld"
#endif
#ifndef PRIu64
#define PRIu64	"llu"
#endif

/* vcos's thread priorities (only ARM-side components make threads: none here) */
#define SCHED_OTHER	0
static inline int sched_get_priority_min (int policy)	{ (void) policy; return 1; }
static inline int sched_get_priority_max (int policy)	{ (void) policy; return 99; }

/* a semaphore wait with a timeout (circle_vcos.c; Circle's vcos has none) */
#define vcos_semaphore_wait_timeout	circle_vcos_semaphore_wait_timeout
#ifdef __cplusplus
extern "C"
#endif
int circle_vcos_semaphore_wait_timeout (void *sem, unsigned timeout_ms);

/* microseconds since boot (Circle's timer; defined by the kernel) */
#ifdef __cplusplus
extern "C"
#endif
unsigned long long circle_micros (void);
#define vcos_getmicrosecs()	((unsigned) circle_micros ())
#define vcos_getmicrosecs64()	circle_micros ()

#endif
