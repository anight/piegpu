#!/usr/bin/env python3
"""failures.py RESULTS_DIR [PATTERN] - the failed dEQP cases of a run-deqp.py
run with their result text and last log lines (grouped by reason)."""
import collections, fnmatch, glob, os, re, sys

out = sys.argv[1]
pattern = sys.argv[2] if len(sys.argv) > 2 else '*'
reasons = collections.OrderedDict()
for f in sorted(glob.glob(os.path.join(out, 'batch*.qpa')), key=lambda p: int(re.search(r'(\d+)', os.path.basename(p)).group(1))):
    t = open(f, errors='replace').read()
    for m in re.finditer(r'#beginTestCaseResult (\S+)(.*?)#endTestCaseResult', t, re.S):
        name, body = m.group(1), m.group(2)
        st = re.search(r'<Result StatusCode="(\w+)">(.*?)</Result>', body, re.S)
        if not st or st.group(1) in ('Pass', 'NotSupported') or not fnmatch.fnmatchcase(name, pattern):
            continue
        texts = [x.strip().replace('\n', ' ') for x in re.findall(r'<Text>(.*?)</Text>', body, re.S)]
        key = (st.group(1), st.group(2)[:70])
        reasons.setdefault(key, []).append((name, texts[-3:]))
for (status, reason), cases in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
    print(f'{len(cases):5d} {status}: {reason}')
    for name, texts in cases[:3]:
        print(f'        {name}')
        for x in texts:
            print(f'            {x[:160]}')
