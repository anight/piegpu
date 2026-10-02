#!/usr/bin/env python3
"""compare.py RATE in.pcm decoded.pcm : what the encoder's units decode to
(ffmpeg -i out.aac -f s16le decoded.pcm) against what went in (16-bit stereo
frames): how far it is shifted, its level, the noise under it, band by band."""
import sys
import numpy as np

rate = int(sys.argv[1])
a = np.frombuffer(open(sys.argv[2], 'rb').read(), dtype='<i2').reshape(-1, 2).astype(float)
b = np.frombuffer(open(sys.argv[3], 'rb').read(), dtype='<i2').reshape(-1, 2).astype(float)
# the shift: where a stretch of the original fits the decoded best
at, n = min(len(a), len(b)) // 3, 1 << 15
ref = a[at:at + n, 0]
best = max(range(-3000, 3001), key=lambda lag: np.dot(ref, b[at + lag:at + lag + n, 0]))
print('the decoded sound is %d frames late' % best)
m = min(len(a), len(b) - best) - 2048
x, y = a[2048:m], b[2048 + best:m + best]
gain = np.sum(x * y) / np.sum(x * x)
noise = y - x
print('its level: %.4f of the original; the noise %.1f dB under the sound (left %.1f, right %.1f)' % (
      gain, 10 * np.log10(np.sum(x * x) / np.sum(noise * noise)),
      10 * np.log10(np.sum(x[:, 0] ** 2) / np.sum(noise[:, 0] ** 2)), 10 * np.log10(np.sum(x[:, 1] ** 2) / np.sum(noise[:, 1] ** 2))))
# by frequency: the sound's and the noise's spectra, averaged
w = np.hanning(2048)
frames = range(0, len(x) - 2048, 1024)
S = sum(np.abs(np.fft.rfft(x[i:i + 2048, 0] * w)) ** 2 for i in frames)
Nz = sum(np.abs(np.fft.rfft(noise[i:i + 2048, 0] * w)) ** 2 for i in frames)
f = np.fft.rfftfreq(2048, 1 / rate)
for lo, hi in ((0, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 12000), (12000, 16000), (16000, 20000)):
    k = (f >= lo) & (f < hi)
    print('  %5d - %5d Hz: the sound %5.1f dB, the noise %5.1f dB under it' % (
          lo, hi, 10 * np.log10(S[k].sum() / len(frames) + 1e-9) - 10 * np.log10(32768.0 ** 2 * 2048), 10 * np.log10((S[k].sum() + 1e-9) / (Nz[k].sum() + 1e-9))))
print('the first unit: the original\'s frames rms %.0f, the noise\'s %.0f' % (np.sqrt(np.mean(a[:1024] ** 2)), np.sqrt(np.mean((b[best:best + 1024] - a[:1024]) ** 2)) if best >= 0 else -1))
