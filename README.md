# piegpu

A toy GPU for OpenGL rendering and media. A Raspberry Pi Zero runs bare
metal as the graphics card of a microcontroller (like a Raspberry Pi
Pico or an ESP32): the microcontroller sends OpenGL ES 2.0 commands
and the RPi renders them with its VideoCore IV V3D, to an ST7789 panel
or to HDMI. Video (H.264, e.g. from an MP4) is decoded by the VideoCore
into textures that any draw can use, and sound (AAC, e.g. the
MP4's, MP3 or Ogg Vorbis) is decoded by the RPi and played on HDMI, with the picture
following it.

The host can be any microcontroller board capable of the link: I2S as the
master (DATA, BCLK and FS out, the replies back on REPLY), a READY input and
optionally FRAME, at 3.3 V ([docs/protocol.md](docs/protocol.md) §2–3). The
host library (`libpgpu`: the protocol, pgl for the GL ES API, an MP4 reader,
a HUD, in C) and the demos are shared by every board; a board adds only a
transport, the few functions of `libpgpu/pgpu_link.h`. Tested on:

- a Raspberry Pi Pico 2 W (RP2350), over I2S;
- an ESP32-P4 (Waveshare ESP32-P4-Module-DEV-KIT), over I2S.

A Linux PC, or a page in Chrome, can be the host too, over the RPi's USB port.
A web page puts piegpu on the RPi's SD card (in the PC's card reader, or over
that port), and the RPi can serve as a monitor for a Linux desktop.

**Supported for now: the Raspberry Pi Zero, the Zero W and the Zero 2 W.** More
RPi boards are to come; "RPi" below means the board piegpu runs on, and a
model is named where only that one was verified.

**Install:** open the installer page, **[anight.github.io/piegpu](https://anight.github.io/piegpu/)**,
in Chrome or Edge, and follow the **[installation guide](docs/installation.md)**:
step by step on Linux, Windows and macOS, with what to do when something
doesn't work.

Documentation ([docs/](docs/README.md)):

- [docs/installation.md](docs/installation.md): installing piegpu on the RPi's
  SD card, and connecting to it;
- [docs/protocol.md](docs/protocol.md): the wire protocol;
- [docs/host-library.md](docs/host-library.md): the host library, libpgpu
  (pgl, the GL ES API);
- [docs/development.md](docs/development.md): how the RPi side works inside.

| Directory | What |
|---|---|
| `gpu/` | the RPi's firmware: links (`link/`), outputs (`display/`), renderer, video (`video/`), audio (`audio/`, with FAAD2, minimp3 and Tremor), SD card installer (`install/`) |
| `drivers/` | the RPi's V3D and ST7789 (DMA) drivers |
| `libpgpu/` | the host library: protocol encoding, pgl (the GL ES API), MP4, MP3 and Ogg readers, HUD, self tests |
| `transports/` | links for the host library: `pico-i2s`, `esp32p4-i2s`, `pc-usb` |
| `hosts/` | builds per host: `pico`, `esp32p4`, `pc`, `web` (a page, WebAssembly) |
| `demos/` | the demos, for every host |
| `protocol/` | the wire format header, shared by both sides |
| `devtools/` | Circle setup and builds per board, the RPi's USB device (serial port, GL interface, monitor), the run log, `run.sh` (boot an RPi over USB, logs, screenshots) |
| `tools/` | `glslc` (GLSL compiler: Mesa's vc4, offline), `deqp` (the conformance tests) |
| `patches/` | a local change to Mesa, for `tools/glslc` (Circle's are in its fork, below) |
| `third_party/` | submodules: Circle (piegpu's fork), Mesa, VK-GL-CTS, the Raspberry Pi userland |
| `web/installer/` | a page that installs piegpu on the RPi's SD card over USB and runs demos on it |
| `experiments/` | the early probes and demos that led here (links, displays, V3D, video) |

## Boards and building

piegpu supports two boards for now:

- a Raspberry Pi Zero or Zero W: 32 bit, `kernel.img`, 1.2 ms to render a
  gears frame;
- a Zero 2 W: 64 bit, `kernel8.img`, 0.9 ms.

piegpu builds with its fork of Circle, the submodule `third_party/circle`: the branch
`piegpu` of [github.com/anight/circle](https://github.com/anight/circle/tree/piegpu),
Circle Step51.1 with piegpu's changes as commits (the USB gadget's CDC and
EP0, FatFs' `f_mkfs`, `MEM_PERSISTENT_SIZE`, vcos in 64 bit, the VideoCore's
dry-spell flags in VCHIQ sound, and a CMake build). With CMake one Circle tree
serves every board: each board has a build directory (`build/zero`,
`build/zerow`, `build/zero2`) with its toolchain and Circle's options, and
the gpu app (`gpu/CMakeLists.txt`) builds on it. `build-gpu.sh` fetches the
submodule and Circle's boot files if they aren't there, configures a board's
directory the first time (and again when its options changed), and builds.

Toolchains: Arm GNU 15.2 in `~/toolchains` (`arm-none-eabi` and
`aarch64-none-elf`).

The firmware needs only the Circle submodule (`build-gpu.sh` fetches it too):

```bash
git submodule update --init third_party/circle
```

```bash
devtools/build-gpu.sh all
```

This gives `build/zero/kernel.img` (the Zero), `build/zerow/kernel.img` (the
Zero W) and `build/zero2/kernel8.img` (the Zero 2 W).
`web/installer/make-firmware.sh` puts the builds on the installer page, which
picks the one for the board.

The Zero's and the Zero W's builds differ by one option, `PGPU_WIRELESS`: the
Zero has no wireless chip, so its kernel is built without the Bluetooth code
(for speakers, to come: for now the Bluetooth switch in Settings); the Zero
W's and the Zero 2 W's are built with it. A build's line in the log says
which it is (`fwconfig=RASPPI=1,AArch32,wireless,...`). Either 32-bit kernel
starts on either board: a Zero W with the Zero's kernel has no Bluetooth
(Settings says so, and the installer page offers the board's own).

**Versions:** `major.minor.patch.build`. `VERSION` holds `major.minor.patch`
(set by hand); every firmware build takes the next build number of the
checkout (`devtools/next-build.sh`, counted in `build/build-number`, not in
git), one for all the boards of a `build-gpu.sh` or `make-firmware.sh` run.
The kernel carries it with the git commit and the build time (UTC; the boot
log, the splash, the installer page, which offers "Upgrade" when its kernel's
version is higher than the card's).

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
   │ (Pi Pico,  │ ◄─────────────────── │   (GPU)    │   └────────────────┘
   │  ESP32-P4) │                      │            │ mini-HDMI ┌─────────┐
   └────────────┘                      │            │──────────►│ monitor │
                                       └─────┬──────┘           └─────────┘
                                             │  USB port, one cable:
                                             │  power; its log; USB boot
                                             │  GL commands and media from a PC
                                             ▼
                                            PC
```

One host is enough, a microcontroller over I2S or a PC over USB; the RPi
also needs power on one of its micro-USB ports. Both can be connected at
once with `host=auto` (the default): USB has priority, the PC's GL commands
taking over from I2S once it opens its stream, until the RPi restarts.

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
| LED (backlight) | 32 (GPIO12, PWM0) |
| T_CLK | 23 (GPIO11, with SCK) |
| T_DIN | 19 (GPIO10, with SDI) |
| T_DO | 21 (GPIO9, with SDO) |
| T_CS | 26 (GPIO7, SPI0 CE1) |

At boot the RPi reads the panel's ID over SDO, so it knows whether a panel is
there. Without the SDO wire it can't tell: set `panel=yes` (see below).

- **No tearing:** the panel refreshes itself 60 times a second along its own
  240x320 order. The RPi turns each landscape frame into that order and
  starts sending it just after the panel's scan has passed the top (it reads
  the scan line over SDO), so every refresh shows one whole frame.
- **Backlight:** the LED pin on GPIO12 is dimmed by PWM (10 kHz, `brightness=`
  below). With the LED pin on 3.3 V instead the backlight is simply on.
- **Touch** (`gpu/display/touch`): the module's XPT2046 shares the panel's
  SPI with its own chip select, and is read right after each frame (T_IRQ
  isn't used). A touch is a pressure reading whose X and Y samples agree,
  twice running; `touchcal=` maps the readings to the panel's pixels. The
  controller's samples are noisy (2 pixels rms under a held finger, whatever
  its SPI clock), so each reading takes eight of X and of Y and keeps the
  middle four, and the position is smoothed over the readings and reported
  only once it has moved 1.25 pixels: a held finger gives a still position. The
  host gets it as a TOUCH reply (`pgpu_get_touch`; the `touch` demo is a
  drawing board with a calibration).

### Settings on the panel

A long press on the panel (2 s, anywhere, any time) opens **Settings** (LVGL,
`gpu/ui`):

- **Display:** the brightness; the touch's calibration and a test.
- **Audio:** the volume, mute (the volume greyed), a test sound (a note on the left, one on the
  right, one on both); Bluetooth on or off (for speakers, to come; greyed on
  a Zero, which has none).
- **Kernel:** the options of the kernel command line below, each a list to
  choose from; the clocks' lists are the rates the firmware has for the board,
  in MHz. They take effect at the next start: leaving the page with one
  changed writes `cmdline.txt` (the options it doesn't know stay) and asks
  whether to restart now. A choice that would take Settings itself away at
  the next start (the touch screen off, no panel, the screen always on HDMI)
  asks first.

While it's open the panel is its: a host on I2S is told to wait (READY low)
and goes on after; a PC over USB keeps running, its frames not shown. Done
closes it and writes what changed of the first two pages to `settings.txt`.

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
  - GL commands and media (video, sound) from a PC program or a page (below);
  - the USB monitor, with `gud=on` (below).

  Without a card that holds piegpu, many RPi boards can boot over USB from
  a PC with `rpiboot` (Raspberry Pi's usbboot lists them). Both supported
  boards do, from the installer page (below) or `rpiboot`:
  - `devtools/run.sh gpu` builds, boots and logs, with the Zero's 32-bit
    build (`build/zero`; `BOARD=zerow`: the Zero W's). Other apps (`experiments/`) keep their Makefiles,
    which need a Circle configured in place (`./configure`, `./makeall` in a
    copy of it).
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
| `gud=off` | the default: no monitor for a PC's desktop to take (the serial port and the GL interface stay) |
| `gud=on` | the RPi is also a USB monitor for a Linux PC (below) |
| `output=auto` | the default: HDMI while a monitor is connected, else the panel |
| `output=panel`, `output=hdmi` | always this output |
| `panel=auto` | the default: a panel if one answers on SDO (MISO) at boot |
| `panel=yes`, `panel=none` | a panel is there (SDO not wired) or none is; without a panel and a monitor the screen stays on HDMI |
| `hdmi_pixels=N` | cap the screen on HDMI to N pixels (default: the monitor's native resolution, up to 1920x1200) |
| `cpu=max`, `cpu=low` | the ARM at its maximum clock (the default: 1000 MHz on both boards, throttled by the firmware at its own temperature limit) or at its lowest (the Zero: 700 MHz, the Zero 2 W: 600 MHz). The rates between aren't offered: asked for 800 or 850 MHz, a Zero's firmware gave 900 |
| `v3d=N` | the V3D's clock, MHz, within the firmware's range (the Zero: 250-300; the default: its maximum). It holds with `cpu=low` only: with the ARM at its maximum the firmware keeps the V3D at its maximum too (measured on a Zero) |
| `volume=N` | the sound's volume on HDMI, percent (default: 10) |
| `mute=on`, `mute=off` | the sound muted, whatever the volume (default: off) |
| `bluetooth=on`, `bluetooth=off` | Bluetooth (for speakers, to come; kept by Settings, not used yet) |
| `brightness=N` | the panel's backlight, percent (default: 100; its LED pin on GPIO12) |
| `touch=auto`, `touch=off` | the panel's touch screen: used if its controller answers (the default), or not |
| `touchcal=x0,x1,y0,y1,swap` | the touch's calibration: the readings at the panel's left and right edges, top and bottom; swap 1: its x from the controller's Y (Settings finds it) |
| `clcheck=on`, `clcheck=off` | check each V3D job's control lists before it runs, and refuse a bad one (the default: on; [docs/development.md](docs/development.md#the-control-list-checker)) |

With `devtools/run.sh`, pass these as `CMDLINE="output=panel" devtools/run.sh gpu`.

**The user's settings** (`volume=`, `mute=`, `brightness=`, `touchcal=`,
`bluetooth=`) belong in `settings.txt` next to `cmdline.txt`: a `key=value` a
line, `#` for comments. The RPi reads it at boot and a key there wins over
`cmdline.txt`'s. The installer page writes `cmdline.txt` (keeping the options
it has no control for) and never `settings.txt`, so an upgrade keeps them;
Settings on the panel writes both: `settings.txt` from its Display and Audio
pages, `cmdline.txt` from its Kernel page.

## Host boards (`hosts/pico`, `hosts/esp32p4`)

The two tested boards. Both builds make the self tests and the demos
(`demos/demos.cmake`), plus the Jet demos (`demos/jet`). Another board needs
its own transport (`libpgpu/pgpu_link.h`; `transports/pico-i2s` and
`transports/esp32p4-i2s` are the examples) and a build like these.

**Raspberry Pi Pico 2 W:** the Pico SDK (2.2.0 here, `PICO_SDK_PATH`). The
build makes one `.uf2` per program in `hosts/pico/build`:

- `gpulink.uf2`: the self tests;
- `gears.uf2`, `antigrav.uf2`, `toy-pong.uf2`, `jet-viewer.uf2` and so on: the demos.

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
- a demo: `gears`, `antigrav`, `toy-NAME`, `jet-NAME` and so on;
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
  fps on the panel, render 1.2 ms a frame). `media_host` plays the file
  the `PGPU_MEDIA` environment variable names, an MP4, an Ogg Vorbis file or an MP3 (else its
  linked-in test video).
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

Compiling GLSL takes the host Mesa, built once with
`tools/glslc/build-mesa.sh` (below).

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

### Third-party code (`third_party/`)

Submodules, each pinned at a commit (shallow clones: git fetches the
pinned commit alone). A build script fetches the one it needs if it isn't
there, or all at once:

```bash
git submodule update --init
```

| Submodule | Pinned at | Used by |
|---|---|---|
| `third_party/circle` | piegpu's fork, branch `piegpu` | the firmware (`devtools/build-gpu.sh`) |
| `third_party/mesa` | `mesa-26.2.3` | `tools/glslc`, built by `tools/glslc/build-mesa.sh` (into `third_party/mesa-install`) |
| `third_party/VK-GL-CTS` | `1d3e817` | dEQP-GLES2, built by `tools/deqp/build-deqp.sh` (into `third_party/deqp-build`) |
| `third_party/userland` | `a54a0db` | `experiments/videodec` (`gpu/video/userland` is a copy of its MMAL client) |

`build-mesa.sh` applies `patches/mesa-vc4-dump.patch` to the Mesa submodule
(the dump hook glslc reads the QPU code from, pgl's limits, and glslc's
modes); git status doesn't show those changes (`ignore = dirty` in
`.gitmodules`). `build-deqp.sh` fetches VK-GL-CTS' own external sources
(glslang, SPIRV-Tools, ...: `external/fetch_sources.py`, 1.2 GB) and links
its `pgl` target in (`ignore = untracked`).

```bash
tools/glslc/build-mesa.sh
```

```bash
tools/deqp/build-deqp.sh
```

## Installing on an SD card (`web/installer`)

**Step by step, on Linux, Windows or macOS, with what to do when something
doesn't work: [docs/installation.md](docs/installation.md).**

A static page puts piegpu on the RPi's microSD card, with its settings, and
then manages it over the RPi's USB port. The settings (section 3 of the
page, folded until its Show button opens it) are the command line above
(`cmdline.txt`) and `config.txt`:

- where the GL commands come from;
- the screen and the panel;
- the HDMI mode;
- the USB monitor (off unless chosen).

The page needs Chrome or Edge on a desktop (WebUSB, Web Serial, the File
System Access API) and a secure origin (https, or `localhost`). It's
published at **https://anight.github.io/piegpu/**: `devtools/publish-installer.sh`
builds it (`make-firmware.sh`: both boards' firmware, the demos, the test
media) and pushes it to the `gh-pages` branch, one commit that replaces the
last (GitHub Pages serves that branch). From a checkout:

```bash
web/installer/make-firmware.sh
```

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory web/installer
```

How its parts work:

- **Prepare a card** (a card in the computer's card reader; `card.js`): the
  chosen board's firmware and kernel, and the `config.txt` and `cmdline.txt`
  of the settings, written into the folder the user picks (the File System
  Access API: the system's own mount of the card, so no permissions), each
  read back and checked by its CRC; a folder that holds other things makes
  the page ask first. Or the same files in a zip (stored, no compression) to
  unpack by hand. The card's side is verified: a card written on a PC (one
  FAT32 partition, type 0x0c, those files) started a Zero 2 W. The page's
  writing is tested into a browser-private folder (read back) and the zip
  with `unzip -t`; not yet onto a real card through the folder picker.
- **Start a blank RPi** (no card reader; `rpiboot.js`): the RPi's boot ROM,
  finding nothing to start, waits for USB, and the page boots piegpu from
  it (WebUSB, the rpiboot protocol; verified on both boards). The page
  serves both builds, and a `config.txt` whose `[pi02]` lines make a Zero 2
  W ask for its 64-bit `kernel8.img` (a Zero asks for `kernel.img`). Once
  piegpu runs, it says which board it is, and Install writes that board's
  files only; a card without a FAT file system can be formatted there (one
  FAT32 partition). The browser needs access to the boot ROM's USB device
  (`0a5c:2763`, `2764`): on Linux a udev rule (Ubuntu's `rpiboot` package
  has one), on Windows the WinUSB driver (not tried). Twice, early on, a Zero
  2 W's boot ROM took `bootcode.bin` and then stopped answering, with
  `rpiboot` too; the cause wasn't found. It then answers nothing until it
  loses power. Every start since has worked, from the page and from
  `rpiboot`, with a blank card in the board or none.
- **Connect** (Web Serial): the RPi's USB serial port. On Linux the user
  needs access to it (the `dialout` group, or a uaccess rule such as the one
  `esptool`'s package installs; when opening fails, the page says so). Install
  again, or change the settings only. When the page's kernel has a higher
  version than the card's, "Upgrade" next to the card's firmware writes the
  kernel alone and restarts the RPi. After every restart the page reads the
  board, the card and its settings again. If piegpu doesn't answer (a
  program left it taking GL commands), the page restarts it; "Reset"
  restarts it any time.
- **A card with another system:** for Prepare a card, format it first; to
  start the RPi from the page, take it out, and put it in when the page asks.

piegpu writes the files itself (`gpu/install`): each is checked by its CRC,
then renamed into place. At the end the RPi restarts from the card, and the
page shows where the screen went. Measured: 3.5 MB in 5.4 s; a 64 GB card
formatted in 7.3 s.

**Test OpenGL, Test Video, Test Audio.** The page runs demos in the browser, with
libpgpu compiled to WebAssembly (`hosts/web`, Emscripten), their GL commands
going over the same Web Serial port as the installer's (the PC's serial
transport; `web/installer/gl.js` moves the bytes). No driver or udev rule is
needed. Test OpenGL runs `demos/gears.c` (60 fps, render 1.2 ms, as
`gears_host`); Test Video runs `demos/media.c` on the Big Buck Bunny trailer
(853x480 H.264, Blender Foundation, CC BY 3.0), decoded by the RPi's
VideoCore: 25 fps, 0 dropped; with its sound on an HDMI monitor that has
speakers (5.1 AAC, mixed down to stereo, at the `volume=` setting). Test
Audio runs it on "Monkeys Spinning Monkeys" by Kevin MacLeod
(incompetech.com, CC BY 4.0), as the format beside the button says: an MP3
(44.1 kHz stereo, 320 kbps, 2:05; decoded by minimp3) or Ogg Vorbis (from
Wikimedia Commons: 44.1 kHz stereo, 128 kbps; decoded by Tremor), decoded by
the RPi, with the title, artist and time on the screen. Verified on
the Zero 2 W with the Dell S2421H: 0 broken frames, 0 ms of silence; the
webcam's microphone heard the track in order and at its speed (3 s windows
over 21 s, each found on its own in the track's first 40 s: 7 of 8 at the
same place within 5 ms, the eighth under a noise in the room); the Ogg,
through the page's WebAssembly demo run by Node on the RPi's serial port
(the page's own code, not in Chrome): 0 broken pages, 0 ms of silence. Stop
ends the demo's session (`STREAM_END`, docs/protocol.md §13): the RPi resets
and its serial port carries the log and the installer again (firmware
without it restarts instead). `make-firmware.sh`
builds the demos when Emscripten is installed (`EMSDK`, default `~/emsdk`)
and downloads the trailer, the MP3 and the Ogg into `web/installer/media/`
(their servers don't let a page fetch them).

## The RPi as a USB monitor for Linux

With `gud=on` (off by default) the RPi is also a monitor for a Linux PC over
its USB port, with no driver to install: GUD, the kernel's Generic USB
Display (`gud`).

The RPi is one USB device, `1d50:614d` (the ID GUD's driver binds to), with
three functions (`devtools/pgpugadget.h`):

- the serial port (interfaces 0 and 1: the log, the installer, a GL stream);
- the GL interface (2);
- the display (3), with `gud=on` only (the default, `gud=off`, leaves it out).

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
  must leave it alone, keep `gud=off` on the RPi (the default), or tag it in udev: `SUBSYSTEM=="drm", KERNEL=="card*",
  ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614d", TAG+="mutter-device-ignore"`.

## Licence and third-party software

piegpu's own code is under the BSD 2-Clause licence ([LICENSE](LICENSE)).
The RPi firmware built from it (`kernel.img`, `kernel8.img`) links Circle
(GPL-3.0) and FAAD2 (GPL-2.0-or-later), so those binaries are distributed
under the GPL, version 3, with this repository and its submodules as their
source.

| Software | Where | Licence | Used for | In what's distributed |
|---|---|---|---|---|
| [Circle](https://github.com/rsta2/circle) Step51.1 (piegpu's [fork](https://github.com/anight/circle/tree/piegpu)) | `third_party/circle` | GPL-3.0; its FatFs add-on ChaN's BSD-style licence, its VCHIQ add-on BSD-3-Clause or GPL-2.0 | the RPi firmware's base: drivers, USB gadget, VCHIQ, FatFs | the firmware |
| Raspberry Pi firmware (`bootcode.bin`, `start.elf`, `fixup.dat`) | fetched into `third_party/circle/boot` | Broadcom's licence: binary redistribution ([LICENCE.broadcom](third_party/circle/boot/LICENCE.broadcom) there) | starting the RPi; the VideoCore's H.264 decoder, ISP and audio service | the card, the installer page |
| [FAAD2](https://github.com/knik0/faad2) 2.11.3 | `gpu/audio/faad2` | GPL-2.0-or-later | AAC decoding | the firmware |
| [minimp3](https://github.com/lieff/minimp3) | `gpu/audio/minimp3` | CC0-1.0 | MP3 decoding | the firmware |
| [Tremor](https://gitlab.xiph.org/xiph/tremor) (libvorbisidec) and [libogg](https://gitlab.xiph.org/xiph/ogg) | `gpu/audio/tremor` | BSD-3-Clause | Ogg Vorbis decoding | the firmware |
| [LVGL](https://github.com/lvgl/lvgl) 9.4 | `third_party/circle/addon/lvgl/lvgl` (Circle's submodule, fetched by `devtools/build-gpu.sh`) | MIT | the Settings app on the panel (`gpu/ui`) | the firmware |
| [Raspberry Pi userland](https://github.com/raspberrypi/userland) (its MMAL client) | `gpu/video/userland` (a copy), `third_party/userland` | BSD-3-Clause | talking to the VideoCore's video components | the firmware |
| GCC runtime (`libgcc`), newlib's `libm` (Arm GNU Toolchain 15.2) | the toolchain | GPL-3.0 with the GCC Runtime Library Exception; newlib: BSD-style | runtime support linked into the firmware | the firmware |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | fetched by `hosts/pico` | BSD-3-Clause | the Pico host | Pico images |
| [ESP-IDF](https://github.com/espressif/esp-idf) 5.5 | installed separately | Apache-2.0 | the ESP32-P4 host | P4 images |
| [libusb](https://libusb.info) 1.0 | the system's | LGPL-2.1-or-later | the PC host's USB (linked dynamically) | — |
| [Emscripten](https://emscripten.org) | installed separately | MIT or University of Illinois/NCSA | the installer page's demos (WebAssembly; its runtime code is in the output) | the installer page |
| [Jet](https://github.com/CubeCoders/Jet) | `demos/jet/Jet` | MIT | the Jet demos' renderer | Jet demo images |
| JetExamples scenes (CubeCoders) | `demos/jet/examples` | MIT | the Jet demos | Jet demo images |
| picojet (its model viewer, runtime pieces, asset converter) | `demos/jet`, `demos/assets` | BSD-2-Clause | the Jet demos, the demos' models | demo images |
| Meshes and textures: radio, column, biplane | `demos/assets` | CC0 (opengameart.org) | flight, the Jet viewer | demo images |
| Meshes and textures: cube, f117, f22, efa, sphere, crab, the pikuma texture | `demos/assets` | not stated: shipped with Gustavo Pezzi's pikuma.com 3D graphics course, kept under the terms they arrived with | the Jet viewer, flight | demo images |
| [Mesa](https://mesa3d.org) 26.2.3 (patched: `patches/mesa-vc4-dump.patch`) | `third_party/mesa` | MIT (mostly; per file) | `tools/glslc`: GLSL to QPU code, on the PC | — (its output: the demos' shader binaries) |
| [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) (dEQP) | `third_party/VK-GL-CTS` | Apache-2.0 | conformance tests (`tools/deqp`) | — |
| [usbboot](https://github.com/raspberrypi/usbboot) (`rpiboot`) | installed separately | Apache-2.0 | `devtools/run.sh`; the protocol `web/installer/rpiboot.js` speaks | — |
| Arm GNU Toolchain 15.2 | installed separately | GPL-3.0 (GCC, binutils) | building the firmware | — |
| *Big Buck Bunny* trailer (Blender Foundation) | downloaded by `make-firmware.sh` | CC BY 3.0 | Test Video | the installer page |
| "Monkeys Spinning Monkeys" (Kevin MacLeod, incompetech.com) | downloaded by `make-firmware.sh` | CC BY 4.0 | Test Audio | the installer page |
