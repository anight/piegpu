# pico-gpu

An OpenGL ES 2.0 GPU for microcontrollers: a Raspberry Pi Zero runs bare metal
(Circle) and renders with its VideoCore IV V3D. The host (a Raspberry Pi
Pico 2 W, an ESP32-P4, or a PC for tests) sends GL commands over a link. The
frames go to an ST7789 panel or to HDMI. Video (H.264, e.g. from an MP4) is
decoded by the VideoCore into textures that any draw can use. The protocol is
in [docs/protocol.md](docs/protocol.md).

| Directory | What |
|---|---|
| `gpu/` | the Zero's firmware: links (`link/`), outputs (`display/`), renderer, video (`video/`) |
| `libpgpu/` | the host library: protocol encoding, pgl (the GL ES API), MP4 reader, HUD, self tests |
| `transports/` | links for the host library: `pico-i2s`, `esp32p4-i2s`, `pc-usb` |
| `hosts/` | builds per host board: `pico`, `esp32p4`, `pc`, `web` (a page, WebAssembly) |
| `demos/` | the demos, for every host |
| `protocol/` | the wire format header, shared by both sides |
| `devtools/` | boot the Zero over USB, logs, screenshots (`run.sh`) |
| `web/installer/` | a page that installs pico-gpu on the Zero's SD card over USB |

## Boards and building

pico-gpu runs on a Raspberry Pi Zero / Zero W (32 bit, `kernel.img`) and on a
Zero 2 W (64 bit, `kernel8.img`; render 0.9 ms a gears frame, the Zero's 1.2). Circle builds in its source tree, so each board has its own: `circle`
and `circle-zero2` (made from `circle` and `patches/`). Toolchains: Arm GNU
15.2 in `~/toolchains` (`arm-none-eabi` and `aarch64-none-elf`).

```bash
devtools/configure-circle.sh zero2
```

```bash
devtools/build-gpu.sh all
```

Video decoding works the same in both: MMAL's messages to the VideoCore have
the VideoCore's 32-bit layout on any client (`gpu/video/userland/interface/
mmal/vc/mmal_vc_msgs.h`, checked against the 32-bit compiler's by
`devtools/mmal-layout.py`); Circle's vcos is used in 64 bit too
(`patches/circle-vcos-aarch64.patch`), and the MMAL sources are built with
`-mstrict-align` there (they read VCHIQ's messages in place, in Circle's
coherent region: Device memory in 64 bit, where unaligned accesses fault).
Verified on both boards: 640x360 to 1280x720, 0 dropped. The Zero 2 W's HDMI hot-plug line is GPIO28 (the
Zero's: GPIO46). `web/installer/make-firmware.sh`
puts both builds on the installer page, which picks the one for the board.

A restart explains itself (`devtools/runlog.h`): the Zero keeps its log in
the top 64 KB of the ARM memory, which Circle leaves to the app
(`patches/circle-persistent-memory.patch`, `MEM_PERSISTENT_SIZE`) and the
firmware doesn't clear at a restart (the low memory it does: the Zero 2 W's
first ~4 MB). At boot the log says how the previous run ended, with its last
lines unless it restarted as asked:

```
runlog: Run 4 since power-on: the previous one crashed; its last lines:
runlog: | 30.33 gpu: Writing to 0xFFFFFFF0
runlog: | 30.33 except: stack[1] is 0x81BDC
runlog: | 30.33 except: stack[19] is 0x8082C
runlog: | 30.33 except: Synchronous exception (PC 0x818A4, EC 0x25, ISS 0x46, FAR 0xFFFFFFF0, SP 0x27C0A0, LR 0x8189C, SPSR 0x60000304)
```

"crashed" is an exception, assertion or panic (its message last); "stopped
without a word" a hang the watchdog caught, or a reset; "restarted as asked"
the host's reboot, the installer's, Reset. Verified on the Zero 2 W (all
four); the Zero not yet. How it works: [DEVELOPMENT.md](DEVELOPMENT.md).

## Wiring

```
                  I2S link + READY/FRAME            SPI0
   ┌────────────┐  (6 signals + GND)   ┌────────────┐   ┌────────────────┐
   │    host    │ ───────────────────► │  Pi Zero   │──►│ ST7789 320x240 │
   │ Pico 2 W   │ ◄─────────────────── │   (GPU)    │   └────────────────┘
   │ or ESP32-P4│                      │            │ mini-HDMI ┌─────────┐
   └────────────┘                      │            │──────────►│ monitor │
                                       └─────┬──────┘           └─────────┘
                                             │ "USB" port: power, boot and log
                                             ▼ (devtools/run.sh)
                                            PC
```

Pin numbers below are **physical header pins** with the GPIO number in
brackets. All signals are 3.3 V. Connect the grounds of both boards.

### Host ↔ Zero: the link

The Zero's PCM (I2S) block is the slave. The host drives the bit clock
(BCLK), the frame sync (FS) and the command data. The Zero answers on REPLY,
and signals READY (room for a packet) and FRAME (a frame went to the screen).

| Signal | Direction | Zero | Pico 2 W | ESP32-P4 (Waveshare dev kit) |
|---|---|---|---|---|
| DATA | host → Zero | 38 (GPIO20, PCM_DIN) | 21 (GP16) | 11 (GPIO21) |
| BCLK | host → Zero | 12 (GPIO18, PCM_CLK) | 22 (GP17) | 13 (GPIO20) |
| FS | host → Zero | 35 (GPIO19, PCM_FS) | 24 (GP18) | 12 (GPIO22) |
| REPLY | Zero → host | 40 (GPIO21, PCM_DOUT) | 25 (GP19) | 7 (GPIO23) |
| READY | Zero → host | 36 (GPIO16) | 26 (GP20) | 18 (GPIO4) |
| FRAME | Zero → host | 37 (GPIO26) | 27 (GP21) | 16 (GPIO5) |
| GND | — | 39, 34 | 23, 28 | 9, 14, 20, 25 |

- **READY needs an external 10 kΩ pull-down to GND on the host side.** While
  the Zero boots, READY must read low so that the host doesn't stream into a
  Zero that isn't running. On the RP2350 the internal pull-down alone isn't
  enough (erratum E9, docs/protocol.md §3.1).
- The ESP32-P4 pins are on the dev kit's 40-pin header, which has the
  Raspberry Pi layout. The bit clock is 31.25 MHz on a v1.x chip (the dev
  kit's; its most) and 40 MHz from chip revision v3.0 on
  (`transports/esp32p4-i2s`).
- On the Pico only physical pins 21–27 are used (GP16–GP21 and GND).

### Zero ↔ ST7789 panel

The panel is a 240x320 ST7789 module (14 pins) on SPI0 at 75 MHz, driven in
landscape as 320x240 (`gpu/display/panel_output`).

| Panel | Zero |
|---|---|
| CS | 24 (GPIO8, SPI0 CE0) |
| SCK | 23 (GPIO11, SPI0 SCLK) |
| SDI (MOSI) | 19 (GPIO10, SPI0 MOSI) |
| DC | 18 (GPIO24) |
| RESET | 22 (GPIO25) |
| SDO (MISO) | 21 (GPIO9, SPI0 MISO) |

At boot the Zero reads the panel's ID over SDO, so it knows whether a panel is
there. Without the SDO wire it can't tell: set `panel=yes` (see below).

### Zero ↔ HDMI, and the PC

- **HDMI:** a monitor on the mini-HDMI port. By default the screen is on HDMI
  while a monitor is connected and on the panel otherwise; the Zero watches the
  hot-plug line and reads the monitor's EDID for its resolution.
  `devtools/config.txt` has `hdmi_force_hotplug=1` so that HDMI stays on when
  the Zero boots without a monitor.
- **PC:** the Zero's "USB" port (not "PWR IN"). Without a card that holds
  pico-gpu the Zero boots over USB from `rpiboot`; `devtools/run.sh gpu`
  builds, boots and logs. The same cable powers it and carries its log, the
  PC's GL commands (below) and the USB monitor.

### Kernel command line (`cmdline.txt`)

| Option | Meaning |
|---|---|
| `host=auto` | the default: GL commands from a PC over USB once it opens its stream (until the Zero restarts), else from I2S |
| `host=usb`, `host=i2s` | only a PC over USB, or only a Pico / ESP32-P4 over I2S (the log, the installer and the USB monitor work either way) |
| `gud=on` | the default: the Zero is also a USB monitor for a Linux PC (below) |
| `gud=off` | no monitor for a PC's desktop to take (the serial port and the GL interface stay) |
| `output=auto` | the default: HDMI while a monitor is connected, else the panel |
| `output=panel`, `output=hdmi` | always this output |
| `panel=auto` | the default: a panel if one answers on SDO (MISO) at boot |
| `panel=yes`, `panel=none` | a panel is there (SDO not wired) or none is; without a panel and a monitor the screen stays on HDMI |
| `hdmi_pixels=N` | cap the screen on HDMI to N pixels (default: the monitor's native resolution, up to 1920x1200) |

With `devtools/run.sh`, pass these as `CMDLINE="output=panel" devtools/run.sh gpu`.

## OpenGL ES from a PC (`hosts/pc`)

A Linux PC can be the host too: the same GL ES 2.0 API (`pgl`, with the GL ES
1.1 fixed-function calls) and the same command packets, over the Zero's USB
serial port instead of I2S (`transports/pc-usb`). GLSL is compiled at run
time on the PC by `tools/glslc` (Mesa's vc4 compiler, offline; results
cached in `~/.cache/pgpu-glslc`). There is no EGL: the Zero's screen is the
window (`pglInit`, `pglGetScreenSize`, `pglSwapBuffers`; `pglInitSurface` for
an off-screen one). `hosts/pc/videoplay.c` is an example program.

```bash
cmake -S hosts/pc -B hosts/pc/build && make -C hosts/pc/build
```

- Over USB there is no FRAME line: `pgpu_wait_frame` sends a PING instead,
  whose answer comes once the Zero has executed the frame (and waited for
  the screen), so programs pace on the Zero's frames as on the Pico's. The
  demos build for the PC as `DEMO_host` (`hosts/pc/build/gears_host`: 60
  fps on the panel, render 1.2 ms a frame).
- While a PC's desktop uses the Zero as a monitor (below), GL frames are
  rendered off screen: turn that monitor off first.
- Two ways over the cable (`transports/pc-usb/pgpu_host.c`): the Zero's GL
  interface (a vendor interface with a bulk endpoint each way, libusb), when
  the PC may open the device; else its serial port, found by name
  (`/dev/serial/by-id/usb-pico-gpu_pico-gpu_*`; `PGPU_TTY` picks it and
  overrides the GL interface). Opening the device takes a udev rule:
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614d", TAG+="uaccess"`
  in `/etc/udev/rules.d/70-pico-gpu.rules`.
- Each program starts with 64 KB of zeros and a PING: a program killed in
  the middle of a packet leaves nothing for the next one to trip on.

Measured with `hosts/pc/build/linktest_host` (libpgpu/test/linktest.c: 64 KB
uploads of random data, and random 256x256 textures read back and compared
word by word; 30 s):

| | PC, USB | ESP32-P4, I2S 31.25 MHz |
|---|---|---|
| to the Zero | 8.3 MB/s | 3.9 MB/s |
| back from it | 6.1 MB/s | 2.37 MB/s |
| errors | 0 of 6.0M words | 0 |

Tests from the PC:

- `hosts/pc/build/gltest_host`: self test 8 (83 checks through `gl*` calls).
- dEQP-GLES2 (the Khronos conformance tests): `tools/deqp/build-deqp.sh`
  builds it with a `pgl` platform; `tools/deqp/run-deqp.py --pbuffer
  PATTERN...` runs cases as Mesa's CI does (a 256x256 RGBA8888 off-screen
  surface), in batches that go on after a crash; `compare-vc4.py` puts the
  results next to Mesa's vc4 on a Raspberry Pi 3 and marks the cases outside
  Khronos' mustpass list. The runner reboots the Zero at the end: not while a
  desktop uses it as a monitor.

Results (2026-09-29): self test 8, 83 of 83. dEQP-GLES2, 2015 cases (clears,
depth/stencil, texture filtering, wrap and mipmaps, shader structs and
functions, FBOs, vertex arrays, random draws; 9.5 minutes): 1999 pass, 16
fail, the same as the full run of 2026-09-27 (17485 cases: 17163 pass, 24
fail, 290 not supported). Of the 16, 11 fail with Mesa's vc4 too (the V3D),
5 are ETC1 wrap cases outside mustpass; on mustpass, 1946 of 1957 pass.

## Installing on an SD card (`web/installer`)

A static page puts pico-gpu on the microSD card in the Zero, over the Zero's
USB port, with its settings (the screen, the panel, the HDMI mode). It needs
Chrome or Edge on a desktop (WebUSB and Web Serial) and a secure origin
(https, or `localhost`).

```bash
web/installer/make-firmware.sh
```

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory web/installer
```

Then open http://localhost:8765 and follow the steps:

- **A blank or new card:** the Zero's boot ROM finds nothing to start and waits
  for USB. "Start a blank Zero" boots pico-gpu from the page (WebUSB, the
  rpiboot protocol: `rpiboot.js`). The page serves both builds and a
  `config.txt` whose `[pi02]` lines would make a Zero 2 W ask for its 64-bit
  `kernel8.img` (a Zero: `kernel.img`); once pico-gpu runs it says which board
  it is, and Install writes that board's files only. A card without a FAT
  file system can be formatted there (one FAT32 partition). **A Zero 2 W**
  didn't start over USB here, with Raspberry Pi's own `rpiboot` either (its
  boot ROM took `bootcode.bin`, then stopped answering): write its card on a
  PC instead (one FAT32 partition, type 0x0c; the files of
  `web/installer/firmware/zero2`, a `config.txt` with `arm_64bit=1`, a
  `cmdline.txt`), then Connect and Install work as on a Zero.
- **A card with pico-gpu:** "Connect to pico-gpu" (Web Serial). Install again,
  or change the settings only. If pico-gpu doesn't answer (a program left it
  taking GL commands), the page restarts it; "Reset" restarts it any time.
- **A card with another system:** take it out, start the Zero from the page,
  and put the card in when the page asks.

pico-gpu writes the files itself (`gpu/install`): each is checked by its CRC,
then renamed into place. At the end the Zero restarts from the card, and the
page shows where the screen went. Measured: 3.5 MB in 5.4 s; a 64 GB card
formatted in 7.3 s.

**Test OpenGL, Test video.** The page runs demos in the browser, with
libpgpu compiled to WebAssembly (`hosts/web`, Emscripten), their GL commands
going over the same Web Serial port as the installer's (the PC's serial
transport; `web/installer/gl.js` moves the bytes). No driver or udev rule is
needed. Test OpenGL runs `demos/gears.c` (60 fps, render 1.2 ms, as
`gears_host`); Test video runs `demos/video.c` on the Sintel trailer (854x480
H.264, Blender Foundation, CC BY 3.0; the MP4's title is the bottom line),
decoded by the Zero's VideoCore: 24 fps, 0 dropped. Stop restarts the Zero,
which takes commands from the serial port until then. `make-firmware.sh`
builds the demos when Emscripten is installed (`EMSDK`, default `~/emsdk`)
and downloads the trailer (Blender's server doesn't let a page fetch it).

## The Zero as a USB monitor for Linux

Over its USB port the Zero is also a monitor for a Linux PC, with no driver
to install: GUD, the kernel's Generic USB Display (`gud`). The Zero is one
USB device, `1d50:614d` (the ID GUD's driver binds to), with the serial port
as before and a display. While the PC has the display on, its desktop takes
the Zero's screen (the panel, or HDMI), and a GL host's frames are rendered
off screen until the PC turns it off (`gpu/display/gud_display`).

- The PC sees one connector with one mode, the Zero's screen size, and an
  EDID: GNOME calls it "PGU pico-gpu".
- `devtools/gudtest.c` drives it through DRM without a desktop: a test
  pattern and two rates. Measured at 320x240: 110 full frames a second
  (17 MB/s), 60 small updates a second.
- GNOME 50.1: it draws no pointer on this monitor while another one has a
  hardware cursor (newer mutter decides per monitor).
  `MUTTER_DEBUG_DISABLE_HW_CURSORS=1` for GNOME Shell gives software cursors
  everywhere, e.g. in a drop-in for its unit:
  `~/.config/systemd/user/org.gnome.Shell@ubuntu.service.d/*.conf` with
  `[Service]` and `Environment=MUTTER_DEBUG_DISABLE_HW_CURSORS=1`.
- Don't unplug or reboot the Zero while GNOME uses it as a monitor: GNOME
  Shell 50.1 crashed once when it came back within seconds (it keeps the old
  device, and the new one had the same `/dev/dri` name). For a desktop that
  must leave it alone, set `gud=off` on the Zero, or tag it in udev: `SUBSYSTEM=="drm", KERNEL=="card*",
  ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614d", TAG+="mutter-device-ignore"`.
