# Developing piegpu

How the RPi side works inside, for changing it. Elsewhere:

- building and using it: the [README](../README.md);
- the wire protocol: [protocol.md](protocol.md);
- the host side (libpgpu, pgl): [host-library.md](host-library.md).

Supported for now: the Raspberry Pi Zero / Zero W and the Zero 2 W; where a
model is named below, the fact was measured on that one.

- [The I2S link](#the-i2s-link)
- [Rendering](#rendering)
- [The screen: panel and HDMI](#the-screen-panel-and-hdmi)
- [Video](#video)
- [Audio](#audio)
- [The run log: a restart explains itself](#the-run-log-a-restart-explains-itself)
- [The control list checker](#the-control-list-checker)

## The I2S link

- **Receive:** cyclic DMA from the PCM receive FIFO into a 1 MB ring buffer. The
  CPU parses packets from the ring and drives READY from its fill level.
- **Replies:** a second self-looping DMA control block plays a 1 MB transmit ring
  of idle words into the PCM transmit FIFO for as long as the Pico clocks. A reply
  is written about 1024 words ahead of the DMA's read position and zeroed again
  once the DMA has passed it.

## Rendering

- **Vertex processing on the ARM:** transform, GL ES 1.1 lighting (up to 4
  lights, no spot lights, infinite viewer), texture matrix, per-vertex fog,
  primitive assembly, clipping (near, far and a guard band), flat shading
  (last vertex), two-sided lighting (facing per triangle). Screen-space triangles
  go to the V3D in NV shader mode.
- **Guard band:** vertices stay within 800 px outside the viewport. Measured on
  the hardware at 320×240: up to x −960 … 1280 px renders, x −1120 … 1440 px
  loses triangles, although the 12.4 fixed-point format reaches ±2048.
- **Points and lines** are screen-space quads, 1 pixel and `LINE_WIDTH` wide.
- **Jobs:** each render target's draws are one V3D job (binning, then rendering
  64×64 tiles). Colour is loaded from memory unless cleared; depth and stencil
  are stored after every job (T-format, about 80 µs at 320×240) and loaded
  unless cleared. A frame that switches targets renders several jobs. The
  rendering list takes up to 43 bytes a tile, and its buffer is sized for the
  largest screen's 32×32 tiles (a fixed 16 KB overflowed at 1920×1080: 510
  tiles).
- **Texture targets** are level 0 of a texture, in its tiled layout (T or LT
  RGBA8888, the tile buffer's format), not y-flipped: rows in GL order.
- **Stencil:** every fragment shader writes the three TLB stencil setup words
  from uniforms (Mesa's encoding); with the test off they say "always pass, keep".
- **Scissor** and the viewport become the V3D's clip window; polygon offset is
  its depth offset (factor and units as in Mesa's `vc4`). An empty clip window
  doesn't clip everything on the V3D: such draws are skipped.
- **V3D state that persists across jobs:** the line width keeps its value from
  the previous job (measured: lines drew wrongly after wide-line tests), so
  each job emits it before first use; the depth offset is treated the same.
- **Programs** run in the V3D's GL shader mode: the binner runs the coordinate
  shader, the renderer the vertex and fragment shaders, and the hardware clips.
  Each draw gets a shader record, attribute records pointing straight into the
  buffers, and uniform streams resolved at draw time. Buffers used by a program
  draw are copied on write for the rest of the frame (§6.3). The viewport has a
  negative y scale on the panel, because its rows go top to bottom.
- **Program fragment shaders** are compiled by Mesa for an RGBA8888 window
  framebuffer with blending off; `glslc` replaces the final colour write with one
  of two endings (`devtools/qpuasm.py`): reorder Mesa's BGRA into the tile
  buffer's RGBA (8 instructions), or load the tile buffer's colour and blend
  per channel with 48 coefficient uniforms, which cover every blend factor and
  equation, the constant colour and the colour mask (about 75 instructions).
- **Fixed-function fragment shaders:** 40 built-in QPU programs (`gpu/shaders.py`,
  generated with `devtools/qpuasm.py`): texture environment (none, MODULATE,
  REPLACE, DECAL, BLEND) × fog × alpha test × blending. Blending and the colour
  mask are done in the shader (tile buffer colour read), with the same
  coefficients as programs; `SRC_ALPHA_SATURATE` is approximated by `SRC_ALPHA`.
  For `A8` textures the texel's RGB is taken as 1 (GL ES 1.1 environments).
- **Textures:** converted at upload to RGBA8888 (R in byte 0) in Mesa's tiled
  layouts (LT for levels up to 16 pixels, else T), levels smallest first with
  level 0 page aligned, cube faces as whole mip trees; rows bottom-up as sent.
  ETC1 is decoded on the ARM. (Raster RGBA32R, used before, reads rows at a
  stride of max(width, 4) texels: verified with widths 1, 2, 4, 8 and 64.)

## The screen: panel and HDMI

- **Output:** the V3D renders RGB565 directly into two alternating screen
  buffers. The ST7789 DMA driver sends them to the panel at 75 MHz (60 fps at
  320×240). On HDMI the V3D renders straight into the pages of a three-page
  firmware framebuffer: one on screen, one waiting for the vertical sync that
  shows it, one being drawn; no copy (a DMA copy cost 2.6 ms of CPU a frame at
  512×300: CPU-G 20% instead of 6%). The vertical sync is the frame count of
  the display scaler's channel for HDMI (HVS `DISPSTAT1`, bits 17:12; measured
  60 a second), polled with a 100 ms timeout. The firmware's "wait for vsync"
  call, used before, has no timeout; with it, the RPi hung once as a monitor
  was switched on during a video (the log stopped, the watchdog reset it;
  where it hung isn't known), and since the change it hasn't. The video demo
  on 1024×600 went from 38–51 to 57–60 fps. A page flip is still a firmware call
  (`SetVirtualOffset`).
- **The firmware's framebuffer limits:** three pages of a 1920×1080 screen
  make a framebuffer 1920×3240. With the firmware's default limits the
  allocation succeeded, but the firmware then stopped answering: the next
  mailbox call (the per-second ARM clock query) never returned, and the
  watchdog restarted the board, at every boot while that monitor was
  connected (measured on a Zero 2 W with a 1920×1080 monitor; 1024×1800
  worked). `config.txt` sets `max_framebuffer_width=2048` and
  `max_framebuffer_height=4096` (room for three pages of 1920×1200); the
  firmware's defaults aren't documented. Circle's mailbox calls have no
  timeout, so any firmware call hangs the same way.
- **`gpu_mem=192`:** with 128 MB, a 1920×1080 framebuffer left the video
  decoder "out of resources" (`MMAL_ENOSPC`, no frames) for a 2048×1152
  texture; 192 MB plays it.
- **Screen size on HDMI** (`gpu/kernel.cpp`, `gpu/display/`; the options
  `output=`, `panel=`, `hdmi_pixels=` are in the README's "Kernel command
  line"): the monitor's preferred mode (native: 1024×600 for a 1024×600
  monitor), up to 1920×1200; a larger one, or one over `hdmi_pixels`, is
  divided by the smallest whole number that makes it fit (with
  `hdmi_pixels=230400`, 1920×1080 and 1280×720 give 640×360, 1024×600 gives
  512×300). The width is a multiple of 16.
- **Panel detection** (`CPanelOutput::Detect`): at boot the panel is reset
  and its registers read over MISO (GPIO9), bit-banged at about 500 kHz (the
  ST7789 reads slowly; the driver's SPI runs at 75 MHz), then the pins go back
  to SPI0. A panel answers RDDID (04h: a dummy bit, then its ID bytes) the
  same with MISO pulled up and pulled down, and releases the line after them;
  with nothing there the pull-up reads all ones and the pull-down all zeros.
  Measured: 1000 of 1000 reads the same with each pull (ID 81 81 B3). The ID
  bytes are the module maker's (IDSET, C1h, in the panel's NVM; the ST7789V
  datasheet's default is 85 85 52), and no register tells the glass's size
  (the controller's memory is 240×320 whatever is attached).
- **Hot plug:** the HDMI hot-plug line is GPIO46 on a Zero, GPIO28 on a Zero
  2 W (low while a monitor is connected), sampled every 20 ms; a change counts
  after 200 ms.
  The EDID is read over the DDC bus (BSC2, address 0x50, 100 kHz, about 12 ms),
  because the firmware's EDID property tag keeps answering with the EDID read
  at boot after the monitor is gone. At boot it's read at once; after a hot
  plug from 2 s on (the firmware reads it too, over the same bus, and sets HDMI
  up again); while a connected monitor doesn't answer, it's tried again every
  second.
- **The panel while the screen is on HDMI** (`ShowPanelNotice`), four
  lines: Monitor and Native resolution (the monitor's EDID: its name and
  preferred mode), HDMI resolution (the signal the firmware sends: the pixel
  valve's size, the refresh measured; `config.txt` sets it at boot, so a mode
  forced there stays whatever monitor is plugged in) and Render resolution
  (the screen GL draws into, which the firmware scales to the signal).
- **The splash** (`ShowSplash`) on the screen, panel or HDMI, while no host
  draws (none has ended a frame, or not for a second): at boot, after a
  screen change, when a monitor's EDID comes and when a PC's desktop lets go
  of the screen. It says what the screen is ("HDMI 1920x1080@60Hz ready"),
  the monitor, the render size, the board, its clocks, the host setting and
  the build. It's laid out for 320x240 and every font pixel drawn as a k × k
  square (the panel 1, 1024x600 2, 1920x1080 4); the host's first frame
  replaces it. At boot it's drawn after the renderer's start, which clears
  the framebuffer's pages.
- **A screen change waits for the frame's end** (`IsBetweenFrames`), and a
  host killed in the middle of a frame never sends it: after a second with no
  packets mid-frame the frame is dropped (`CCommands::AbandonFrame`) and the
  screen changes.
- **HDMI mode:** the firmware chooses it at boot and doesn't change it later.
  `config.txt` has `hdmi_force_hotplug=1`, so that HDMI stays on (640×480)
  when the RPi boots without a monitor. By default the firmware prefers TV
  modes: for a 1024×600 monitor it sent 720×576 at 50 Hz, capping frames at
  50 fps. `hdmi_group=2` gave 1024×768 at 60 Hz; `hdmi_mode=87` with
  `hdmi_cvt=1024 600 60` gives that monitor's own mode (measured: 59.9 fps).

## Video

- **Video** (`gpu/video/`): MMAL (the Raspberry Pi userland's client,
  BSD-3, vendored in `gpu/video/userland`) talks to the firmware's components
  over Circle's VCHIQ. A stream is `vc.ril.video_decode` tunnelled to
  `vc.ril.isp` inside the VideoCore; the ISP scales to the texture's size and
  converts to RGBA, and its frames come into ARM memory (4 KB aligned buffers,
  by the VideoCore's DMA: the ARM copies nothing). The texture then points at
  the frame: raster RGBA (TMU type RGBA32R). The TMU reads raster rows at a
  power-of-two stride (measured: a 320-wide texture came out garbled, 512
  right), hence the width rule. MMAL's zero-copy would need the firmware's
  VCSM service, which Circle lacks; it isn't needed.
- **Each frame is tiled once:** the TMU samples raster rows about four times
  slower than a tiled texture (a 1024x576 quad on HDMI: 8.7 vs 2.05 ms), and
  at 60 fps a 24 fps frame is drawn 2.5 times. So a video texture has tiled
  storage of its own (`CTextures::CreateExternal`), and each new frame is
  copied into it once, between frames: a render-only V3D job whose tiles load
  the frame's raster rows and store T-format (`CRenderer::CopyToTiled`,
  `CV3D::RunRender`; no shader, no TMU). Without the storage, or if a copy
  fails, the TMU reads the raster frame. Measured on the Zero 2 W, HDMI
  1024x600: GPU 71% → 35%.
- **VCHIQ's tasks** run when the main loop yields (Circle's cooperative
  scheduler): the loop yields after at most 1 ms of commands. Measured: with
  64 packets a turn and a fast host (a PC over USB), the decoder stopped after
  11 frames. The V3D jobs yield too while they run
  (`CV3D::SetWaitHandler`, pointed at `CScheduler::Yield`): with 3–4 ms more
  work at a frame's end that didn't yield (a plain busy wait did the same),
  the decoder stopped for good after 4 frames.
- **Measured** (720p H.264 test pattern, High profile with B-frames, 512×288
  texture on the panel, from the P4): 30 video frames a second with none
  dropped, GL at 60 fps, the host's CPU 1–2%. Decoder to RGBA in ARM memory:
  96 fps from 720p, 43 fps from 1080p (1024×576).
- **In 64 bit too** (the Zero 2 W): MMAL's messages to the VideoCore have the
  VideoCore's 32-bit layout on any client
  (`gpu/video/userland/interface/mmal/vc/mmal_vc_msgs.h`);
  `devtools/mmal-layout.py` checks the layout against the 32-bit compiler's.
  Circle's vcos is used in 64 bit too (the fork's "vcos: build for AArch64
  too"). The MMAL sources are built with `-mstrict-align`: they read VCHIQ's
  messages in place, in Circle's coherent region, which is Device memory in 64
  bit, where unaligned accesses fault.

## Audio

The audio stream (protocol §7.13, `gpu/audio/`): AAC decoded on the ARM,
played on HDMI through the VideoCore's audio service.

- **Output** (`CAudioOut`): Circle's VCHIQ sound device
  (`addon/vc4/sound`), destination HDMI, 16-bit stereo in chunks of 2048
  frames (43 ms at 48 kHz) from a 2 s ring; silence while the ring is empty.
  The chunks are handed over in VCHIQ's task, on core 0, when the main loop
  yields, and Circle's driver keeps only two queued in the VideoCore: with
  480-frame chunks (20 ms) the VideoCore ran dry for a moment when a stream
  opened (the main loop busy 20–40 ms: MMAL's set-up, the first frames), a
  click; with 2048 frames it has 85 ms, and ran dry 0 times in three runs.
  The firmware sets HDMI up with audio from the monitor's EDID at boot:
  nothing in `config.txt` was needed for the Dell S2421H (a monitor without
  speakers gets no sound). One output a sample rate, kept for the whole run
  (the service is opened once a device); a stream's close stops it, the next
  stream starts it again.
- **Decoding** (`aac.c`): FAAD2 2.11.3's libfaad (`gpu/audio/faad2`,
  vendored unchanged; GPL 2 or later, so it can be linked with Circle's GPL
  3), float out, more than two channels mixed down to stereo (FAAD2's
  downmatrix), mono doubled, the volume applied in the conversion to 16 bit.
  It's a library of its own (`gpu/audio/faad2/Makefile`, `libaac.a`): the
  gpu app's C flags force-include MMAL's header, and FAAD2 needs two shims
  for Circle, which has no C library (`compat/`: an `assert.h` found before
  Circle's, whose `circle/macros.h` redefines FAAD2's `ALIGN`; `qsort`,
  `abs`, and its three `fprintf`s to stderr dropped). Checked on a PC
  against ffmpeg: a stereo AAC file the same sample for sample (correlation
  1.00000, at most 245 of 32768 apart); FAAD2 drops the first unit's
  priming frames, so the frames of unit k start at unit k's pts.
- **A core of its own:** on the Zero 2 W the decoder runs on core 1
  (`CCores` in `gpu/kernel.h`; Circle configured with `ARM_ALLOW_MULTI_CORE`,
  `devtools/build-gpu.sh`; cores 2 and 3 halt). On the Zero, one
  core, it runs in the main loop, up to 2 ms a turn. Between the cores:
  rings with one writer and one reader each and memory barriers, no locks.
  The compressed samples: `Data` (core 0) publishes a sample once its last
  chunk has come; the decoder frees it once decoded (the host's room). The
  frames: the decoder writes, `GetChunk` (VCHIQ's task, core 0) reads. The
  times: a mark (output frame, pts) for each decoded unit. Closing parks the
  decoder first: each side stores its flag, then a full barrier, then reads
  the other's, so at least one sees the other.
- **The clock:** the time being heard is the newest mark at or before the
  frame being played (frames read from the ring less the two chunks the
  VideoCore holds, plus the time since the last chunk was asked for, at most
  a chunk: so it runs smoothly, not in 43 ms steps), plus the frames since
  it. (With the steps, a video frame was dropped now and then: 38 in 70 s
  once; smoothed, 0 in three 25 s runs.) A video stream opened with the audio (`CVideo::SetClock`)
  shows the frame due at that time, and its first frame until the sound
  starts. If the sound runs dry the clock stops, and the picture waits.
  Not measured: the delay after the VideoCore (the camera sees only the
  panel, so the picture against the sound can't be timed).
- **Volume:** `volume=` on the command line (percent, default 10), and
  `VOLUME` for a stream.
- **The VideoCore running dry** is flagged in its completion messages: bit
  30 of their byte count (undocumented; Circle's driver masked bits 31–30
  off, Linux's takes such a count as an underrun). piegpu's fork counts
  them (`GetCompleteFlagCount`, the last's bits and time). Shown on
  purpose by the `SOUNDTEST` host line (text mode): a tone, the main loop
  stalled for 150 ms without yielding: one completion with `0x40000000`,
  at the stall's end, and a chunk asked for 151.5 ms after the one before.
- **Debugging clicks:** the output keeps the last 20 s of what it handed to
  the VideoCore and each chunk's time, frames from the ring and the ring's
  level. The `PCM` host line dumps them, with the completion flags;
  `devtools/pcmdump.py` fetches that (text mode: after the session), writes
  a WAV (`devtools/logs/pcm.wav`) and lists late chunks, chunks padded with
  silence and jumps in the samples.
- **Verified** on the Zero 2 W with the Dell S2421H, the trailer through
  `video_host`: the webcam's microphone recording cross-correlated with the
  trailer's audio decoded on the PC: 2 s windows over 40 s all within ±4 ms
  of their expected place, across the loop (period 33.0 s) too; 1843 units,
  0 broken, 30 ms of silence at the start; decoded on core 1. With the
  decoder in the main loop: the same sound (8 s of recording = 7.996 s of
  trailer). The Zero: builds, not run.

## The run log: a restart explains itself

An RPi that restarts on its own used to leave nothing behind:

- **A crash can't report itself.** An exception, a failed assertion or
  `LOGPANIC` halts the CPU. Its message goes through the logger to the USB
  serial port, but the USB gadget needs the interrupts that the crash just
  lost, so the message never leaves the board.
- **A hang has no message at all.**

In both cases devlink's hardware watchdog (`DEVLINK_WATCHDOG_SECONDS`, 10 s)
restarts the board, and the next run started blank.

Now each run keeps its log in RAM that survives a restart. At boot, the next
run says how the previous one ended and shows its last lines.

| Code | What it does |
|---|---|
| `devtools/runlog.{h,cpp}` | `CRunLog`: the ring, the event hook, the report |
| piegpu's fork of Circle (github.com/anight/circle, branch `piegpu`) | Circle leaves the top of the ARM memory to the app (the commit "Memory: MEM_PERSISTENT_SIZE …") |
| `devtools/build-gpu.sh` | Circle's option `MEM_PERSISTENT_SIZE=0x10000` (CIRCLE_DEFINES) |
| `gpu/kernel.cpp` | `m_RunLog.Initialize ()` right after the logger's; `Report ()` after the boot lines |
| `gpu/main.cpp`, `gpu/install/installer.cpp`, `devtools/devlink.cpp` | Mark the restarts that were asked for |

### Which RAM survives a restart

This was measured, not taken from documentation. A probe build wrote marker
words at chosen addresses, restarted the board with `reboot ()` (a watchdog
reset), and logged which markers were still there. Results on the Zero 2 W,
with `gpu_mem=128`, so the ARM gets 384 MB up to 0x18000000:

| Addresses | After a restart |
|---|---|
| 0x120000–0x3E0000: the kernel area past the image, the stacks, the start of the gap in Circle's memory map | zeroed |
| 0x3F0000 and up, to 0x4F0000 | kept |
| 0x08000000–0x16FF0000: the heap's upper part | kept |
| 0x17F00000–0x17FFFFF0: the top of the ARM memory | kept |

- **Who clears it:** the boot firmware, not Circle. Circle clears only its
  bss (`lib/sysinit.cpp`).
- **Where it stops:** somewhere between 0x3E0000 and 0x3F0000. That's not a
  documented or round number, so the log doesn't rely on it.
- **The heap:** it survives but belongs to the heap, so the log can't use it
  either.

### Keeping the top of the ARM memory out of Circle's hands

Circle hands all the ARM memory the firmware reports
(`PROPTAG_GET_ARM_MEMORY`) to its heap and page allocator. piegpu's fork
subtracts `MEM_PERSISTENT_SIZE` from that size in `CMemorySystem`
(`lib/memory.cpp` and `lib/memory64.cpp`). The top 64 KB is then the app's:
no allocator ever returns it. The run log finds it with the same property
tag, at `nBaseAddress + nSize - MEM_PERSISTENT_SIZE`.

`build-gpu.sh` configures each board's build with `MEM_PERSISTENT_SIZE`
(Circle's CMake option `CIRCLE_DEFINES`). Without it, `runlog.cpp` stops the
build with an `#error`, rather than writing to memory the heap might use.

How the reserved memory is mapped differs between the boards, because the
memory now lies above what Circle thinks is RAM:

- **Zero 2 W, 64 bit:** Circle maps it as Device memory (64 KB pages), which
  is uncached. Writes reach RAM directly. Accesses have to be aligned, which
  they are: the header is `u32`s and the ring is bytes.
- **Zero, 32 bit:** Circle maps 1 MB sections, and the one holding the top
  64 KB starts below the reduced size, so it's normal cached memory. A reset
  doesn't write the data cache back, so every write is followed by
  `CleanAndInvalidateDataCacheRange ()` over what changed (`CRunLog::Flush`).
  That's harmless on the Zero 2 W.

### Layout

```
top of ARM memory - 64 KB
┌──────────────────────────────────────────────┐
│ header, 6 × u32                              │
│   Magic     0x52554E4C ("RUNL")              │
│   Check     ~Magic ^ nRun                    │
│   nRun      runs since power-on              │
│   End       EndRunning / EndPanic /          │
│             EndRestart                       │
│   nIn       write position in the ring       │
│   bWrapped  the ring has wrapped             │
├──────────────────────────────────────────────┤
│ ring: 64 KB - 24 bytes of text, one line per │
│ message: "<seconds>.<hundredths> <source>:   │
│ <message>\n"                                 │
└──────────────────────────────────────────────┘
```

After power-on the RAM holds random bits. A header only counts as the
previous run's if all of these hold:

- the magic matches;
- `Check` matches `nRun`;
- `End` is a known ending;
- `nIn` lies inside the ring.

Otherwise the run is "Run 1 since power-on". The text read back is sanitized:
anything that isn't printable ASCII or a newline becomes `?`.

### Writing: from the logger's event hook

`CLogger::Write ()` does three things, in this order:

1. queues an event (`WriteEvent`: source, severity, message) and calls the
   registered event notification handler;
2. sends the formatted line to its target device (devlink's serial port, once
   a host is connected);
3. for `LogPanic`, calls the panic handler and halts.

`CRunLog::EventHandler` is that notification handler. It drains the event
queue with `ReadEvent ()`, and for every message the log level lets through,
appends one line to the ring and flushes it. Step 1 comes before the USB
write, so a panic's message is in RAM before anything can go wrong on the way
out. A `LogPanic` event also sets `End = EndPanic`.

This covers every crash path in Circle:

- `CExceptionHandler` logs the exception, the stack words, the PC, the fault
  address and so on at `LogPanic` through `CLogger::Write`;
- `assert ()` and `LOGPANIC ()` go the same way.

Nothing else in the app reads the logger's event queue. A new user of
`ReadEvent ()` or `RegisterEventNotificationHandler ()` would compete with
the run log.

### How a run ends

| `End` | Set by | Report at the next boot |
|---|---|---|
| `EndRunning` | `Initialize ()`, at boot | "stopped without a word (hang: the watchdog; or a reset)", with its last lines |
| `EndPanic` | the event hook, on a `LogPanic` message | "crashed", with its last lines (the panic or exception last) |
| `EndRestart` | `CRunLog::Restarting ()`, before a `reboot ()` that was asked for | "restarted as asked", no lines |

`Restarting ()` is called:

- before the reboot on the host's reboot magic: `CDevLink` calls its
  `RegisterRebootHandler` hook, which the kernel points at
  `CRunLog::Restarting`, because devlink is shared with apps that don't have
  a run log;
- on the installer's `PGI REBOOT`;
- on `ShutdownReboot` in `main.cpp`.

The page's Reset and Stop send the reboot magic, so they count as asked for.
A new way to restart must call `Restarting ()` too, or its restarts will show
up as "stopped without a word".

### Reading: at the next boot

`Initialize ()` runs right after the logger's `Initialize ()`, before this
run logs anything:

1. checks the header;
2. copies the previous run's last 10 lines (`MaxLines`) into
   `m_PreviousLines`;
3. starts this run's header (`nRun + 1`, `EndRunning`, an empty ring);
4. registers the event hook.

`Report ()` then logs the result, after the build and throttle lines. The
report is ordinary log output: `devtools/run.sh --log` shows it, because
devlink replays the boot log when a host connects.

Getters for the page or the installer's INFO: `GetPreviousEnd ()` with
`GetEndName ()` (`none` / `stopped` / `crash` / `restart`), `GetRun ()` and
`GetPreviousLines ()`.

The report covers only the run just before this one. The next restart
replaces it. In particular, the page restarts an RPi that doesn't answer when
it connects, and that restart overwrites a crash report before the page could
show it.

### Testing it

This is how it was verified on the Zero 2 W. The test commands were
temporary: they were not committed. A host line in `CKernel::HostInput ()`
ended the run in one of three ways:

```cpp
else if (strcmp (m_HostLine, "CRASHTEST panic") == 0)
{
	LOGPANIC ("Test panic %u", 42);
}
else if (strcmp (m_HostLine, "CRASHTEST abort") == 0)
{
	LOGNOTE ("Writing to 0xFFFFFFF0");
	*(volatile u32 *) (uintptr) 0xFFFFFFF0 = 1;	// data abort
}
else if (strcmp (m_HostLine, "CRASHTEST hang") == 0)
{
	LOGNOTE ("Hanging, IRQs off");
	DisableIRQs ();
	for (;;);			// the watchdog restarts it after 10 s
}
```

The fourth case, a restart that was asked for, is `devtools/run.sh --reboot`.
After each ending, wait about 25 s, then run `devtools/run.sh --log` and
look for the `runlog:` lines. Results:

| Ending | Report |
|---|---|
| Asked-for restart | "Run 2 since power-on (the previous one restarted as asked)" |
| Panic | "crashed", with `gpu: Test panic 42` as the last line |
| Data abort | "crashed", with `except: Synchronous exception (PC 0x818A4, EC 0x25, ISS 0x46, FAR 0xFFFFFFF0, …)` and the stack lines |
| Hang | "stopped without a word", with `gpu: Hanging, IRQs off` as the last line |

Never make a test crash happen at boot: the board would crash on every start,
and its card would have to be written again: from the installer page, after
starting the board over USB with the card taken out, or on a PC.

### Not verified yet

- **The Zero (32 bit):** it builds, but no test has run on it. Its firmware
  may clear or use other areas. The address test above is the way to check.
- **A short power cut:** unplugging the Zero 2 W's USB cable and plugging it
  back in gave "Run 1 since power-on" (three times, verified). A cut of a
  fraction of a second might leave the header in the RAM; the report would
  then say "stopped without a word".

## The control list checker

A V3D job is two control lists that the renderer writes byte by byte: a
binning list (the draws) and a rendering list (per 64x64 tile: loads, the
tile's sublist, stores). A wrong address, size or stride in them makes the
V3D write over other memory or hang, with no message. Since the V3D reads
the lists by DMA, nothing on the ARM side notices until something else
breaks. The checker (`drivers/v3dcheck.{h,cpp}`) reads both lists before each
job runs, in the spirit of Mesa's vc4 kernel validator (`vc4_validate.c`),
and refuses a job that fails: the job isn't run, the log says which list,
which packet and why.

| Code | What it does |
|---|---|
| `drivers/v3dcheck.{h,cpp}` | `V3DCheckJob`: the checks |
| `drivers/v3d.{h,cpp}` | the region table (`AddRegion`, `RemoveRegion`, `InRegion`), `NewBlock`/`DeleteBlock`, the hook (`SetJobCheck`) that `RunJob` and `RunRender` call first |
| `gpu/kernel.cpp` | sets the hook, unless `clcheck=off` |
| `gpu/textures.cpp`, `programs.cpp`, `commands.cpp`, `video/video.cpp` | allocate what the V3D reads or writes with `CV3D::NewBlock` (textures, shader code, GL buffers, depth/stencil buffers, video frames) |
| `gpu/display/hdmi_output.cpp` | registers the firmware's framebuffer |

### What it checks

- **Packets:** only the ones this renderer emits, each whole and inside its
  list. Anything else is "not a packet this renderer emits": a change to the
  renderer that adds a packet has to add it here too.
- **Shape:** binning: the binning mode, `START_TILE_BINNING`, then draws,
  `FLUSH` last. Rendering: the rendering mode first, tile coordinates inside
  the frame, stores after tile coordinates, one end-of-frame store, last.
- **Every address, with its extent, inside one registered block of memory:**
  - the render target (the rendering mode's buffer: raster, T or LT, 16 or 32
    bits a pixel) and every general tile load and store;
  - the binner's tile memory and tile state;
  - the sublists the rendering list branches to;
  - shader records (GL and NV), the shaders' code and uniform streams;
  - index buffers, and each vertex attribute over the vertices a draw really
    uses. For an indexed draw that means reading the index buffer for the
    smallest and largest index: piegpu points converted vertex data below its
    start (the V3D adds index x stride), so the range can't start at 0. The
    largest index must not exceed the draw's max index.

Not checked: texture addresses. They are in the uniform streams, and only the
shader code says which words are texture configs (Mesa's kernel parses the
shader for that).

### The region table

Every block the V3D may touch is registered, by its bus address and size, in
a table sorted by address (1024 entries, binary search). `CV3D::Alloc` adds
its blocks itself; the rest come from `CV3D::NewBlock`/`DeleteBlock`, which
replace `new u8[]`/`delete []` wherever the V3D reads or writes the memory;
the HDMI framebuffer, which the firmware allocates, is added and removed by
`CHDMIOutput`. A new kind of V3D memory must be registered the same way, or
its jobs are refused ("not the V3D's"). If the table ever fills up, the
checker logs it once and stops judging addresses rather than refuse valid
jobs.

### When a job is refused

`RunJob` or `RunRender` returns FALSE, as after a timeout: that job's pixels
are missing, everything else goes on. The first ten refusals are logged, then
every 1000th, e.g.:

```
v3dcheck: Job refused (1 so far): rendering list +43, packet 28: store Z/stencil at C4000000 (2490368 bytes): not the V3D's
```

(That one is real: a test build that put the depth buffer's store 9 MB past
its start, the kind of bug that corrupts memory silently. The job wasn't run,
the board went on at 60 fps with nothing rendered, and only the first ten
refusals were logged.) The run log keeps these lines across a restart.

### Cost and verification

Measured on the Zero 2 W, HDMI 1024x600 (one job a frame):

| Demo | Check time per job |
|---|---|
| gears | 170 us |
| flight | 260 us |
| toy-voronoi | 240 us |
| video (the frame copy is a second, smaller job) | 130 us |

Nearly all of it is the rendering list (160 tiles, about 1,400 packets, 6.9
KB); the binning list takes about 10 us. That's 1-2% of a 60 fps frame. These
were measured with the ARM at 600 MHz (the firmware's starting clock,
`cpu=low`; a plain byte-by-byte read of the same 6.9 KB took 45 us); at its
maximum, 1000 MHz, gears' check takes 103 us. A job refused costs nothing
more.

Verified with the check on: the demos, the video, gltest (the same 19 known
failures on HDMI as without it) and dEQP-GLES2's 2015-case subset (1999 pass,
16 fail: the same cases as before), with no job refused; and the refusal above.
`clcheck=off` turns it off.
