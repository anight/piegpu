# Installing piegpu

piegpu runs from the microSD card of a Raspberry Pi Zero, Zero W or Zero 2 W.
Installing it means putting a few files on that card. The installer page
does it for you, in one of two ways:

- **Way A, with a card reader** (recommended): the page writes the files onto
  the card while it's in your computer. No drivers, no permissions.
- **Way B, without a card reader:** the card stays in the RPi; the page
  starts piegpu on the RPi over USB, and piegpu writes its own card.

Then you connect the RPi to the page over USB to check it, change its
settings, update it and run the demos.

What has been tested so far: everything on Linux (Ubuntu) with Chrome, on
both boards, except the page writing onto a real card in Way A (the files it
writes were tested on a card written by hand, and the writing on a
browser-private folder). Nothing has been tried on Windows or macOS yet: the
steps for them follow what those systems and Chrome document, and are marked
"not tested".

- [What you need](#what-you-need)
- [Open the installer page](#open-the-installer-page)
- [Step 1: put piegpu on the card](#step-1-put-piegpu-on-the-card)
  - [Way A: with a card reader](#way-a-with-a-card-reader)
  - [Way B: without a card reader](#way-b-without-a-card-reader)
- [Step 2: connect](#step-2-connect)
- [Step 3: settings and a test](#step-3-settings-and-a-test)
- [Updating](#updating)
- [Formatting a card as FAT32](#formatting-a-card-as-fat32)
- [Access to USB devices](#access-to-usb-devices)
- [Troubleshooting](#troubleshooting)
- [Running the page yourself](#running-the-page-yourself)

## What you need

- **The board:** a Raspberry Pi Zero, Zero W or Zero 2 W. The Zero and the
  Zero W take the same files; the Zero 2 W other ones. To tell them apart:
  the Zero 2 W's big chip is marked RP3A0, and its name is printed on the
  board.
- **A microSD card:** any size; piegpu needs about 4 MB of it. It must hold
  a FAT32 file system (most cards up to 32 GB come that way; bigger ones come
  as exFAT: see [Formatting a card as FAT32](#formatting-a-card-as-fat32)).
  Whatever is on the card may be overwritten.
- **A USB cable with data lines** for the RPi's port marked **USB** (the
  inner one of the two), not PWR IN. Some cables only charge: with one of
  those the RPi gets power but the computer never sees it. The same cable
  powers the RPi.
- **A computer** with Linux, Windows or macOS, and **Chrome or Edge** (or
  another Chromium browser). Firefox and Safari can only do Way A through a
  zip file, and can't connect to the RPi.
- **To see it working** (optional): an HDMI monitor on the RPi's mini-HDMI
  port, or an ST7789 panel wired to it (see the [README](../README.md#wiring)).
  With neither, the page is the only sign of life: piegpu doesn't blink the
  RPi's LED.
- **Way A only:** a card reader for microSD.

## Open the installer page

Open **https://anight.github.io/piegpu/** in Chrome or Edge. It's the page
of this repository (`web/installer`), with the latest firmware for both
boards, published from it (`devtools/publish-installer.sh`). Everything
runs in the browser: the page talks to the RPi directly.

A copy of the page on your own computer works the same way: [Running the
page yourself](#running-the-page-yourself). Opening `index.html` as a file
does not work (the browser refuses its scripts, and USB needs `https` or
`localhost`).

## Step 1: put piegpu on the card

Choose **Way A** if you have a card reader, else **Way B**. A card that
already has piegpu needs neither: go to [Step 2](#step-2-connect).

### Way A: with a card reader

1. **Check that the card is FAT32.** Put it in the card reader.
   - Windows: in File Explorer, right-click the card, Properties: "File
     system: FAT32".
   - macOS: in Finder, select the card, File > Get Info: "Format: MS-DOS
     (FAT32)".
   - Linux: in GNOME Disks, select the card: "FAT (32-bit version)"; or run
     `lsblk -f` and look for `vfat`.

   If it's anything else (exFAT, NTFS, or a card from Raspberry Pi OS with
   two partitions), format it first: [Formatting a card as
   FAT32](#formatting-a-card-as-fat32).
2. **Prepare the card.** On the page press **Prepare a card**, choose the
   board, and (optionally) open section 3, **Settings**, to change them:
   they go on the card too. Then:
   - Chrome, Edge: press **Choose the card and write**, and pick the card's
     top folder (the card itself: on Windows its drive, e.g. `E:\`; on macOS
     `/Volumes/<name>`; on Linux `/media/<you>/<name>` or
     `/run/media/<you>/<name>`). The browser asks to let the page change the
     folder: allow it. The page writes six files, reads each back and
     checks it. If the folder holds other things, the page asks first:
     that's the moment to notice a wrong folder.
   - Any browser: press **Download a .zip**, and unpack the zip onto the
     card, so that the six files are at the top of the card (not in a
     folder of their own).

   The files: `bootcode.bin`, `start.elf`, `fixup.dat` (the Raspberry Pi's
   firmware), `kernel.img` (Zero) or `kernel8.img` (Zero 2 W), and
   `config.txt` and `cmdline.txt` (the settings).
3. **Eject the card** (Windows: "Eject"; macOS: drag it to the Trash or
   press the eject button; Linux: the eject button in the file manager). Take
   it out, put it in the RPi.
4. Go on with [Step 2](#step-2-connect). The RPi starts piegpu from the card
   as soon as it has power: a monitor or panel shows piegpu's screen,
   "waiting for OpenGL ES or media".

### Way B: without a card reader

The RPi can start from your computer over USB: with no bootable card, its
boot ROM waits for the computer to send it the files. The page does that,
then piegpu, running from memory, formats and writes its own card.

1. **Once per computer, let the browser reach the RPi's boot mode** (a USB
   device, `0a5c:2763` on a Zero, `0a5c:2764` on a Zero 2 W):
   - Linux: `sudo apt install rpiboot` (Debian, Ubuntu: its package brings a
     udev rule for these devices; replug the RPi afterwards), or add the rule
     yourself: [Access to USB devices](#access-to-usb-devices).
   - Windows (not tested): the device needs the WinUSB driver; see [Access
     to USB devices](#access-to-usb-devices).
   - macOS (not tested): nothing is expected to be needed.
2. **The card:** leave the new or empty card in the RPi, or no card. A card
   that holds another system (Raspberry Pi OS, say) must come out: the RPi
   would start that instead. The page asks for it later.
3. **Connect the RPi's USB port** (not PWR IN) to the computer.
4. On the page open **No card reader? Start a blank RPi over USB** and press
   **Start a blank RPi**. Chrome shows its USB device picker: pick the
   RPi's boot device (a Zero 2 W's is named "BCM2710 Boot"). The RPi takes
   the first file, then
   appears as a new device: if the page says so, press **Continue** and pick
   it again (Chrome asks for each new device once); likewise for piegpu's
   serial port at the end.
5. piegpu starts (the screen shows it) and the page connects to it:
   section 2 shows the board and the card. If you took the card out, put it
   in now. A card without a FAT32 file system gets **Erase and format the
   card** (the RPi formats it: a 64 GB card in 7 seconds).
6. Press **Install piegpu** (section 4): piegpu writes its files onto the
   card, checks each, and restarts from the card. Done: the RPi now starts
   piegpu on its own.

## Step 2: connect

1. Connect the RPi's **USB** port (not PWR IN) to the computer with a data
   cable. piegpu is up within a few seconds.
2. On the page press **Connect**. The first time, Chrome shows its serial
   port picker: pick **piegpu**. Afterwards the page finds it by itself.
3. Section 2 shows the board, the card and the firmware on it.

The page talks to piegpu over the RPi's USB serial port. Linux needs your
user to have access to it (once: see [Access to USB
devices](#access-to-usb-devices)); Windows 10 and later and macOS have the
driver built in (not tested).

## Step 3: settings and a test

- Section 3, **Settings** (press **Show**): where GL commands come from
  (Auto: a PC over USB once it connects, else a microcontroller over I2S),
  the screen (HDMI while a monitor is connected, else the panel), the panel,
  the HDMI mode, and the USB monitor for a Linux desktop (off by default).
  **Save the settings only** writes them to the card and restarts the RPi.
  The same settings are the card's `cmdline.txt` and `config.txt`, which can
  also be edited by hand (the [README](../README.md#kernel-command-line-cmdlinetxt)
  lists the options).
- Section 4: **Test OpenGL** runs glxgears-style gears on the RPi from the
  page; **Test Video** plays a film with its sound, **Test Audio** an MP3
  (sound needs an HDMI monitor with speakers). **Stop** ends it.

## Updating

- **Upgrade** (next to the card's firmware in section 2) appears when the
  page has a newer version: it writes the new kernel only and restarts the
  RPi. Settings and the other files stay.
- **Install piegpu** writes all the files again, with the settings.
- Way A works for updates too: **Prepare a card** again, with the card in the
  card reader.

## Formatting a card as FAT32

Formatting erases everything on the card. The card needs one FAT32
partition (MBR partition table).

- **Any system, any size: the RPi itself.** Way B, step 5: **Erase and
  format the card**. Needs Way B's one-time setup.
- **Any system: Raspberry Pi Imager** (raspberrypi.com/software): Choose OS
  > Erase (format as FAT32), choose the card, Write. Some users report it
  making exFAT instead on big cards: check the result (Way A, step 1).
- **Windows, cards up to 32 GB:** File Explorer, right-click the card,
  Format, File system FAT32. For bigger cards Windows' Format offers only
  exFAT and NTFS: use one of the ways above.
- **macOS:** Disk Utility, View > Show All Devices, select the card itself
  (not a volume on it), Erase: Format "MS-DOS (FAT)", Scheme "Master Boot
  Record". (Not tested.)
- **Linux:** GNOME Disks: select the card, the menu > Format Disk: "Compatible
  with all systems and devices (MBR / DOS)"; then + (create a partition),
  type "For use with all systems and devices (FAT)". Tested here with the
  same steps done through udisks.

## Access to USB devices

Browsers can use only the USB devices the system lets your user open.

- **Linux, the serial port (Step 2):** the port (`/dev/ttyACM0` or so)
  belongs to the `dialout` group. Once: `sudo usermod -aG dialout $USER`,
  then log out and in again. Some packages grant it to the desktop user
  anyway (e.g. `esptool`'s udev rule).
- **Linux, the boot mode (Way B):** USB devices are read-only for users by
  default. `sudo apt install rpiboot` brings a rule; or create
  `/etc/udev/rules.d/70-rpiboot.rules` with

  ```
  SUBSYSTEM=="usb", ATTR{idVendor}=="0a5c", ATTR{idProduct}=="2763|2764", TAG+="uaccess"
  ```

  then run `sudo udevadm control --reload` and replug the RPi.
- **Windows, the boot mode (Way B; not tested):** Chrome can use a USB device
  only through the WinUSB driver, and the RPi's boot mode has no driver of its
  own. Zadig (zadig.akeo.ie) can install WinUSB for it: plug the RPi in with
  no bootable card, in Zadig choose Options > List All Devices, pick the
  device with USB ID `0A5C 2763` (Zero) or `0A5C 2764` (Zero 2 W), WinUSB,
  Install Driver. Once per device ID.
- **Windows, the serial port:** Windows 10 and later load their own driver
  for it (not tested).
- **macOS:** no setup expected for either (not tested).

## Troubleshooting

**The page:**

- *The page is blank or its buttons do nothing:* it was opened as a file.
  Open https://anight.github.io/piegpu/, or serve it ([Running the page
  yourself](#running-the-page-yourself)).
- *"This browser can't talk to USB devices":* use Chrome or Edge on a
  desktop. Way A's zip still works in any browser.

**Way A:**

- *The folder picker refuses the folder, or nothing happens:* pick the card
  itself, not a folder on it; or use **Download a .zip**.
- *The page says the folder "holds other items":* it isn't an empty card
  (or not the card at all). A used card is fine if you mean it: its old
  files stay, piegpu's replace theirs. Better, format it first.
- *The RPi doesn't start piegpu from the card* (a monitor stays black, and
  Connect finds nothing):
  - the card isn't FAT32 on an MBR partition table: format it;
  - the files are in a folder on the card instead of at its top;
  - the card was prepared for the other board: prepare it again, choosing
    the right one;
  - the card was pulled out before the writing finished: eject it properly.

**Way B:**

- *The USB picker shows no Raspberry Pi device:*
  - the RPi started from its card instead: take the card out;
  - the cable goes to PWR IN, or it's a charge-only cable;
  - on Linux the device shows but opening it fails: the udev rule (Way B,
    step 1) is missing, or the RPi wasn't replugged after adding it.
- *It stops after the first file:* a Zero 2 W's boot ROM once stopped
  answering after taking it, twice early in testing; it answers nothing
  until it loses power. Unplug the cable, plug it back in, start again.
- *The page asks to press Continue:* Chrome needs a click to pick each new
  USB device (the ROM, then the files' server, then piegpu's port). Press it
  and pick the device.

**Connect:**

- *The serial port picker lists nothing:*
  - piegpu isn't running: see "The RPi doesn't start piegpu" above; after
    power-up give it a few seconds;
  - the cable goes to PWR IN, or it's a charge-only cable;
  - Windows: Device Manager should list the RPi under Ports as a USB serial
    device; if it shows with a warning, its driver didn't load (not tested).
- *The port is listed but opening it fails:*
  - Linux: your user has no access to it ([Access to USB
    devices](#access-to-usb-devices)); the page says so;
  - another program has it open: another tab with this page, a serial
    terminal, `devtools/run.sh --log`. Close it.
- *"piegpu doesn't answer":* a program left it taking GL commands; the page
  restarts it by itself. **Reset** restarts it any time.
- *Connect works but the Test buttons do nothing on the RPi:* the card's
  Host setting is I2S: set it to Auto or USB and save the settings.

## Running the page yourself

To run the page from a checkout (your own build, or offline). On Linux (the
build is Linux-only for now):

```bash
git clone --recurse-submodules https://github.com/anight/piegpu.git
```

```bash
cd piegpu && web/installer/make-firmware.sh
```

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory web/installer
```

`make-firmware.sh` builds the firmware for both boards (it needs the Arm
toolchains: [README](../README.md#boards-and-building)), the page's demos if
Emscripten is installed, and downloads the test video and MP3. Then open
http://localhost:8765. Another computer can use the served files as well:
copy `web/installer` there and serve it the same way (Python 3 on Windows:
`py -m http.server 8765 --bind 127.0.0.1 --directory installer`).
