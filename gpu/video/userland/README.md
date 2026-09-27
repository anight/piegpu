# MMAL from raspberrypi/userland

The files of `interface/mmal/` that the GPU's video support uses: MMAL's core,
utilities and VideoCore client, which talks to the firmware's components
(`vc.ril.video_decode`, `vc.ril.isp`) over VCHIQ.

- Source: https://github.com/raspberrypi/userland at
  a54a0dbb2b8dcf9bafdddfc9a9374fb51d97e976 (2024-12-23), BSD-3-Clause
  (`LICENCE`, Broadcom / Raspberry Pi).
- Only the sources built by `gpu/Makefile` and the headers they include are
  here, in the same layout (`#include "interface/mmal/..."`).
- One change: in `vc/mmal_vc_opaque_alloc.c`, `mmal_vc_opaque_acquire` and
  `mmal_vc_opaque_release` take `MMAL_OPAQUE_IMAGE_HANDLE_T` as their header
  declares (the upstream definitions say `unsigned int`, a different type on
  Circle).

Built on Circle's ports of vcos and VCHIQ (`circle/addon/vc4`), with the few
things those lack in `../compat/` (a microsecond clock, a timed semaphore wait,
POSIX scheduler priorities, `vchiq_initialise_fd`).
