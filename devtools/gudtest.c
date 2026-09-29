/*
 * gudtest.c - drives the RPi's USB monitor (GUD, gpu/display/gud_display)
 * from a Linux PC through DRM, without a desktop using it: a test pattern
 * (colour bars, a border, a grid), then two measurements, each update
 * flushed by DRM_IOCTL_MODE_DIRTYFB (with the gud driver's default
 * synchronous flushing, it returns once the pixels have gone over USB):
 *
 *   - a 40x40 square moving across the pattern (two small rectangles an update)
 *   - the whole screen in changing colours
 *
 * Then the pattern stays on screen for HOLD seconds.
 *
 *   cc -O2 -o gudtest devtools/gudtest.c $(pkg-config --cflags --libs libdrm)
 *   ./gudtest [/dev/dri/cardN] [HOLD]
 *
 * Without a card it takes the first one whose driver is "gud". A desktop
 * must not be using it (GNOME: the udev tag mutter-device-ignore).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static double now (void)
{
	struct timespec t;
	clock_gettime (CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int open_gud (const char *path)
{
	if (path)
	{
		return open (path, O_RDWR | O_CLOEXEC);
	}
	for (int i = 0; i < 16; i++)
	{
		char name[32];
		snprintf (name, sizeof name, "/dev/dri/card%d", i);
		int fd = open (name, O_RDWR | O_CLOEXEC);
		if (fd < 0)
		{
			continue;
		}
		drmVersionPtr v = drmGetVersion (fd);
		int gud = v && strcmp (v->name, "gud") == 0;
		drmFreeVersion (v);
		if (gud)
		{
			printf ("gudtest: %s\n", name);
			return fd;
		}
		close (fd);
	}
	errno = ENODEV;
	return -1;
}

static uint32_t *pixels;
static unsigned width, height, pitch;		/* pitch in pixels */

static void fill (unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color)
{
	for (unsigned j = y; j < y + h && j < height; j++)
	{
		for (unsigned i = x; i < x + w && i < width; i++)
		{
			pixels[j * pitch + i] = color;
		}
	}
}

/* the pattern: eight colour bars, a grid every 40 pixels, a white border */
static uint32_t pattern_at (unsigned x, unsigned y)
{
	static const uint32_t bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
					 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
	if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
	{
		return 0xffffff;
	}
	if (x % 40 == 0 || y % 40 == 0)
	{
		return 0x808080;
	}
	return bars[x * 8 / width];
}

static void draw_pattern (unsigned x, unsigned y, unsigned w, unsigned h)
{
	for (unsigned j = y; j < y + h && j < height; j++)
	{
		for (unsigned i = x; i < x + w && i < width; i++)
		{
			pixels[j * pitch + i] = pattern_at (i, j);
		}
	}
}

int main (int argc, char **argv)
{
	const char *path = argc > 1 && argv[1][0] == '/' ? argv[1] : NULL;
	int hold = argc > 1 ? atoi (argv[argc - 1]) : 0;
	if (hold <= 0)
	{
		hold = 10;
	}

	int fd = open_gud (path);
	if (fd < 0)
	{
		perror ("gudtest: no gud DRM device");
		return 1;
	}
	if (drmSetMaster (fd) != 0)
	{
		perror ("gudtest: not DRM master (is a desktop using the device?)");
		return 1;
	}

	drmModeResPtr res = drmModeGetResources (fd);
	drmModeConnectorPtr conn = NULL;
	for (int i = 0; res && i < res->count_connectors; i++)
	{
		conn = drmModeGetConnector (fd, res->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
		{
			break;
		}
		drmModeFreeConnector (conn);
		conn = NULL;
	}
	if (!conn)
	{
		fprintf (stderr, "gudtest: no connected connector with a mode\n");
		return 1;
	}
	drmModeModeInfo mode = conn->modes[0];
	width = mode.hdisplay;
	height = mode.vdisplay;
	uint32_t crtc = res->crtcs[0];
	printf ("gudtest: connector %u, mode %s (%ux%u), crtc %u\n", conn->connector_id, mode.name,
		width, height, crtc);

	struct drm_mode_create_dumb create = {.width = width, .height = height, .bpp = 32};
	if (drmIoctl (fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
	{
		perror ("gudtest: CREATE_DUMB");
		return 1;
	}
	pitch = create.pitch / 4;
	uint32_t fb;
	uint32_t handles[4] = {create.handle}, pitches[4] = {create.pitch}, offsets[4] = {0};
	if (drmModeAddFB2 (fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) != 0)
	{
		perror ("gudtest: AddFB2");
		return 1;
	}
	struct drm_mode_map_dumb map = {.handle = create.handle};
	if (drmIoctl (fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
	{
		perror ("gudtest: MAP_DUMB");
		return 1;
	}
	pixels = mmap (NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (pixels == MAP_FAILED)
	{
		perror ("gudtest: mmap");
		return 1;
	}

	draw_pattern (0, 0, width, height);
	double t = now ();
	if (drmModeSetCrtc (fd, crtc, fb, 0, 0, &conn->connector_id, 1, &mode) != 0)
	{
		perror ("gudtest: SetCrtc");
		return 1;
	}
	printf ("gudtest: mode set, pattern shown in %.1f ms\n", (now () - t) * 1000);

	/* a square moving along the middle: the old place back to the pattern,
	   the new one white; both rectangles in one DIRTYFB */
	const unsigned S = 40, frames = 200;
	unsigned y = height / 2 - S / 2, px = 0;
	t = now ();
	for (unsigned f = 0; f < frames; f++)
	{
		unsigned x = f * (width - S) / frames;
		draw_pattern (px, y, S, S);
		fill (x, y, S, S, 0xffffff);
		drmModeClip clips[2] = {{px, y, px + S, y + S}, {x, y, x + S, y + S}};
		if (drmModeDirtyFB (fd, fb, clips, 2) != 0)
		{
			perror ("gudtest: DirtyFB");
			return 1;
		}
		px = x;
	}
	double dt = now () - t;
	printf ("gudtest: moving square: %u updates in %.2f s, %.1f a second\n", frames, dt, frames / dt);

	/* the whole screen, a new colour each update */
	const unsigned full = 60;
	t = now ();
	for (unsigned f = 0; f < full; f++)
	{
		fill (0, 0, width, height, (f * 40) << 16 | (255 - f * 4) << 8 | (f * 97 & 255));
		drmModeClip clip = {0, 0, width, height};
		if (drmModeDirtyFB (fd, fb, &clip, 1) != 0)
		{
			perror ("gudtest: DirtyFB");
			return 1;
		}
	}
	dt = now () - t;
	double mb = (double) full * width * height * 2 / 1e6;	/* RGB565 over USB */
	printf ("gudtest: full screen: %u updates in %.2f s, %.1f a second, %.1f MB/s of RGB565\n",
		full, dt, full / dt, mb / dt);

	draw_pattern (0, 0, width, height);
	drmModeClip clip = {0, 0, width, height};
	drmModeDirtyFB (fd, fb, &clip, 1);
	printf ("gudtest: the pattern stays for %d s\n", hold);
	sleep (hold);

	return 0;
}
