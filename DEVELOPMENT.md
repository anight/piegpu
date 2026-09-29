# Developing piegpu

Notes on how the RPi side works inside, for changing it. How to build and
use it is in the [README](README.md). Supported for now: the Raspberry Pi
Zero / Zero W and the Zero 2 W; where a model is named below, the fact was
measured on that one.

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
| `devtools/configure-circle.sh` | `-d MEM_PERSISTENT_SIZE=0x10000` |
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

`configure-circle.sh` clones the fork into a board's tree when it isn't
there, and configures it with `MEM_PERSISTENT_SIZE`. Without `MEM_PERSISTENT_SIZE`, `runlog.cpp` stops the
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
KB); the binning list takes about 10 us. That's 1-2% of a 60 fps frame, on an
ARM that runs at 600 MHz: the firmware starts it there and piegpu doesn't
raise it (a plain byte-by-byte read of the same 6.9 KB takes 45 us). A job
refused costs nothing more.

Verified with the check on: the demos, the video, gltest (the same 19 known
failures on HDMI as without it) and dEQP-GLES2's 2015-case subset (1999 pass,
16 fail: the same cases as before), with no job refused; and the refusal above.
`clcheck=off` turns it off.
