# Developing pico-gpu

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
| `patches/circle-persistent-memory.patch` | Circle leaves the top of the ARM memory to the app |
| `devtools/configure-circle.sh` | Applies the patch; `-d MEM_PERSISTENT_SIZE=0x10000` |
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
(`PROPTAG_GET_ARM_MEMORY`) to its heap and page allocator. The patch
subtracts `MEM_PERSISTENT_SIZE` from that size in `CMemorySystem`
(`lib/memory.cpp` and `lib/memory64.cpp`). The top 64 KB is then the app's:
no allocator ever returns it. The run log finds it with the same property
tag, at `nBaseAddress + nSize - MEM_PERSISTENT_SIZE`.

`configure-circle.sh` applies the patch to `circle-zero2` when it creates
that tree. The `circle` tree has the patches applied in place, like the
others in `patches/`. Without `MEM_PERSISTENT_SIZE`, `runlog.cpp` stops the
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
