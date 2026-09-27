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
| `hosts/` | builds per host board: `pico`, `esp32p4`, `pc` |
| `demos/` | the demos, for every host |
| `protocol/` | the wire format header, shared by both sides |
| `devtools/` | boot the Zero over USB, logs, screenshots (`run.sh`) |

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
  Raspberry Pi layout. The bit clock is 25 MHz on a v1.x chip (the dev kit's)
  and 40 MHz from chip revision v3.0 on (`transports/esp32p4-i2s`).
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

The panel's SDO (MISO) isn't used. Because nothing can be read back, the Zero
can't detect the panel: without one, set `panel=none` (see below).

### Zero ↔ HDMI, and the PC

- **HDMI:** a monitor on the mini-HDMI port. By default the screen is on HDMI
  while a monitor is connected and on the panel otherwise; the Zero watches the
  hot-plug line and reads the monitor's EDID for its resolution.
  `devtools/config.txt` has `hdmi_force_hotplug=1` so that HDMI stays on when
  the Zero boots without a monitor.
- **PC:** the Zero's "USB" port (not "PWR IN"), with no SD card. The Zero
  boots over USB from `rpiboot`. The same cable powers it and carries its log;
  `devtools/run.sh gpu` builds, boots and logs.

### Kernel command line (`cmdline.txt`)

| Option | Meaning |
|---|---|
| `output=auto` | the default: HDMI while a monitor is connected, else the panel |
| `output=panel`, `output=hdmi` | always this output |
| `panel=none` | no panel: without a monitor the screen stays on HDMI |
| `hdmi_pixels=N` | cap the screen on HDMI to N pixels (default: the monitor's native resolution, up to 1920x1200) |

With `devtools/run.sh`, pass these as `CMDLINE="output=panel" devtools/run.sh gpu`.
