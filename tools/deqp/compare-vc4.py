#!/usr/bin/env python3
"""compare-vc4.py RESULTS_DIR... - our dEQP-GLES2 results next to Mesa's vc4
driver on a Raspberry Pi 3 (the same V3D; Mesa CI's expected failures,
third_party/mesa/src/broadcom/ci/broadcom-rpi3-fails.txt).

Prints the cases that fail here but not with Mesa (ours to fix), those that
fail with both (V3D or compiler limits), and a summary. Cases outside the
conformance list Mesa CI runs (the mustpass, gles2-main.txt) are marked
"(not in mustpass)": Khronos leaves them out, Mesa never runs them."""
import collections, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MESA = os.path.join(ROOT, 'third_party', 'mesa', 'src', 'broadcom', 'ci', 'broadcom-rpi3-fails.txt')

mesa = {}
for line in open(MESA):
    line = line.strip()
    # the arm32-/ubsan- builds run on the same V3D: their failures count too
    for prefix in ('', 'arm32-', 'ubsan-'):
        if line.startswith(prefix + 'dEQP-GLES2.') and ',' in line:
            name, status = line[len(prefix):].split(',', 1)
            mesa.setdefault(name, status)

MUSTPASS = os.path.join(ROOT, 'third_party', 'VK-GL-CTS', 'external', 'openglcts', 'data', 'gl_cts',
                        'data', 'mustpass', 'gles', 'aosp_mustpass', 'main', 'gles2-main.txt')
mustpass = set(l.strip() for l in open(MUSTPASS))


def note(n):
    return '' if n in mustpass else ' (not in mustpass)'


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
    print(f'  {n} {ours[n]}{note(n)}')
print(f'fail with both ({len(both)}):')
for n in both:
    print(f'  {n} {ours[n]}{note(n)}')
print(f'pass here, fail with Mesa vc4 ({len(we_pass)}):')
for n in we_pass:
    print(f'  {n}')
c = collections.Counter(ours.values())
print('summary: ' + ', '.join(f'{k} {v}' for k, v in sorted(c.items())) + f' of {len(ours)}')
m = collections.Counter(s for n, s in ours.items() if n in mustpass)
print('mustpass: ' + ', '.join(f'{k} {v}' for k, v in sorted(m.items())) + f' of {sum(m.values())}')
