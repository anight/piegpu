# piegpu

A toy GPU for OpenGL rendering and media. A Raspberry Pi Zero runs bare
metal as the graphics card of a microcontroller (like a Raspberry Pi
Pico or an ESP32): the microcontroller sends OpenGL ES 2.0 commands
and the RPi renders them with its VideoCore IV V3D, to an ST7789 panel
(with its touch screen) or to HDMI. Video (H.264, e.g. from an MP4) is
decoded by the VideoCore into textures that any draw can use. Sound (AAC,
e.g. the MP4's, MP3 or Ogg Vorbis) is decoded by the RPi and played on HDMI
or on a Bluetooth speaker, with the picture following it; short sound
effects the host sends once are mixed in by the RPi, on channels the host
plays them on. Edges can be antialiased in hardware: 4x MSAA (the V3D's
multisampling, four samples a pixel). And
the RPi can record what it shows and plays as an MP4 on its card: H.264 by
the VideoCore's encoder, AAC by an encoder of its own.

The host can be any microcontroller board capable of the link: I2S as the
master (DATA, BCLK and FS out, the replies back on REPLY), a READY input and
optionally FRAME, at 3.3 V ([docs/protocol.md](docs/protocol.md) §2–3). The
host library (`libpgpu`: the protocol, pgl for the GL ES API, readers for
MP4, MP3 and Ogg files, a HUD, in C) and the demos are shared by every board; a board adds only a
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
| `gpu/` | the RPi's firmware: links (`link/`), outputs, touch, backlight and the HDMI mode (`display/`), renderer, video and the recorder (`video/`), audio (`audio/`: the decoders FAAD2, minimp3 and Tremor, an AAC encoder, the outputs, the sound effects' mixer), Bluetooth for a speaker (`bt/`), Settings on the panel (`ui/`), SD card installer (`install/`) |
| `drivers/` | the RPi's V3D and ST7789 (DMA) drivers |
| `libpgpu/` | the host library: protocol encoding, pgl (the GL ES API), MP4, MP3 and Ogg readers, HUD, self tests |
| `transports/` | links for the host library: `pico-i2s`, `esp32p4-i2s`, `pc-usb` |
| `hosts/` | builds per host: `pico` (with the Pico W's own programs: Wi-Fi setup, a Bluetooth keyboard and mouse; its stick and game controller), `esp32p4`, `pc`, `web` (a page, WebAssembly) |
| `demos/` | the demos, for every host |
| `engine/` | a small 3D engine for three of the demos: Quake-format levels (BSP), movement, lifts and doors, sounds; its levels and the tools that make them and the sounds |
| `protocol/` | the wire format header, shared by both sides |
| `devtools/` | Circle setup and builds per board, the RPi's USB device (serial port, GL interface, monitor), the run log, `run.sh` (boot an RPi over USB, logs, screenshots), `record.py` (a recording of the screen and the sound) |
| `tools/` | `glslc` (GLSL compiler: Mesa's vc4, offline), `deqp` (the conformance tests), `aacenc` (the AAC encoder's tables, and its test on a PC) |
| `patches/` | a local change to Mesa, for `tools/glslc` (Circle's are in its fork, below) |
| `third_party/` | submodules: Circle (piegpu's fork; LVGL as its submodule), Mesa, VK-GL-CTS, the Raspberry Pi userland |
| `web/installer/` | a page that installs piegpu on the RPi's SD card over USB and runs demos on it |
| `experiments/` | the early probes and demos that led here (links, displays, V3D, video) |

## Boards and building

piegpu supports three boards for now, each with its build:

- a Raspberry Pi Zero: 32 bit, `kernel.img`, 1.2 ms to render a gears frame;
- a Zero W: the same, and Bluetooth (its own `kernel.img`, below);
- a Zero 2 W: 64 bit, `kernel8.img`, 0.9 ms; Bluetooth; a second core for
  the audio decoder.

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
(`gpu/bt`: a speaker for the sound, below); the Zero
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

Video decoding works the same in 32 and in 64 bit
([docs/development.md](docs/development.md#video)). Verified on a Zero and a
Zero 2 W: 640x360 to 1280x720, 0 dropped.

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
the host's reboot, the installer's, Settings', Reset. Verified on the Zero
2 W (all four); on the Zero and the Zero W only the restart as asked has been
seen. How it works:
[docs/development.md](docs/development.md#the-run-log-a-restart-explains-itself).

## Wiring

```
                  I2S link + READY/FRAME            SPI0
   ┌────────────┐  (6 signals + GND)   ┌────────────┐   ┌────────────────┐
   │  host MCU  │ ───────────────────► │    RPi     │──►│ ST7789 320x240 │
   │ (Pi Pico,  │ ◄─────────────────── │   (GPU)    │   │  + touch       │
   │  ESP32-P4) │                      │            │   └────────────────┘
   └────────────┘                      │            │ mini-HDMI ┌─────────┐
                                       │            │──────────►│ monitor │
                                       │            │ Bluetooth ┌─────────┐
                                       │            │ ·  ·  ·  ►│ speaker │
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
taking over from I2S once it opens its stream, until its session ends (the
program's exit: `STREAM_END`). A session starts with a reset of the RPi's GL
state, so the microcontroller's program has to start over afterwards: the RPi
doesn't tell it yet.

Pin numbers below are **physical header pins** with the GPIO number in
brackets. All signals are 3.3 V. Connect the grounds of both boards. The
three RPi boards have the same 40-pin header, so the same wiring. Verified:
gears from the ESP32-P4 on a Zero and a Zero 2 W; the engine's demos from a
Pico 2 W on all three, on the panel.

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
  The stick and the game controller, if there, are on picosdl's pins:
  GP26, GP27 and GP22 (31, 32, 29), GP6 and GP7 (9, 10).

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
  host gets it as a TOUCH reply (`pgpu_get_touch` for how it is now,
  `pgpu_poll_touch` for what happened in order: put down, moved, let go;
  the `touch` demo is a drawing board with a calibration).

### Settings on the panel

A long press on the panel (2 s, anywhere, any time) opens **Settings** (LVGL,
`gpu/ui`):

- **Display:** the brightness; the touch's calibration and a test.
- **Audio:** the volume, mute (the volume greyed), a test sound (a note on
  the left, one on the right, one on both); Bluetooth on or off (greyed on a
  Zero, which has none). On: the speakers nearby are listed under the switch
  (a new one has to be in its pairing mode); a tap on one connects to it,
  pairing with it if it's new, and the sound goes there: the test sound, and
  a host's MP3, Ogg or AAC. A tap on the connected one lets it go; a long
  press on a paired one asks whether to forget it.
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
  - The HDMI mode follows the monitor: the firmware sets one at boot only,
    from its own lists (640x480 without a monitor then; 1024x768 for a
    1024x600 monitor), and scales the screen to it, which made text
    unreadable. So when a monitor's EDID has come, at boot or after it's
    plugged in, the RPi asks the firmware for the monitor's own preferred
    timing (`gpu/display/tv_service`; 0.35 s; `hdmi_signal=boot` leaves the
    firmware's). Verified on a Zero 2 W with a 1024x600 monitor by what the
    RPi sends (the mode read back is the EDID's); a real plug-in after a
    boot without a monitor, sound over HDMI after the change and other
    monitors not yet.
- **PC:** the RPi's "USB" port (not "PWR IN"). The one cable carries:
  - power;
  - the RPi's log;
  - the installer (below);
  - GL commands and media (video, sound) from a PC program or a page (below);
  - the USB monitor, with `gud=on` (below).

  Without a card that holds piegpu, many RPi boards can boot over USB from
  a PC with `rpiboot` (Raspberry Pi's usbboot lists them). The supported
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
| `host=auto` | the default: GL commands from a PC over USB from when it opens its stream till its session ends, else from I2S |
| `host=usb`, `host=i2s` | only a PC over USB, or only a Pico / ESP32-P4 over I2S (the log, the installer and the USB monitor work either way) |
| `gud=off` | the default: no monitor for a PC's desktop to take (the serial port and the GL interface stay) |
| `gud=on` | the RPi is also a USB monitor for a Linux PC (below) |
| `output=auto` | the default: HDMI while a monitor is connected, else the panel |
| `output=panel`, `output=hdmi` | always this output |
| `panel=auto` | the default: a panel if one answers on SDO (MISO) at boot |
| `panel=yes`, `panel=none` | a panel is there (SDO not wired) or none is; without a panel and a monitor the screen stays on HDMI |
| `hdmi_pixels=N` | cap the screen on HDMI to N pixels (default: the monitor's native resolution, up to 1920x1200) |
| `hdmi_signal=boot` | keep the HDMI mode the firmware chose at boot (`config.txt`). Default (`monitor`): the RPi asks the firmware for the monitor's own preferred mode, at boot and when a monitor is plugged in, so the picture isn't scaled twice |
| `cpu=max`, `cpu=low` | the ARM at its maximum clock (the default: 1000 MHz on all three boards, throttled by the firmware at its own temperature limit) or at its lowest (the Zero: 700 MHz, the Zero 2 W: 600 MHz). The rates between aren't offered: asked for 800 or 850 MHz, a Zero's firmware gave 900 |
| `v3d=N` | the V3D's clock, MHz, within the firmware's range (the Zero and the Zero W: 250-300; the default: its maximum). It holds with `cpu=low` only: with the ARM at its maximum the firmware keeps the V3D at its maximum too (measured on both). The Zero 2 W's is 400 and stays there: its `config.txt` pins it (`v3d_freq`, `v3d_freq_min`) |
| `volume=N` | the sound's volume, percent (default: 10) |
| `mute=on`, `mute=off` | the sound muted, whatever the volume (default: off) |
| `bluetooth=on`, `bluetooth=off` | Bluetooth, for a speaker (default: off; the Zero W's and the Zero 2 W's kernels) |
| `btlog=on` | log every Bluetooth packet (its first bytes): debugging |
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
pages, `cmdline.txt` from its Kernel page. The Bluetooth speakers paired with
are in a third file, `speakers.txt` (below).

### Sound: a stream, effects, where it goes

- **The audio stream** ([docs/protocol.md](docs/protocol.md) §7.13): AAC, MP3
  or Ogg Vorbis from the host, decoded by the RPi (on a Zero 2 W on its
  second core); a video stream can follow its clock.
- **Sound effects** (§7.14): the host sends short mono sounds once and plays
  them on 16 channels, each with its volume left and right and its pitch
  (how fast it plays: an engine's note); the RPi mixes them into what it
  plays, the stream's sound or silence, so an effect is heard as soon as the
  output allows (HDMI: the 85 ms the VideoCore holds; a Bluetooth speaker:
  its own buffer, not measured). The engine's demos, `antigrav`, `tumble`
  and `aquarium` use them (below).
- **The output:** a Bluetooth speaker while one is connected, else HDMI (a
  monitor with speakers). The volume and mute (Settings, `volume=`, `mute=`)
  apply to all of it.

### Sound on a Bluetooth speaker

The Zero W's and the Zero 2 W's kernels (`PGPU_WIRELESS`) have piegpu's own
small Bluetooth stack (`gpu/bt`, BSD like the rest: just what a speaker
takes): the board's controller on its inner UART (GPIO30-33, nothing on the
header), devices found and named, pairing (Secure Simple Pairing without a
PIN; an old device's PIN 0000), L2CAP, the service records a speaker may ask
for, AVDTP, AVRCP for the volume, and A2DP's SBC (Bluedroid's encoder, `gpu/bt/sbc_encoder`,
Apache 2.0). The controller runs as it comes, without Broadcom's patch file.

- **The speakers paired with** are remembered in `speakers.txt` on the card
  (address, key, name; the last used first; up to 8). While none is connected
  they are called in turn every 10 s, and a call from one of them is taken:
  a speaker switched on is back by itself. Settings pairs and forgets.
- **Where the sound goes:** to the speaker while its stream is there, else
  to HDMI; decided when a host's stream (or the test sound) starts, and for
  sound effects as the speaker comes and goes. The stream is SBC at the
  sound's rate when the speaker changes to it (44.1 or 48 kHz, joint stereo,
  bitpool up to 53), else the sound is resampled; it's started when there's
  sound and suspended after 5 s without (not while a host has sound effects:
  they may come any moment).
- **The volume is one:** a speaker that takes its volume from the source
  (AVRCP's absolute volume) gets the sound as it is and Settings' volume as
  its own: when it connects, and whenever the bar (or a host) changes it. Its
  own buttons move the bar, and `settings.txt` a moment later. A speaker that
  only sends its buttons' presses moves the volume here by 6% a press; one
  that does neither keeps its volume to itself, and ours is applied to the
  sound, as on HDMI. Verified with a JBL GO on a Zero 2 W: the volume taken
  at the connection and when it's changed here, a button's press heard here.
- **Off and on again** (the switch in Settings) lets the speaker go and
  calls it back: measured with a JBL GO, 3 to 20 s till its stream is open
  again (the speaker's own time to answer).
- **Verified** on a Zero W: a JBL GO found, paired with and streamed to
  (835 packets, none dropped); this PC as the speaker (BlueZ, PipeWire), what
  it received recorded: the test sound's three notes on the right channels at
  the right level, an MP3 from a host playing 18 s through. And on a Zero
  2 W (the decoder on its second core): the same recording; the JBL GO
  paired with from the panel, back by itself after a restart (its call
  taken, the stream open 0.8 s later), the test sound sent to it (465
  packets, none dropped); a video with its sound from a PC (70 s: 59.8 fps,
  5361 packets, none dropped); a demo's sound effects from a Pico at 60 fps.

### Antialiasing

- **Hardware antialiasing is 4x MSAA** (multisample antialiasing, the V3D's
  own): `pglSamples (4)` (the `MULTISAMPLE` capability,
  [docs/protocol.md](docs/protocol.md) §10.1) has the RPi render with four
  samples a pixel (coverage and depth per sample, the fragment shader run
  once a pixel) and average them as the picture is stored, on
  the screen or into a texture; `pglSamples (1)` is without again. The
  V3D's tiles are 32 pixels then instead of 64, so a frame costs more. A
  program that blends is compiled with `glslc --ms` to blend each sample
  with its own colour; without it the edges under what it draws go flat
  (harmless for a HUD's letters).
- **Supersampling** takes nothing of the RPi: the scene drawn twice as wide
  and high into a texture, and that drawn over the screen through a linear
  filter.
- `antigrav` is drawn with 4x MSAA (`m` on the console
  takes it off and puts it back). `aquarium` shows both and neither, ten
  seconds each. Measured there on a
  Zero 2 W, the panel, 60 fps in all three: a frame's rendering 3.5 ms
  without, 5.8 ms with 4x MSAA, 9.0 ms supersampled; the edges compared in
  screenshots. Not tried: the Zero and the Zero W, a multisampled frame
  drawn in several jobs, multisampling into a texture.

### Recording the screen and the sound

The RPi can record what it shows and plays as an MP4. For now it's a
debugging tool, started from a PC over the RPi's USB serial port (the `ENC`
line of the text console): no demo and nothing in the protocol starts it.

- **The picture:** the frames shown go to the VideoCore's H.264 encoder
  (`gpu/video/encode_test`: high profile, a key frame a second), which takes
  the panel's RGB565 as it is.
- **The sound:** what the output takes, the stream and the effects, before
  the volume, through piegpu's own AAC encoder (`gpu/audio/aac_enc.c`: AAC
  LC, stereo at 44.1 or 48 kHz; small: long blocks only, no psychoacoustic
  model, the noise kept a fixed distance under each band's level).
- **The file:** the RPi writes it to its card as it records
  (`gpu/video/mp4_writer`: `RECnnn.MP4`, the frames with the times they
  were shown, the index at the end), and it is fetched over USB:

  ```bash
  devtools/record.py 60 out.mp4 --card
  ```

  Without `--card` the stream and the sound come back over USB at the end
  and ffmpeg makes the MP4 on the PC.
- **Measured** on a Zero 2 W, the `aquarium` on the panel (320x240, 60 fps),
  1000 kbit/s of video: a frame comes out of the encoder 3 ms after it went
  in, 0.17 ms to hand it over; the sound's encoding 0.6 ms for 21 ms of
  sound, 174 kbit/s, its noise 26 dB under the sound. A minute onto the
  card: 8.9 MB, 59.6 fps, the picture and the sound within a frame of each
  other; ffmpeg decodes it whole and Chromium plays it. The card's writes
  stop the main loop (4 ms for 16 KB), so they are done in each frame's
  wait for the panel; where rendering leaves little of that (9 ms a frame),
  seconds of 55 to 57 fps.
- **Not yet:** the sound judged by ear (a sharp click's noise spreads over
  its block, 15 dB under it); the Zero and the Zero W; a card that fills up.
  A recording that doesn't end (the power gone) has no index and can't be
  played. How it works: [docs/development.md](docs/development.md#video)
  (the recorder) and [its Audio part](docs/development.md#audio) (the AAC
  encoder).

## The demos (`demos/`)

Every host builds them (`demos/demos.cmake`):

| Demo | What |
|---|---|
| `gears`, `breakout`, `flight` | the classic gears; a 3D Breakout that plays itself; a biplane over a cloud deck |
| `aquarium` | a tank of tropical fish: five kinds that swim by a wave down their bodies, school, come for food and flee a knock on the glass; a starfish creeping over a rock, two shrimps walking the sand; caustics on the sand, shafts of light, swaying plants, bubbles, the pump's hum. It shows antialiasing, ten seconds each: none, 2x2 supersampled (drawn twice the size into a texture), hardware 4x MSAA (`pglSamples`) |
| `tumble` | 2D physics: balls and boxes in a box that the stick tilts (rigid bodies, friction, stacking), each meeting heard; a finger on the panel adds a ball |
| `antigrav` | anti-gravity racing, after WipEout: six craft, a circuit; heard from the craft followed (engines whose note is their speed, the wind, the crowd, pads, the countdown); antialiased in hardware (4x MSAA) |
| `walk` | the BSP engine (`engine/`): a Quake-format level walked through, doors that open, a lift |
| `keep` | the engine outdoors: a castle at dusk, a moat of lava, a lift, nine gems to find |
| `isles` | the engine in the sky: floating islands, a moving platform, a jump pad, a portal, coins |
| `touch` | the panel's touch screen: a drawing board with a calibration |
| `toy-NAME` | Shadertoy-style shaders (`tunnel`, `spheres`, `clouds`, `voronoi`) and games in a shader (`pong`, `snake`, `asteroids`) |
| `media` | an MP4, an MP3 or an Ogg Vorbis file played by the RPi: the video into a texture, the sound with it |
| `jet-NAME` | the Jet scenes (`demos/jet`): CubeCoders' sixteen JetExamples, from a cube to a two-minute film, and a model viewer, written again for OpenGL ES and antialiased (4x MSAA) |
| `selftest`, `linktest` | the link, the protocol and pgl tested; the link at full speed both ways |
| `wifi` | the Pico 2 W only (`hosts/pico/wifi`): Wi-Fi asked of a phone (below) |
| `keyboard`, `mouse` | the Pico 2 W only (`hosts/pico/keyboard`, `mouse`): a Bluetooth keyboard, or a mouse, found, paired with and used (below) |

`walk`, `keep` and `isles` are played with the console's keys (w s a d, the
arrows, q e, space) and, on the Pico, with an analog stick and an Adafruit
Gamepad QT where they're wired (`hosts/pico/input`: picosdl's drivers and
pins, GP26/27 and GP22, I2C1 on GP6/7; each looked for at the start): the
stick walks and turns, the controller's Y and A step sideways, B, X or the
stick pressed jump. Without any of it for 10 s an autopilot walks the level.
They have sound effects, placed in Quake's manner (`engine/sound.c`:
quieter with the distance, louder on the side they're on): steps, the jump,
landings, lifts and doors with their motors, jump pads and teleporters,
pickups, the wind, lava. The sounds are made from nothing by
`engine/tools/make_sounds.py` (no samples from anywhere: 250 KB in the host's
flash); the levels by `engine/tools/make_*.py` and ericw-tools' compilers.

`wifi` gives the Pico W its Wi-Fi network without a keyboard. With none kept
it's an access point with a name and a password made up at the start, shown
on the screen as a QR code ("SCAN TO SETUP WIFI": a phone's camera offers to
join it). A phone on it gets an address and every name answered with the
Pico's (`net_servers.c`), and any page it asks for sent on to the Pico's
(`portal.c`), which is how phones find a network's sign-in page: they offer
it by themselves; the screen shows its address (192.168.4.1) as a second QR
code for one that doesn't. The page asks for the network's name (the ones
found around are offered) and its password; the Pico leaves the access
point and joins it. Joined, the network is kept in the flash (as it is:
readable there) and joined at every start; not joined, the access point is
back and both the screen and the page say why. The stick's button held at
the start, or `s` on the console, sets it up again. The QR codes are Project
Nayuki's generator (MIT, `qrcodegen.c`, as LVGL carries it).

`keyboard` connects a Bluetooth keyboard to the Pico W, Classic or LE: picosdl's
keyboard host on BTstack (`hosts/pico/bt`). It looks for one, LE and
Classic in turn (the keyboard in its pairing mode), connects to the first it
finds, shows the digits to type on it if pairing wants them, and then what's
typed and each key as it goes down and up. The keyboard is remembered
(BTstack's store, the flash's last two sectors) and reached for at the next
start. On the console: `s` the state, `r` forget it and look again, `n` look
again, `d` the Bluetooth log; the stick's button held at the start forgets
it too.

`mouse` does the same for a Bluetooth mouse (the same host, looking for a
mouse: a device's reports are read for a keyboard's fields and for a mouse's):
a pointer follows it, with its three buttons, its wheels and a mark where it
was clicked. The mouse and the keyboard are each remembered on their own; one
device at a time.

## Host boards (`hosts/pico`, `hosts/esp32p4`)

The two tested boards. Both builds make the self tests and the demos
(`demos/demos.cmake`), plus the Jet demos (`demos/jet`). Another board needs
its own transport (`libpgpu/pgpu_link.h`; `transports/pico-i2s` and
`transports/esp32p4-i2s` are the examples) and a build like these.

**Raspberry Pi Pico 2 W:** the Pico SDK (2.2.0 here, `PICO_SDK_PATH`). The
build makes one `.uf2` per program in `hosts/pico/build`:

- `gpulink.uf2`: the self tests;
- `gears.uf2`, `keep.uf2`, `toy-pong.uf2`, `jet-viewer.uf2` and so on: the demos;
- `wifi.uf2`, `keyboard.uf2`, `mouse.uf2`: the Pico W's own (above).

The console is on UART0 (GP0/GP1, pins 1 and 2: a Debugprobe's UART bridge).
Programming over SWD while one of these programs runs: stop its DMA first.
The reply ring's DMA goes on with the cores halted, and where the ring lies in
OpenOCD's work area (0x20010000, 64 KB) it writes into the image on its way
to the flash (the verify then fails).

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
- A program's exit ends its session (`STREAM_END`; a killed one's too: its
  signal handler sends it), and the RPi takes commands from I2S again. Over
  the serial port nothing else may use that port meanwhile (a log reader
  takes the program's replies away: "no credit from the RPi").
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
- the HDMI mode the firmware starts with (the RPi then asks for the
  monitor's own, above, unless `hdmi_signal=boot`);
- the USB monitor (off unless chosen).

The page needs Chrome or Edge on a desktop (WebUSB, Web Serial, the File
System Access API) and a secure origin (https, or `localhost`). It's
published at **https://anight.github.io/piegpu/**: `devtools/publish-installer.sh`
builds it (`make-firmware.sh`: each board's firmware, the demos, the test
media) and pushes it to the `gh-pages` branch, one commit that replaces the
last (GitHub Pages serves that branch). The published page is the last one
pushed (it shows its version), not this checkout. From a checkout:

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
  it (WebUSB, the rpiboot protocol; verified on a Zero and a Zero 2 W). The
  page serves the Zero's and the Zero 2 W's kernels, and a `config.txt`
  whose `[pi02]` lines make a Zero 2 W ask for its 64-bit `kernel8.img` (a
  Zero or a Zero W asks for `kernel.img`: the Zero's starts both). Once
  piegpu runs, it says which board it is, and Install writes that board's
  files only (a Zero W's own kernel, with Bluetooth: this part of the page
  hasn't run in a browser yet); a card without a FAT file system can be
  formatted there (one FAT32 partition). The browser needs access to the boot ROM's USB device
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
speakers, or on the Bluetooth speaker (5.1 AAC, mixed down to stereo, at the
`volume=` setting). Test
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
| Raspberry Pi firmware (`bootcode.bin`, `start.elf`, `fixup.dat`) | fetched into `third_party/circle/boot` | Broadcom's licence: binary redistribution ([LICENCE.broadcom](third_party/circle/boot/LICENCE.broadcom) there) | starting the RPi; the VideoCore's H.264 decoder and encoder, ISP and audio service, and its TV and command services (the HDMI mode) | the card, the installer page |
| [FAAD2](https://github.com/knik0/faad2) 2.11.3 | `gpu/audio/faad2` | GPL-2.0-or-later | AAC decoding; the Huffman tables of piegpu's AAC encoder (`gpu/audio/aac_enc_tables.h`) are made from its codebook tables (`tools/aacenc/gen_tables.c`) | the firmware |
| [minimp3](https://github.com/lieff/minimp3) | `gpu/audio/minimp3` | CC0-1.0 | MP3 decoding | the firmware |
| [Tremor](https://gitlab.xiph.org/xiph/tremor) (libvorbisidec) and [libogg](https://gitlab.xiph.org/xiph/ogg) | `gpu/audio/tremor` | BSD-3-Clause | Ogg Vorbis decoding | the firmware |
| Bluedroid's SBC encoder (Broadcom, from Android; the copy in [BTstack](https://github.com/bluekitchen/btstack)'s `3rd-party/bluedroid/encoder`, with its marked changes) | `gpu/bt/sbc_encoder` | Apache-2.0 | the sound's encoding for a Bluetooth speaker | the firmware (the Zero W's, the Zero 2 W's) |
| [LVGL](https://github.com/lvgl/lvgl) 9.4 | `third_party/circle/addon/lvgl/lvgl` (Circle's submodule, fetched by `devtools/build-gpu.sh`) | MIT | the Settings app on the panel (`gpu/ui`) | the firmware |
| [Raspberry Pi userland](https://github.com/raspberrypi/userland) (its MMAL client) | `gpu/video/userland` (a copy), `third_party/userland` | BSD-3-Clause | talking to the VideoCore's video components | the firmware |
| GCC runtime (`libgcc`), newlib's `libm` (Arm GNU Toolchain 15.2) | the toolchain | GPL-3.0 with the GCC Runtime Library Exception; newlib: BSD-style | runtime support linked into the firmware | the firmware |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | fetched by `hosts/pico` | BSD-3-Clause | the Pico host | Pico images |
| [BTstack](https://github.com/bluekitchen/btstack), [cyw43-driver](https://github.com/georgerobotics/cyw43-driver), [lwIP](https://savannah.nongnu.org/projects/lwip/) | in the Pico SDK (`lib/`) | BTstack and cyw43-driver: as Raspberry Pi licenses them for its Pico W boards (the SDK's `LICENSE.RP` files); lwIP: BSD-3-Clause | the Pico W's Bluetooth and Wi-Fi (`keyboard`, `mouse`, `wifi`) | those Pico images |
| picosdl's Bluetooth HID host, stick and Gamepad QT drivers (the same author's) | `hosts/pico/bt`, `hosts/pico/input` | BSD-2-Clause; `hosts/pico/bt` carries BSD-3-Clause material (its README) | a keyboard, a mouse, the stick and the game controller on the Pico | Pico images |
| QR Code generator (Project Nayuki, as LVGL carries it) | `hosts/pico/wifi/qrcodegen.c` | MIT | `wifi`'s QR codes | the `wifi` image |
| [ESP-IDF](https://github.com/espressif/esp-idf) 5.5 | installed separately | Apache-2.0 | the ESP32-P4 host | P4 images |
| [libusb](https://libusb.info) 1.0 | the system's | LGPL-2.1-or-later | the PC host's USB (linked dynamically) | — |
| [Emscripten](https://emscripten.org) | installed separately | MIT or University of Illinois/NCSA | the installer page's demos (WebAssembly; its runtime code is in the output) | the installer page |
| [JetExamples](https://github.com/CubeCoders/JetExamples) scenes (CubeCoders), ported; what they take of [Jet](https://github.com/CubeCoders/Jet) (its light, particles, primitives) | `demos/jet` | MIT | the Jet scenes | Jet scene images |
| JetExamples' data: its own artwork (MIT), a crate texture (Cpt_Flash, CC0), the Utah teapot (FreeGLUT's data, its permissive licence) | `demos/jet/assets` | as named | the Jet scenes | Jet scene images |
| The car of `jet-neon-car` (the "Nascar Intel Edition" model and livery) | `demos/jet/assets/car*.hpp` | none stated: supplied to JetExamples without a licence of its own | `jet-neon-car` | its image |
| picojet (its model viewer, asset converter) | `demos/jet/scenes/viewer.cpp`, `demos/assets` | BSD-2-Clause | the Jet viewer, the demos' models | demo images |
| Meshes and textures: radio, column, biplane | `demos/assets` | CC0 (opengameart.org) | flight, the Jet viewer | demo images |
| Meshes and textures: cube, f117, f22, efa, sphere, crab, the pikuma texture | `demos/assets` | not stated: shipped with Gustavo Pezzi's pikuma.com 3D graphics course, kept under the terms they arrived with | the Jet viewer, flight | demo images |
| [Mesa](https://mesa3d.org) 26.2.3 (patched: `patches/mesa-vc4-dump.patch`) | `third_party/mesa` | MIT (mostly; per file) | `tools/glslc`: GLSL to QPU code, on the PC | — (its output: the demos' shader binaries) |
| [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) (dEQP) | `third_party/VK-GL-CTS` | Apache-2.0 | conformance tests (`tools/deqp`) | — |
| [usbboot](https://github.com/raspberrypi/usbboot) (`rpiboot`) | installed separately | Apache-2.0 | `devtools/run.sh`; the protocol `web/installer/rpiboot.js` speaks | — |
| Arm GNU Toolchain 15.2 | installed separately | GPL-3.0 (GCC, binutils) | building the firmware | — |
| *Big Buck Bunny* trailer (Blender Foundation) | downloaded by `make-firmware.sh` | CC BY 3.0 | Test Video | the installer page |
| "Monkeys Spinning Monkeys" (Kevin MacLeod, incompetech.com) | downloaded by `make-firmware.sh` | CC BY 4.0 | Test Audio | the installer page |
