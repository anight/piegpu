#!/usr/bin/env python3
"""run-deqp.py - run dEQP-GLES2 on pigpu (pgl on the PC, the RPi over
USB; tools/deqp/build-deqp.sh builds it).

usage: run-deqp.py [-o OUT_DIR] [-x EXCLUDE]... [--resume] [--pbuffer] PATTERN...

  PATTERN  test case names with wildcards, e.g. 'dEQP-GLES2.functional.color_clear.*'

dEQP stops at a crash; this runs the cases in batches and goes on after the
case that crashed (it is reported as "Crash"). Writes OUT_DIR/results.txt (one
line per case: name status) and prints a summary per group.
"""
import argparse, collections, fnmatch, os, re, subprocess, sys, time

# the RPi's serial port: the gpu app's (devtools/pgpugadget: "pigpu" and
# the board's serial number), or an older one's (Circle's CDC gadget); the
# transport (transports/pc-usb) is told which by PGPU_TTY
import glob
TTY = (glob.glob('/dev/serial/by-id/usb-pigpu_pigpu_*-if00')
       or ['/dev/serial/by-id/usb-Circle_CDC_Gadget-if00'])[0]
os.environ.setdefault('PGPU_TTY', TTY)
STALL_SECONDS = 120		# no log output for this long: the case hangs

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
GLES2 = os.path.join(ROOT, 'third_party', 'deqp-build', 'modules', 'gles2')
DEQP = os.path.join(GLES2, 'deqp-gles2')
BASE_ARGS = ['--deqp-log-images=disable', '--deqp-log-shader-sources=disable',
             '--deqp-visibility=hidden', '--deqp-watchdog=disable']
PANEL = ['--deqp-surface-width=320', '--deqp-surface-height=240']
# Mesa CI's vc4 configuration (deqp-broadcom-rpi3-gl.toml): 256x256 RGBA8888
PBUFFER = ['--deqp-surface-type=pbuffer', '--deqp-surface-width=256', '--deqp-surface-height=256']


def all_cases():
    """the case list of the module (deqp-gles2 --deqp-runmode=txt-caselist)"""
    out = os.path.join(GLES2, 'dEQP-GLES2-cases.txt')
    if not os.path.exists(out):
        subprocess.run([DEQP, '--deqp-runmode=txt-caselist'], cwd=GLES2, check=True,
                       stdout=subprocess.DEVNULL)
    return [l[6:].strip() for l in open(out) if l.startswith('TEST: ')]


def parse_log(path):
    """(case -> status) from a .qpa log; the last case started without a result"""
    results, started = {}, None
    if not os.path.exists(path):
        return results, started
    text = open(path, errors='replace').read()
    for m in re.finditer(r'#beginTestCaseResult (\S+)|#endTestCaseResult|#terminateTestCaseResult (\S+)|'
                         r'<Result StatusCode="(\w+)"', text):
        if m.group(1):
            started = m.group(1)
        elif m.group(3) and started:
            results[started] = m.group(3)
        elif m.group(0).startswith('#terminate') and started:
            results[started] = m.group(2)
            started = None
        elif m.group(0) == '#endTestCaseResult':
            started = None
    return results, started


def reboot_rpi():
    """load the gpu app on the RPi again (after a crash or a hang: its
    watchdog reboots it into USB boot); False if it doesn't come back"""
    for attempt in range(3):
        print(f'run-deqp: rebooting the RPi', flush=True)
        subprocess.run([os.path.join(ROOT, 'devtools', 'run.sh'), 'gpu', '0'], cwd=ROOT,
                       env=dict(os.environ, CAMERA='none'), stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        for _ in range(30):
            if os.path.exists(TTY):
                time.sleep(2)
                return True
            time.sleep(1)
    return False


def write_results(path, cases, results):
    with open(path, 'w') as f:
        for c in cases:
            f.write(f'{c} {results.get(c, "Missing")}\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default=os.path.join(ROOT, 'third_party', 'deqp-results'))
    ap.add_argument('-x', '--exclude', action='append', default=[], help='PATTERN to leave out')
    ap.add_argument('--resume', action='store_true',
                    help='keep the results of an interrupted run in OUT_DIR, run the rest')
    ap.add_argument('--pbuffer', action='store_true',
                    help="render offscreen like Mesa CI (256x256 RGBA8888), not on the panel")
    ap.add_argument('patterns', nargs='+')
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)		# dEQP runs in its data directory
    if not args.resume and os.path.exists(os.path.join(args.out, 'caselist.txt')):
        sys.exit(f'run-deqp: {args.out} has a run (use --resume or remove it)')
    os.makedirs(args.out, exist_ok=True)

    cases = [c for c in all_cases() if any(fnmatch.fnmatchcase(c, p) for p in args.patterns)
             and not any(fnmatch.fnmatchcase(c, p) for p in args.exclude)]
    print(f'run-deqp: {len(cases)} cases', flush=True)
    results = collections.OrderedDict()
    batch = 0
    if args.resume:
        # finished cases of the earlier batches (a case interrupted by the
        # stop runs again)
        while os.path.exists(os.path.join(args.out, f'batch{batch}.qpa')):
            got, _ = parse_log(os.path.join(args.out, f'batch{batch}.qpa'))
            results.update(got)
            batch += 1
        print(f'run-deqp: resuming, {len(results)} cases done', flush=True)
    remaining = [c for c in cases if c not in results]
    t0 = time.time()
    while remaining:
        # an RPi booted just before this run may not be on USB yet
        for _ in range(30):
            if os.path.exists(TTY):
                break
            time.sleep(1)
        else:
            if not reboot_rpi():
                sys.exit('run-deqp: the RPi does not come up')
        caselist = os.path.join(args.out, 'caselist.txt')
        open(caselist, 'w').write('\n'.join(remaining) + '\n')
        log = os.path.join(args.out, f'batch{batch}.qpa')
        with open(os.path.join(args.out, f'batch{batch}.out'), 'w') as out:
            proc = subprocess.Popen([DEQP, f'--deqp-caselist-file={caselist}', f'--deqp-log-filename={log}']
                                    + BASE_ARGS + (PBUFFER if args.pbuffer else PANEL), cwd=GLES2, stdout=out, stderr=subprocess.STDOUT,
                                    env=dict(os.environ, PGPU_TEXT_LOG=os.path.join(args.out, f'rpi{batch}.log')))
            size, since = -1, time.time()
            while proc.poll() is None:
                time.sleep(2)
                now = os.path.getsize(log) if os.path.exists(log) else 0
                if now != size:
                    size, since = now, time.time()
                elif time.time() - since > STALL_SECONDS:
                    proc.kill()			# hangs: counted as a crash below
                    proc.wait()
        if not os.path.exists(log):
            sys.exit(f'run-deqp: no log from {DEQP} (see {args.out}/batch{batch}.out)')
        got, crashed = parse_log(log)
        if not got and not crashed and not os.path.exists(TTY):
            # nothing ran because the RPi is missing: not the cases' fault
            if not reboot_rpi():
                write_results(os.path.join(args.out, 'results.txt'), cases, results)
                sys.exit('run-deqp: the RPi does not come back, giving up')
            batch += 1
            continue
        if proc.returncode != 0 and (got or crashed) and not reboot_rpi():
            write_results(os.path.join(args.out, 'results.txt'), cases, results)
            sys.exit('run-deqp: the RPi does not come back, giving up')
        results.update(got)
        if crashed:
            results[crashed] = 'Crash'
        done = set(results)
        rest = [c for c in remaining if c not in done]
        if len(rest) == len(remaining):
            # nothing ran: give up on the first one
            results[rest[0]] = 'Crash'
            rest = rest[1:]
        remaining = rest
        batch += 1

    write_results(os.path.join(args.out, 'results.txt'), cases, results)

    # summary per group (the 3rd level: dEQP-GLES2.functional.<group>)
    groups = collections.OrderedDict()
    for c in cases:
        g = '.'.join(c.split('.')[:3])
        groups.setdefault(g, collections.Counter())[results.get(c, 'Missing')] += 1
    total = collections.Counter()
    for g, n in groups.items():
        total.update(n)
        print(f'{g:55s} ' + ' '.join(f'{k} {v}' for k, v in sorted(n.items())))
    print(f'{"total":55s} ' + ' '.join(f'{k} {v}' for k, v in sorted(total.items())))
    print(f'run-deqp: {time.time() - t0:.0f} s, results in {args.out}/results.txt')


if __name__ == '__main__':
    main()
