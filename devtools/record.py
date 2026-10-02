#!/usr/bin/env python3
"""Record what the RPi shows and plays as an MP4: the gpu app's ENC host line
(text mode) puts the frames shown through the VideoCore's H.264 encoder and
keeps the sound its output takes meanwhile; both come back over the RPi's USB
serial port at the end, and ffmpeg (needed here) makes the file: the video as
it is, the sound as AAC.

With --card the RPi makes the MP4 itself, on its SD card as it records
(RECnnn.MP4: the sound as PCM, there being no AAC encoder there), and the
file is fetched from the card as it is; ffmpeg isn't needed.

usage: devtools/record.py SECONDS OUT.mp4 [--kbit N] [--port DEV] [--keep] [--card]
  --kbit: the video's bit rate (default 1000)
  --port: the RPi's serial port (default: /dev/serial/by-id/*piegpu*)
  --keep: leave OUT.h264 and OUT.pcm (the stream and the sound as they came);
          with --card: leave the file on the card
  --card: the RPi's own MP4, by way of its card
"""
import base64, glob, os, re, subprocess, sys, time
import serial

opts = {'--kbit': '1000', '--port': None}
argv, args, keep, card = sys.argv[1:], [], False, False
while argv:
    a = argv.pop(0)
    if a in opts:
        opts[a] = argv.pop(0)
    elif a == '--keep':
        keep = True
    elif a == '--card':
        card = True
    else:
        args.append(a)
if len(args) != 2:
    sys.exit(__doc__)
seconds, out = int(args[0]), args[1]
port = opts['--port'] or (glob.glob('/dev/serial/by-id/*piegpu*') or [None])[0]
if not port:
    sys.exit('record: no RPi serial port found (--port)')

s = serial.Serial(port, 115200, timeout=0.1)
time.sleep(0.3)
s.reset_input_buffer()


def lines_till(done, limit, what):
    buf, start = b'', time.time()
    while not done(buf):
        buf += s.read(1 << 18)
        if time.time() - start > limit:
            sys.exit('record: ' + what + ':\n' + '\n'.join(
                     l for l in buf.decode('latin1').splitlines()[-40:] if ' enc: ' in l or ' mp4: ' in l or 'gpu: ' in l))
    return buf.decode('latin1')


if card:
    s.write(('ENC %d %s RGB FILE\n' % (seconds, opts['--kbit'])).encode())
    text = lines_till(lambda b: b'enc: The file:' in b and b.endswith(b'\n'), seconds + 30, 'no file was made')
    for l in text.splitlines():
        if ' enc: all' in l or ' enc: The file' in l:
            print(l)
    name = re.findall(r'Recording into (\S+)', text)[-1]
    if 'The file: good' not in text:
        sys.exit('record: %s on the card is no good' % name)
    s.write(('GETFILE %s\n' % name).encode())
    text = lines_till(lambda b: b'#FILE' in b and b'#END' in b[-4096:].split(b'#FILE')[-1], 900, 'the file did not come')
    m = re.search(r'#FILE (\S+) (\d+)\n(.*?)#END', text, re.S)
    data = base64.b64decode(''.join(x for x in m.group(3).split('\n') if re.fullmatch(r'[A-Za-z0-9+/=]+', x)))
    if len(data) != int(m.group(2)):
        sys.exit('record: %d of %s bytes of %s came' % (len(data), m.group(2), name))
    open(out, 'wb').write(data)
    if not keep:
        s.write(('DELFILE %s\n' % name).encode())
        time.sleep(0.5)
    print('%s: %d bytes, the RPi\'s %s%s' % (out, len(data), name, ' (still on its card)' if keep else ''))
    sys.exit(0)

s.write(('ENC %d %s RGB DUMP\n' % (seconds, opts['--kbit'])).encode())
start, buf = time.time(), b''
while not (b'#SOUND' in buf and buf.rstrip().endswith(b'#END')):
    buf += s.read(1 << 16)
    if time.time() - start > seconds + 10 and b'#H264' not in buf:
        sys.exit('record: no stream from the RPi:\n' + '\n'.join(
                 l for l in buf.decode('latin1').splitlines() if ' enc: ' in l or 'ENC' in l))
    if time.time() - start > seconds + 120:
        sys.exit('record: the dump did not end')
text = buf.decode('latin1')
for l in text.splitlines():
    if ' enc: all' in l or ' enc: The sound' in l:
        print(l)


def part(tag):
    m = re.search(r'#' + tag + r' ([^\n]*)\n(.*?)#END', text, re.S)
    lines = [x for x in m.group(2).split('\n') if re.fullmatch(r'[A-Za-z0-9+/=]+', x)]
    return m.group(1).split(), base64.b64decode(''.join(lines))


(said, frames, us), video = part('H264')
(rate, sound_frames, _), sound = part('SOUND')
if len(video) != int(said) or len(sound) != int(sound_frames) * 4:
    sys.exit('record: %d of %s bytes of video, %d of %d of sound came' % (len(video), said, len(sound), int(sound_frames) * 4))
if not int(frames):
    sys.exit('record: no frames (is a program drawing?)')

base = os.path.splitext(out)[0]
h264, pcm, timed = base + '.h264', base + '.pcm', base + '.video.mp4'
open(h264, 'wb').write(video)
open(pcm, 'wb').write(sound)
fps = '%.4f' % (int(frames) * 1e6 / int(us))		# the rate the frames really came at
# (the bare stream has no times: a first pass gives the frames theirs)
subprocess.check_call(['ffmpeg', '-v', 'error', '-y', '-r', fps, '-i', h264, '-c', 'copy', timed])
cmd = ['ffmpeg', '-v', 'error', '-y', '-i', timed]
if int(sound_frames):
    cmd += ['-f', 's16le', '-ar', rate, '-ac', '2', '-i', pcm, '-map', '0:v', '-map', '1:a',
            '-c:a', 'aac', '-b:a', '160k', '-af', 'apad', '-t', '%.3f' % (int(us) / 1e6)]
subprocess.check_call(cmd + ['-c:v', 'copy', out])
os.remove(timed)
if not keep:
    os.remove(h264)
    os.remove(pcm)
print('%s: %s frames at %s a second, %s' % (out, frames, fps,
      'sound at %s Hz' % rate if int(sound_frames) else 'no sound (no output was playing)'))
