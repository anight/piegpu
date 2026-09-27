/* circle_vcos.c - vcos calls MMAL needs that Circle's vcos port lacks */
#include "interface/vcos/vcos.h"

int circle_vcos_semaphore_wait_timeout (void *sem, unsigned timeout_ms)
{
	unsigned long long end = circle_micros () + timeout_ms * 1000ULL;
	for (;;)
	{
		if (vcos_semaphore_trywait ((VCOS_SEMAPHORE_T *) sem) == VCOS_SUCCESS)
		{
			return VCOS_SUCCESS;
		}
		if (circle_micros () >= end)
		{
			return VCOS_EAGAIN;
		}
		vcos_sleep (1);
	}
}
