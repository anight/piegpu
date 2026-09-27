/* the userland's vchiq_util.h (which brings in the VCHIQ API): Circle's */
#include <vc4/vchiq/vchiq_util.h>

/* the userland opens /dev/vchiq; here the kernel API is called directly */
#define vchiq_initialise_fd(pinstance, fd)	((void) (fd), vchiq_initialise (pinstance))
