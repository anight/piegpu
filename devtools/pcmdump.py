#!/usr/bin/env python3
"""Fetch what the RPi's audio output handed to the VideoCore lately (the gpu
app's PCM host line: the last 20 s, and each 10 ms chunk's timing) and look
for what clicks:

  - the VideoCore's completion flags (bit 30: it ran dry, a gap on HDMI
    though the PCM is whole; shown by SOUNDTEST, which stalls the main loop);
  - chunks asked for late: Circle's VCHIQ sound keeps two chunks queued, so
    a chunk asked for more than two chunks' time after the one before may
    have come after the VideoCore ran dry;
  - chunks padded with silence (the ring had run dry: the decoder behind);
  - jumps in the samples themselves (a second difference far above the
    signal's).

usage: devtools/pcmdump.py [out.wav]   (the RPi in text mode: no GL session)
"""
import base64, glob, os, serial, struct, sys, termios, time, wave
import numpy as np

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join (os.path.dirname (__file__), 'logs', 'pcm.wav')
port = (glob.glob ('/dev/serial/by-id/usb-piegpu_piegpu_*') or sorted (glob.glob ('/dev/ttyACM*')))[0]
s = serial.Serial (port, 115200, timeout=0.2)
a = termios.tcgetattr (s.fd); a[6][termios.VMIN] = 1; termios.tcsetattr (s.fd, termios.TCSANOW, a)
s.reset_input_buffer ()
s.write (b'PCM\n')

lines, buf, started, end = [], b'', False, time.time () + 120
while time.time () < end:
    buf += s.read (65536)
    *done, buf = buf.split (b'\n')
    for l in done:
        l = l.decode (errors='replace').rstrip ('\r')
        if l.startswith ('#PCM'):
            started = True
            lines = []
        if started:
            lines.append (l)
            if l == '#END':
                end = 0
                break
if not lines or lines[-1] != '#END':
    sys.exit ('pcmdump: no dump (is the gpu app running, in text mode?)')
if lines[0] == '#PCM none':
    sys.exit ('pcmdump: no stream has played yet')

_, rate, frames, fmt = lines[0].split ()
rate, frames = int (rate), int (frames)
c = next (i for i, l in enumerate (lines) if l.startswith ('#CHUNKS'))
f = next (i for i, l in enumerate (lines) if l.startswith ('#VCFLAGS'))
# (line by line: the capture comes in two parts, each padded)
pcm = np.frombuffer (b''.join (base64.b64decode (l) for l in lines[1:f]), np.int16).reshape (-1, 2)
_, nflags, lastflags, flagtime = lines[f].split ()
assert len (pcm) == frames, (len (pcm), frames)
chunks = np.array ([[int (x) for x in l.split ()] for l in lines[c + 1:-1]], dtype=np.int64)

w = wave.open (out, 'wb'); w.setnchannels (2); w.setsampwidth (2); w.setframerate (rate)
w.writeframes (pcm.tobytes ()); w.close ()
print ('%s: %.2f s at %u Hz, peak %d, rms %.0f' % (out, frames / rate, rate, np.abs (pcm).max (), pcm.std ()))

# the chunks: times between requests (u32 microseconds, wrapping)
t = chunks[:, 0]; dt = np.diff (t) % (1 << 32) / 1000.0
print ('the VideoCore: %s completions with flags (bits 31-30; Linux reads them as an underrun), the last %s at %.3f s'
       % (nflags, lastflags, ((int (flagtime) - int (t[0])) % (1 << 32)) / 1e6 if int (nflags) else -1))
chunk_ms = 1000.0 * np.median (chunks[:, 1]) / rate
late = np.nonzero (dt > 1.5 * chunk_ms)[0]
print ('%u chunks of %.1f ms; interval ms: median %.1f, max %.1f; over 1.5 chunks: %u'
       % (len (chunks), chunk_ms, np.median (dt), dt.max (), len (late)))
start = t[0]
for i in late[:20]:
    print ('  late: chunk %u at %.3f s: %.1f ms after the one before' % (i + 1, (t[i + 1] - start) % (1 << 32) / 1e6, dt[i]))
short = np.nonzero (chunks[:, 2] < chunks[:, 1])[0]
print ('%u chunks padded with silence (%.0f ms of silence)' % (len (short), (chunks[short, 1] - chunks[short, 2]).sum () * 1000.0 / rate))
for i in short[:20]:
    print ('  silence: chunk %u at %.3f s: %u of %u frames from the ring' % (i, (t[i] - start) % (1 << 32) / 1e6, chunks[i, 2], chunks[i, 1]))
q = chunks[:, 3] * 1000.0 / rate
print ('ring before a chunk, ms: min %.0f, median %.0f' % (q.min (), np.median (q)))

# jumps in the samples: the second difference against the local level (the
# capture's last frames are the newest; it starts where the capture begins)
x = pcm.astype (float).mean (axis=1)
d2 = np.abs (x[2:] - 2 * x[1:-1] + x[:-2])
k = 480
env = np.convolve (np.abs (np.diff (x)), np.ones (k) / k, mode='same')[1:]
spikes = np.nonzero ((d2 > 8 * env + 200))[0]
groups = []
for i in spikes:
    if not groups or i - groups[-1][-1] > rate // 100:
        groups.append ([i])
    else:
        groups[-1].append (i)
print ('%u jumps in the samples' % len (groups))
for g in groups[:20]:
    i = g[0]
    print ('  jump at %.3f s: second difference %.0f, the level around %.0f' % (i / rate, d2[i], env[i]))
