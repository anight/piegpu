#!/usr/bin/env python3
"""caselog.py RESULTS_DIR CASE - the log text of one dEQP case (GL calls, messages)"""
import glob, os, re, sys, html
for f in glob.glob(os.path.join(sys.argv[1], 'batch*.qpa')):
    t = open(f, errors='replace').read()
    m = re.search(r'#beginTestCaseResult ' + re.escape(sys.argv[2]) + r'\n(.*?)#endTestCaseResult', t, re.S)
    if m:
        body = re.sub(r'<Image .*?</Image>', '[image]', m.group(1), flags=re.S)
        body = re.sub(r'<ShaderSource>(.*?)</ShaderSource>', lambda x: '\n' + x.group(1) + '\n', body, flags=re.S)
        body = re.sub(r'<[^>]+>', ' ', body)
        print(html.unescape(re.sub(r'\n\s*\n', '\n', body)))
