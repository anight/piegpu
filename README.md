# piegpu

A toy GPU for OpenGL and video rendering. A Raspberry Pi (RPi) runs bare
metal (Circle) as the graphics card of a microcontroller (like a Raspberry Pi
Pico or an ESP32): the microcontroller sends OpenGL ES 2.0 commands (and 1.1's
fixed function), and the RPi renders them with its VideoCore IV V3D, to an
ST7789 panel or to HDMI. Video (H.264, e.g. from an MP4) is decoded by the
VideoCore into textures that any draw can use, and its sound (AAC) is
decoded by the RPi and played on HDMI, with the picture following it.

The host can be any microcontroller board capable of the link: I2S as the
master (DATA, BCLK and FS out, the replies back on REPLY), a READY input and
optionally FRAME, at 3.3 V ([docs/protocol.md](docs/protocol.md) §2–3). The
host library (`libpgpu`: the protocol, pgl for the GL ES API, an MP4 reader,
a HUD, in C) and the demos are shared by every board; a board adds only a
transport, the few functions of `libpgpu/pgpu_link.h`. Tested on:

- a Raspberry Pi Pico 2 W (RP2350), over I2S;
- an ESP32-P4 (Waveshare ESP32-P4-Module-DEV-KIT), over I2S.

A Linux PC, or a page in Chrome, can be the host too, over the RPi's USB port.
Over the same port the RPi also installs itself on its SD card (from a web
page) and serves as a monitor for a Linux desktop.

**Supported for now: the Raspberry Pi Zero / Zero W and the Zero 2 W.** More
RPi boards are to come; "RPi" below means the board piegpu runs on, and a
model is named where only that one was verified.

Documentation ([docs/](docs/README.md)):

- [docs/protocol.md](docs/protocol.md): the wire protocol;
- [docs/host-library.md](docs/host-library.md): the host library, libpgpu
  (pgl, the GL ES API);
- [docs/development.md](docs/development.md): how the RPi side works inside.

| Directory | What |
|---|---|
| `gpu/` | the RPi's firmware: links (`link/`), outputs (`display/`), renderer, video (`video/`), audio (`audio/`, with FAAD2), SD card installer (`install/`) |
| `drivers/` | the RPi's V3D and ST7789 (DMA) drivers |
| `libpgpu/` | the host library: protocol encoding, pgl (the GL ES API), MP4 reader, HUD, self tests |
| `transports/` | links for the host library: `pico-i2s`, `esp32p4-i2s`, `pc-usb` |
| `hosts/` | builds per host: `pico`, `esp32p4`, `pc`, `web` (a page, WebAssembly) |
| `demos/` | the demos, for every host |
| `protocol/` | the wire format header, shared by both sides |
| `devtools/` | Circle setup and builds per board, the RPi's USB device (serial port, GL interface, monitor), the run log, `run.sh` (boot an RPi over USB, logs, screenshots) |
| `tools/` | `glslc` (GLSL compiler: Mesa's vc4, offline), `deqp` (the conformance tests) |
| `patches/` | a local change to Mesa, for `tools/glslc` (Circle's are in its fork, below) |
| `web/installer/` | a page that installs piegpu on the RPi's SD card over USB and runs demos on it |
| `experiments/` | the early probes and demos that led here (links, displays, V3D, video) |

## Boards and building

piegpu supports two boards for now:

- a Raspberry Pi Zero or Zero W: 32 bit, `kernel.img`, 1.2 ms to render a
  gears frame;
- a Zero 2 W: 64 bit, `kernel8.img`, 0.9 ms.

piegpu builds with its fork of Circle: the branch `piegpu` of
[github.com/anight/circle](https://github.com/anight/circle/tree/piegpu),
Circle Step51.1 with piegpu's changes as commits (the USB gadget's CDC and
EP0, FatFs' `f_mkfs`, `MEM_PERSISTENT_SIZE`, vcos in 64 bit). Circle builds
in its source tree, so each board has its own copy, `circle` and
`circle-zero2`: `configure-circle.sh` clones the fork into the one it's
given if it isn't there, then configures and builds it.

Toolchains: Arm GNU 15.2 in `~/toolchains` (`arm-none-eabi` and
`aarch64-none-elf`).

```bash
devtools/configure-circle.sh zero
```

```bash
devtools/configure-circle.sh zero2
```

```bash
devtools/build-gpu.sh all
```

This gives `gpu/kernel.img` (the Zero) and `build/zero2/gpu/kernel8.img` (the
Zero 2 W). `web/installer/make-firmware.sh` puts both builds on the installer
page, which picks the one for the board.

Video decoding works the same on both boards (in 64 bit too:
[docs/development.md](docs/development.md#video)). Verified on both boards:
640x360 to 1280x720, 0 dropped.

### Why the RPi restarted: the run log

A restart explains itself (`devtools/runlog.h`): the RPi keeps its log in
the top 64 KB of the ARM memory, which Circle leaves to the app
(`MEM_PERSISTENT_SIZE`, in piegpu's fork of Circle) and the
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
four); the Zero not yet. How it works:
[docs/development.md](docs/development.md#the-run-log-a-restart-explains-itself).

## Wiring

```
                  I2S link + READY/FRAME            SPI0
   ┌────────────┐  (6 signals + GND)   ┌────────────┐   ┌────────────────┐
   │  host MCU  │ ───────────────────► │    RPi     │──►│ ST7789 320x240 │
   │ (Pico 2 W, │ ◄─────────────────── │   (GPU)    │   └────────────────┘
   │  ESP32-P4) │                      │            │ mini-HDMI ┌─────────┐
   └────────────┘                      │            │──────────►│ monitor │
                                       └─────┬──────┘           └─────────┘
                                             │ "USB" port, one cable:
                                             │  power; its log; USB boot
                                             │  GL commands and video from a PC
                                             │    or a page (instead of I2S)
                                             │  the installer (web/installer)
                                             │  a monitor for the PC's desktop
                                             ▼
                                            PC
```

Pin numbers below are **physical header pins** with the GPIO number in
brackets. All signals are 3.3 V. Connect the grounds of both boards. The
Zero and the Zero 2 W have the same 40-pin header, so the same wiring.
Verified on both: gears from the ESP32-P4 over I2S, on the panel.

### Host ↔ RPi: the link

The RPi's PCM (I2S) block is the slave. The host drives the bit clock
(BCLK), the frame sync (FS) and the command data. The RPi answers on REPLY,
and signals READY (room for a packet) and FRAME (a frame went to the screen).

| Signal | Direction | RPi | Pico 2 W | ESP32-P4 (Waveshare dev kit) |
|---|---|---|---|---|
| DATA | host → RPi | 38 (GPIO20, PCM_DIN) | 21 (GP16) | 11 (GPIO21) |
| BCLK | host → RPi | 12 (GPIO18, PCM_CLK) | 22 (GP17) | 13 (GPIO20) |
| FS | host → RPi | 35 (GPIO19, PCM_FS) | 24 (GP18) | 12 (GPIO22) |
| REPLY | RPi → host | 40 (GPIO21, PCM_DOUT) | 25 (GP19) | 7 (GPIO23) |
| READY | RPi → host | 36 (GPIO16) | 26 (GP20) | 18 (GPIO4) |
| FRAME | RPi → host | 37 (GPIO26) | 27 (GP21) | 16 (GPIO5) |
| GND | — | 39, 34 | 23, 28 | 9, 14, 20, 25 |

- **READY needs an external 10 kΩ pull-down to GND on the host side.** While
  the RPi boots, READY must read low so that the host doesn't stream into an
  RPi that isn't running. On the RP2350 the internal pull-down alone isn't
  enough (erratum E9, docs/protocol.md §3.1).
- The ESP32-P4 pins are on the dev kit's 40-pin header, which has the
  Raspberry Pi layout. The bit clock is 31.25 MHz on a v1.x chip (the dev
  kit's; its most) and 40 MHz from chip revision v3.0 on
  (`transports/esp32p4-i2s`).
- On the Pico the link uses only physical pins 21–27 (GP16–GP21 and GND).

### RPi ↔ ST7789 panel

The panel is a 240x320 ST7789 module (14 pins) on SPI0 at 75 MHz, driven in
landscape as 320x240 (`gpu/display/panel_output`).

| Panel | RPi |
|---|---|
| CS | 24 (GPIO8, SPI0 CE0) |
| SCK | 23 (GPIO11, SPI0 SCLK) |
| SDI (MOSI) | 19 (GPIO10, SPI0 MOSI) |
| DC | 18 (GPIO24) |
| RESET | 22 (GPIO25) |
| SDO (MISO) | 21 (GPIO9, SPI0 MISO) |

At boot the RPi reads the panel's ID over SDO, so it knows whether a panel is
there. Without the SDO wire it can't tell: set `panel=yes` (see below).

### RPi ↔ HDMI, and the PC

- **HDMI:** a monitor on the mini-HDMI port. By default the screen is on HDMI
  while a monitor is connected and on the panel otherwise.
  - The RPi watches the hot-plug line: GPIO46 on a Zero, GPIO28 on a Zero 2
    W.
  - It reads the monitor's EDID for the resolution.
  - `config.txt` has `hdmi_force_hotplug=1` (`devtools/config.txt`, and the
    installer's), so that HDMI stays on when the RPi boots without a
    monitor.
- **PC:** the RPi's "USB" port (not "PWR IN"). The one cable carries:
  - power;
  - the RPi's log;
  - the installer (below);
  - GL commands and video from a PC program or a page (below);
  - the USB monitor (below).

  Without a card that holds piegpu, many RPi boards can boot over USB from
  a PC with `rpiboot` (Raspberry Pi's usbboot lists them). Both supported
  boards do, from the installer page (below) or `rpiboot`:
  - `devtools/run.sh gpu` builds, boots and logs, with the Zero's 32-bit
    build (`make -C gpu`).
  - A Zero 2 W boots its 64-bit build the same way: `rpiboot -d` on a folder
    with the files of `web/installer/firmware/zero2`, a `config.txt` with
    `arm_64bit=1` and a `cmdline.txt`. Verified with usbboot's
    `bootcode.bin` there (its `msd` one), and from the page with Circle's.
  - `devtools/run.sh --log` captures the log of either board.

### Kernel command line (`cmdline.txt`)

| Option | Meaning |
|---|---|
| `host=auto` | the default: GL commands from a PC over USB once it opens its stream (until the RPi restarts), else from I2S |
| `host=usb`, `host=i2s` | only a PC over USB, or only a Pico / ESP32-P4 over I2S (the log, the installer and the USB monitor work either way) |
| `gud=on` | the default: the RPi is also a USB monitor for a Linux PC (below) |
| `gud=off` | no monitor for a PC's desktop to take (the serial port and the GL interface stay) |
| `output=auto` | the default: HDMI while a monitor is connected, else the panel |
| `output=panel`, `output=hdmi` | always this output |
| `panel=auto` | the default: a panel if one answers on SDO (MISO) at boot |
| `panel=yes`, `panel=none` | a panel is there (SDO not wired) or none is; without a panel and a monitor the screen stays on HDMI |
| `hdmi_pixels=N` | cap the screen on HDMI to N pixels (default: the monitor's native resolution, up to 1920x1200) |
| `cpu=max`, `cpu=low` | the ARM at its maximum clock (the default; the Zero 2 W: 1000 MHz, throttled by the firmware at its own temperature limit) or at the firmware's starting clock (600 MHz) |
| `volume=N` | the sound's volume on HDMI, percent (default: 10) |
| `clcheck=on`, `clcheck=off` | check each V3D job's control lists before it runs, and refuse a bad one (the default: on; [docs/development.md](docs/development.md#the-control-list-checker)) |

With `devtools/run.sh`, pass these as `CMDLINE="output=panel" devtools/run.sh gpu`.

## Host boards (`hosts/pico`, `hosts/esp32p4`)

The two tested boards. Both builds make the self tests and the demos
(`demos/demos.cmake`), plus the Jet demos (`demos/jet`). Another board needs
its own transport (`libpgpu/pgpu_link.h`; `transports/pico-i2s` and
`transports/esp32p4-i2s` are the examples) and a build like these.

**Raspberry Pi Pico 2 W:** the Pico SDK (2.2.0 here, `PICO_SDK_PATH`). The
build makes one `.uf2` per program in `hosts/pico/build`:

- `gpulink.uf2`: the self tests;
- `gears.uf2`, `toy-pong.uf2`, `jet-viewer.uf2` and so on: the demos.

The console is on UART0 (GP0/GP1, pins 1 and 2: a Debugprobe's UART bridge).

```bash
cmake -S hosts/pico -B hosts/pico/build -G Ninja
```

```bash
ninja -C hosts/pico/build
```

**ESP32-P4** (Waveshare ESP32-P4-Module-DEV-KIT): ESP-IDF v5.5. One program
per build, chosen with `PGPU_APP`:

- `selftest`, the default;
- a demo: `gears`, `toy-NAME`, `jet-NAME` and so on;
- `pincheck`: the pins alone, before wiring the RPi.

```bash
. ~/esp/esp-idf-v5.5/export.sh
```

```bash
idf.py -C hosts/esp32p4 -B hosts/esp32p4/build-gears -DPGPU_APP=gears build flash monitor
```

## OpenGL ES from a PC (`hosts/pc`)

A Linux PC can be the host too. It uses the same GL ES 2.0 API (`pgl`, with
the GL ES 1.1 fixed-function calls) and the same command packets, but over the
RPi's USB port instead of I2S (`transports/pc-usb`). GLSL is compiled at run
time on the PC by `tools/glslc` (Mesa's vc4 compiler, offline; results
cached in `~/.cache/pgpu-glslc`). There is no EGL: the RPi's screen is the
window (`pglInit`, `pglGetScreenSize`, `pglSwapBuffers`; `pglInitSurface` for
an off-screen one). `hosts/pc/videoplay.c` is an example program.

```bash
cmake -S hosts/pc -B hosts/pc/build && make -C hosts/pc/build -j$(($(nproc) - 1))
```

- Over USB there is no FRAME line: `pgpu_wait_frame` sends a PING instead,
  whose answer comes once the RPi has executed the frame (and waited for
  the screen), so programs pace on the RPi's frames as on the Pico's. The
  demos build for the PC as `DEMO_host` (`hosts/pc/build/gears_host`: 60
  fps on the panel, render 1.2 ms a frame).
- While a PC's desktop uses the RPi as a monitor (below), GL frames are
  rendered off screen: turn that monitor off first.
- Two ways over the cable (`transports/pc-usb/pgpu_host.c`): the RPi's GL
  interface (a vendor interface with a bulk endpoint each way, libusb), when
  the PC may open the device; else its serial port, found by name
  (`/dev/serial/by-id/usb-piegpu_piegpu_*`; `PGPU_TTY` picks it and
  overrides the GL interface). Opening the device takes a udev rule:
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614d", TAG+="uaccess"`
  in `/etc/udev/rules.d/70-piegpu.rules`.
- Each program starts with 64 KB of zeros and a PING: a program killed in
  the middle of a packet leaves nothing for the next one to trip on.

Measured with `hosts/pc/build/linktest_host` (libpgpu/test/linktest.c: 64 KB
uploads of random data, and random 256x256 textures read back and compared
word by word; 30 s):

| | PC, USB | ESP32-P4, I2S 31.25 MHz |
|---|---|---|
| to the RPi | 8.3 MB/s | 3.9 MB/s |
| back from it | 6.1 MB/s | 2.37 MB/s |
| errors | 0 of 6.0M words | 0 |

Tests from the PC:

- `hosts/pc/build/gltest_host`: self test 8 (83 checks through `gl*` calls).
- dEQP-GLES2 (the Khronos conformance tests): `tools/deqp/build-deqp.sh`
  builds it with a `pgl` platform; `tools/deqp/run-deqp.py --pbuffer
  PATTERN...` runs cases as Mesa's CI does (a 256x256 RGBA8888 off-screen
  surface), in batches that go on after a crash; `compare-vc4.py` puts the
  results next to Mesa's vc4 on a Raspberry Pi 3 and marks the cases outside
  Khronos' mustpass list. The runner reboots the RPi at the end: not while a
  desktop uses it as a monitor.

Results (2026-09-29): self test 8, 83 of 83. dEQP-GLES2, 2015 cases (clears,
depth/stencil, texture filtering, wrap and mipmaps, shader structs and
functions, FBOs, vertex arrays, random draws; 9.5 minutes): 1999 pass, 16
fail, the same as the full run of 2026-09-27 (17485 cases: 17163 pass, 24
fail, 290 not supported). Of the 16, 11 fail with Mesa's vc4 too (the V3D),
5 are ETC1 wrap cases outside mustpass; on mustpass, 1946 of 1957 pass.

## Installing on an SD card (`web/installer`)

A static page puts piegpu on the microSD card in the RPi, over the RPi's
USB port, with its settings. The settings are the command line above
(`cmdline.txt`) and `config.txt`:

- where the GL commands come from;
- the screen and the panel;
- the HDMI mode;
- the USB monitor.

The page needs Chrome or Edge on a desktop (WebUSB and Web Serial) and a
secure origin (https, or `localhost`).

```bash
web/installer/make-firmware.sh
```

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory web/installer
```

Then open http://localhost:8765 and follow the steps:

- **A blank or new card:** the RPi's boot ROM finds nothing to start and waits
  for USB. "Start a blank RPi" boots piegpu from the page (WebUSB, the
  rpiboot protocol: `rpiboot.js`; verified on both boards).
  - The page serves both builds, and a `config.txt` whose `[pi02]` lines would
    make a Zero 2 W ask for its 64-bit `kernel8.img` (a Zero asks for
    `kernel.img`).
  - Once piegpu runs, it says which board it is, and Install writes that
    board's files only.
  - A card without a FAT file system can be formatted there (one FAT32
    partition).
- **If a USB start stalls:** twice, early on, a Zero 2 W's boot ROM took
  `bootcode.bin` and then stopped answering, with `rpiboot` too; the cause
  wasn't found. It then answers nothing until it loses power: unplug the
  cable, plug it back in, and start again. Every start since has worked,
  from the page and from `rpiboot`, with a blank card in the board or none.
  Writing the card on a PC works too (verified on a Zero 2 W): one FAT32
  partition (type 0x0c), the files of `web/installer/firmware/<board>`, a
  `config.txt` (`arm_64bit=1` for a Zero 2 W) and a `cmdline.txt`.
- **A card with piegpu:** "Connect" (Web Serial). Install again,
  or change the settings only. If piegpu doesn't answer (a program left it
  taking GL commands), the page restarts it; "Reset" restarts it any time.
- **A card with another system:** take it out, start the RPi from the page,
  and put the card in when the page asks.

piegpu writes the files itself (`gpu/install`): each is checked by its CRC,
then renamed into place. At the end the RPi restarts from the card, and the
page shows where the screen went. Measured: 3.5 MB in 5.4 s; a 64 GB card
formatted in 7.3 s.

**Test OpenGL, Test video.** The page runs demos in the browser, with
libpgpu compiled to WebAssembly (`hosts/web`, Emscripten), their GL commands
going over the same Web Serial port as the installer's (the PC's serial
transport; `web/installer/gl.js` moves the bytes). No driver or udev rule is
needed. Test OpenGL runs `demos/gears.c` (60 fps, render 1.2 ms, as
`gears_host`); Test video runs `demos/video.c` on the Big Buck Bunny trailer
(853x480 H.264, Blender Foundation, CC BY 3.0), decoded by the RPi's
VideoCore: 25 fps, 0 dropped; with its sound on an HDMI monitor that has
speakers (5.1 AAC, mixed down to stereo, at the `volume=` setting). Stop
ends the demo's session (`STREAM_END`, docs/protocol.md §13): the RPi resets
and its serial port carries the log and the installer again (firmware
without it restarts instead). `make-firmware.sh`
builds the demos when Emscripten is installed (`EMSDK`, default `~/emsdk`)
and downloads the trailer (Blender's server doesn't let a page fetch it).

## The RPi as a USB monitor for Linux

Over its USB port the RPi is also a monitor for a Linux PC, with no driver
to install: GUD, the kernel's Generic USB Display (`gud`).

The RPi is one USB device, `1d50:614d` (the ID GUD's driver binds to), with
three functions (`devtools/pgpugadget.h`):

- the serial port (interfaces 0 and 1: the log, the installer, a GL stream);
- the GL interface (2);
- the display (3). `gud=off` leaves it out.

While the PC has the display on, its desktop takes the RPi's screen (the
panel, or HDMI). A GL host's frames are rendered off screen until the PC turns
the display off (`gpu/display/gud_display`).

- The PC sees one connector with one mode, the RPi's screen size, and an
  EDID: GNOME calls it "PGU piegpu".
- `devtools/gudtest.c` drives it through DRM without a desktop: a test
  pattern and two rates. Measured at 320x240: 110 full frames a second
  (17 MB/s), 60 small updates a second.
- GNOME 50.1: it draws no pointer on this monitor while another one has a
  hardware cursor (newer mutter decides per monitor).
  `MUTTER_DEBUG_DISABLE_HW_CURSORS=1` for GNOME Shell gives software cursors
  everywhere, e.g. in a drop-in for its unit:
  `~/.config/systemd/user/org.gnome.Shell@ubuntu.service.d/*.conf` with
  `[Service]` and `Environment=MUTTER_DEBUG_DISABLE_HW_CURSORS=1`.
- Don't unplug or reboot the RPi while GNOME uses it as a monitor: GNOME
  Shell 50.1 crashed once when it came back within seconds (it keeps the old
  device, and the new one had the same `/dev/dri` name). For a desktop that
  must leave it alone, set `gud=off` on the RPi, or tag it in udev: `SUBSYSTEM=="drm", KERNEL=="card*",
  ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614d", TAG+="mutter-device-ignore"`.
