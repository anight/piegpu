#!/usr/bin/env python3
"""compare-vc4.py RESULTS_DIR... - our dEQP-GLES2 results next to Mesa's vc4
driver on a Raspberry Pi 3 (the same V3D; Mesa CI's expected failures,
third_party/mesa/src/broadcom/ci/broadcom-rpi3-fails.txt).

Prints the cases that fail here but not with Mesa (ours to fix), those that
fail with both (V3D or compiler limits), and a summary."""
import collections, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MESA = os.path.join(ROOT, 'third_party', 'mesa', 'src', 'broadcom', 'ci', 'broadcom-rpi3-fails.txt')

mesa = {}
for line in open(MESA):
    line = line.strip()
    if line.startswith('dEQP-GLES2.') and ',' in line:	# not the arm32-/ubsan- variants
        name, status = line.split(',', 1)
        mesa[name] = status

ours = collections.OrderedDict()
for d in sys.argv[1:]:
    for line in open(os.path.join(d, 'results.txt')):
        name, status = line.split()
        ours[name] = status

bad = ('Fail', 'Crash', 'Timeout', 'Missing', 'ResourceError', 'InternalError', 'QualityWarning')
only_ours = [n for n, s in ours.items() if s in bad and n not in mesa]
both = [n for n, s in ours.items() if s in bad and n in mesa]
we_pass = [n for n, s in ours.items() if s == 'Pass' and n in mesa]

print(f'fail here, not with Mesa vc4 ({len(only_ours)}):')
for n in only_ours:
    print(f'  {n} {ours[n]}')
print(f'fail with both ({len(both)}):')
for n in both:
    print(f'  {n} {ours[n]}')
print(f'pass here, fail with Mesa vc4 ({len(we_pass)}):')
for n in we_pass:
    print(f'  {n}')
c = collections.Counter(ours.values())
print('summary: ' + ', '.join(f'{k} {v}' for k, v in sorted(c.items())) + f' of {len(ours)}')
