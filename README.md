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
  rpiboot protocol: `rpiboot.js`). A card without a FAT file system can be
  formatted there (one FAT32 partition).
- **A card with pico-gpu:** "Connect to pico-gpu" (Web Serial). Install again,
  or change the settings only.
- **A card with another system:** take it out, start the Zero from the page,
  and put the card in when the page asks.

pico-gpu writes the files itself (`gpu/install`): each is checked by its CRC,
then renamed into place. At the end the Zero restarts from the card, and the
page shows where the screen went. Measured: 3.5 MB in 5.4 s; a 64 GB card
formatted in 7.3 s.

**Try it: Run gears.** The page runs gears in the browser: `demos/gears.c`
with libpgpu compiled to WebAssembly (`hosts/web`, Emscripten), its GL
commands going over the same Web Serial port as the installer's (the PC's
serial transport; `web/installer/gl.js` moves the bytes). No driver or udev
rule is needed. Stop restarts the Zero, which takes commands from the serial
port until then. `make-firmware.sh` builds the demo when Emscripten is
installed (`EMSDK`, default `~/emsdk`); measured from Node against the
Zero: 60 fps, render 1.2 ms, as `gears_host`.

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
