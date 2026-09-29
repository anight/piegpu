#!/usr/bin/env python3
"""glslc - precompile a GLSL ES 1.00 program for the Pico GPU (docs/protocol.md 7.10).

Compiles a vertex and a fragment shader with Mesa's vc4 driver (the VideoCore
IV compiler, run on the host through its no-hardware DRM shim, built by
tools/glslc/build-mesa.sh) and writes a C header for the Pico with the
program blob (protocol/pgpu_program.h), the attribute locations, the uniform
storage offsets and the sampler indices.

usage:
  glslc.py -n NAME --vs FILE --fs FILE -a NAME:TYPE:SIZE [-a ...]
           [-v PRIM ...] -o OUT.h

  TYPE   float, short, short_norm, ubyte_norm, byte_norm, ubyte, byte,
         ushort, ushort_norm, fixed (docs/protocol.md 10.5)
  PRIM   triangles, lines, points (default: all three, as GL requires a
         program to draw every primitive type; -v limits the blob to some)

Attribute locations are in -a order. The attribute formats are the fast
path: the RPi converts arrays of other types and sizes. Each variant has
two fragment shader endings (plain, and blending / colour mask from
uniforms), so every blend state works with every program.
"""
import argparse, hashlib, json, math, os, re, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'devtools'))
import qpuasm
MESA_SRC = os.path.join(ROOT, 'third_party', 'mesa')
MESA_PREFIX = os.path.join(ROOT, 'third_party', 'mesa-install')
MESA_LIB = os.path.join(MESA_PREFIX, 'lib', 'x86_64-linux-gnu')
BUILD = os.path.join(HERE, 'build')

MAGIC = 0x31504750
MARKER_BASE = 1000000

# (protocol type, GL type name for the harness, normalized)
TYPES = {
    'float':       (0, 'float', 0),
    'short':       (1, 'short', 0),
    'short_norm':  (2, 'short', 1),
    'ubyte_norm':  (3, 'ubyte', 1),
    'byte_norm':   (4, 'byte', 1),
    'ubyte':       (5, 'ubyte', 0),
    'byte':        (6, 'byte', 0),
    'ushort':      (7, 'ushort', 0),
    'ushort_norm': (8, 'ushort', 1),
    'fixed':       (9, 'fixed', 0),
}
PRIMS = {'triangles': 0, 'lines': 1, 'points': 2}
VK_POINT_SIZE = 1 << 16
VERT_ATTRIB_GENERIC0 = 15	# Mesa's gl_vert_attrib: generic attribute 0 (inputs_read bit)
VK_TEXTURE_TARGET = 1 << 17
BLEND_PLAIN, BLEND_GENERIC = 0, 1	# fragment shader endings (protocol/pgpu_program.h)
U_BLEND = 35				# uniform kind: blend coefficient (data 0-47)
STAGES = {'fs': 0, 'vs': 1, 'cs': 2}

# GL uniform types: (rows, columns); float, int and bool types
GL_TYPE_SHAPE = {0x1406: (1, 1), 0x8B50: (2, 1), 0x8B51: (3, 1), 0x8B52: (4, 1),
                 0x1404: (1, 1), 0x8B53: (2, 1), 0x8B54: (3, 1), 0x8B55: (4, 1),
                 0x8B56: (1, 1), 0x8B57: (2, 1), 0x8B58: (3, 1), 0x8B59: (4, 1),
                 0x8B5A: (2, 2), 0x8B5B: (3, 3), 0x8B5C: (4, 4)}
FLOAT_TYPES = {0x1406, 0x8B50, 0x8B51, 0x8B52, 0x8B5A, 0x8B5B, 0x8B5C}
INT_TYPES = {0x1404, 0x8B53, 0x8B54, 0x8B55}
BOOL_TYPES = {0x8B56, 0x8B57, 0x8B58, 0x8B59}

# Mesa built-in state variables the RPi provides -> pgpu uniform kind
STATE_KINDS = {
    'state.FbWposYTransform': 32,	# gl_FragCoord
    'state.depth.range': 33,		# gl_DepthRange
    'state.FbPntcYTransform': 34,	# gl_PointCoord
}

# uniform kinds the RPi implements (protocol/pgpu_program.h)
SUPPORTED_KINDS = {
    'QUNIFORM_CONSTANT', 'QUNIFORM_UNIFORM',
    'QUNIFORM_VIEWPORT_X_SCALE', 'QUNIFORM_VIEWPORT_Y_SCALE',
    'QUNIFORM_VIEWPORT_Z_OFFSET', 'QUNIFORM_VIEWPORT_Z_SCALE',
    'QUNIFORM_TEXTURE_CONFIG_P0', 'QUNIFORM_TEXTURE_CONFIG_P1',
    'QUNIFORM_TEXTURE_CONFIG_P2', 'QUNIFORM_TEXTURE_FIRST_LEVEL',
    'QUNIFORM_UNIFORMS_ADDRESS', 'QUNIFORM_STENCIL', 'QUNIFORM_UBO0_ADDR',
}
# uniform kinds whose data is a sampler index
TEXTURE_KINDS = {
    'QUNIFORM_TEXTURE_CONFIG_P0', 'QUNIFORM_TEXTURE_CONFIG_P1',
    'QUNIFORM_TEXTURE_CONFIG_P2', 'QUNIFORM_TEXTURE_FIRST_LEVEL',
}
EXPECTED_KINDS = {
    'QUNIFORM_CONSTANT': 0, 'QUNIFORM_UNIFORM': 1,
    'QUNIFORM_VIEWPORT_X_SCALE': 2, 'QUNIFORM_VIEWPORT_Y_SCALE': 3,
    'QUNIFORM_VIEWPORT_Z_OFFSET': 4, 'QUNIFORM_VIEWPORT_Z_SCALE': 5,
    'QUNIFORM_TEXTURE_CONFIG_P0': 6, 'QUNIFORM_TEXTURE_CONFIG_P1': 7,
    'QUNIFORM_TEXTURE_CONFIG_P2': 8, 'QUNIFORM_TEXTURE_FIRST_LEVEL': 9,
    'QUNIFORM_UNIFORMS_ADDRESS': 24, 'QUNIFORM_STENCIL': 22, 'QUNIFORM_UBO0_ADDR': 11,
}


def fail(msg):
    sys.exit(f'glslc: {msg}')


def uniform_kinds():
    """Mesa's enum quniform_contents, in order."""
    src = open(os.path.join(MESA_SRC, 'src/gallium/drivers/vc4/vc4_qir.h')).read()
    body = re.search(r'enum quniform_contents \{(.*?)\n\};', src, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    names = re.findall(r'\b(QUNIFORM_[A-Z0-9_]+)\b', body)
    kinds = {n: i for i, n in enumerate(names)}
    for n, v in EXPECTED_KINDS.items():
        if kinds.get(n) != v:
            fail(f'{n} is {kinds.get(n)} in this Mesa, protocol/pgpu_program.h expects {v}')
    return {v: n for n, v in kinds.items()}


def build_harness():
    exe = os.path.join(BUILD, 'harness')
    src = os.path.join(HERE, 'harness.c')
    if not os.path.exists(os.path.join(MESA_LIB, 'libvc4_noop_drm_shim.so')):
        fail('host Mesa not built: run tools/glslc/build-mesa.sh')
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        # parallel builds run several glslc at once: build privately, then
        # rename (atomic)
        tmp = f'{exe}.{os.getpid()}'
        subprocess.check_call(['gcc', '-O1', '-Wall', '-o', tmp, src,
                               '-I' + os.path.join(MESA_PREFIX, 'include'),
                               '-L' + MESA_LIB, '-Wl,-rpath-link,' + MESA_LIB,
                               '-lEGL', '-lGLESv2'])
        os.replace(tmp, exe)
    return exe


def vpm_writes(code):
    """Instructions writing the VPM (write address 48 on the add or mul unit)."""
    n = 0
    for w in code:
        if w >> 60 == 15:		# branch
            continue
        n += (((w >> 38) & 63) == 48) + (((w >> 32) & 63) == 48)
    return n


# ---- fragment shader endings -------------------------------------------------
#
# The fragment shaders are compiled for an RGBA8888 window framebuffer with
# blending disabled: they end by writing the colour, packed BGRA, to
# tlb_color_all (conditionally, for discard), followed by the program end.
# That write is redirected into an accumulator, and one of two endings is
# appended that writes the tile buffer in the RPi's order (R in byte 0):
#   plain:   reorder the bytes (blending off, colour mask all on)
#   generic: blending and colour mask from 48 uniforms: per channel c
#            F = k0 + k1 As + k2 Ad + k3 Sc + k4 Dc + k5 min (As, 1 - Ad)
#            for the source (k0-k5) and the destination (k6-k11) factor,
#            result = Sc Fs + Dc Fd (the equation's sign is in the k),
#            channels R, G, B, A; D is the tile buffer's colour (colour load)

W_TLB_COLOR_ALL, W_NOP = 46, 39


def fs_ending(code, stream, generic):
    ends = [i for i, w in enumerate(code)
            if w >> 60 != 15 and W_TLB_COLOR_ALL in ((w >> 38) & 63, (w >> 32) & 63)]
    if len(ends) != 1:
        fail(f'fragment shader: {len(ends)} colour writes, expected one')
    i = ends[0]
    w = code[i]
    for t in code[i + 1:]:
        if t >> 60 in (14, 15) or ((t >> 38) & 63, (t >> 32) & 63) != (W_NOP, W_NOP):
            fail('fragment shader: unexpected code after the colour write')
    sig = w >> 60
    if sig not in (1, 13, 14):		# none, small immediate, load immediate
        fail(f'fragment shader: signal {sig} on the colour write')

    # redirect the write into r0 (r1 if the other unit writes r0)
    add_w, mul_w = (w >> 38) & 63, (w >> 32) & 63
    on_add = add_w == W_TLB_COLOR_ALL
    other = mul_w if on_add else add_w
    acc = 33 if other == 32 else 32
    shift = 38 if on_add else 32
    cond = (w >> 49) & 7 if on_add else (w >> 46) & 7
    if sig == 14:			# load immediate: both conditions apply
        cond = (w >> 49) & 7
    w = (w & ~(63 << shift)) | (acc << shift)
    cond_name = {v: k for k, v in qpuasm.CONDS.items()}[cond]
    src = 'r%d' % (acc - 32)

    p = qpuasm.Program('ending')
    uniforms = []
    if not generic:
        # BGRA -> RGBA: bytes 2, 1, 0, 3 as floats, packed again
        p.mov('ra0', src)
        for byte, pack in (('8c', '8a'), ('8b', '8b'), ('8a', '8c'), ('8d', '8d')):
            p.fmax('r1', 'ra0.' + byte, 'ra0.' + byte)
            p.mov('r3.' + pack, 'r1')
    else:
        p.mov('ra0', src)			# source, BGRA
        p.sig('colorload')			# r4: tile buffer colour, R in byte 0
        S = {'r': 'ra1', 'g': 'ra2', 'b': 'ra3', 'a': 'ra4'}
        D = {'r': 'rb1', 'g': 'rb2', 'b': 'rb3', 'a': 'rb4'}
        for ch, byte in (('r', '8c'), ('g', '8b'), ('b', '8a'), ('a', '8d')):
            p.fmax(S[ch], 'ra0.' + byte, 'ra0.' + byte)
        for ch, byte in (('r', '8a'), ('g', '8b'), ('b', '8c'), ('a', '8d')):
            p.fmax(D[ch], 'r4.' + byte, 'r4.' + byte)
        p.mov('r1', D['a'])			# (1.0 and rb4 would both need the B port)
        p.fsub('r0', 1.0, 'r1')
        p.fmin('ra5', S['a'], 'r0')		# SRC_ALPHA_SATURATE
        k = 0
        for ch, pack in (('r', '8a'), ('g', '8b'), ('b', '8c'), ('a', '8d')):
            terms = [S['a'], D['a'], S[ch], D[ch], 'ra5']
            for side in (0, 1):
                p.mov('r0', p.unif('k%d' % k))
                uniforms.append(k)
                k += 1
                tmp = ('r1', 'r2')
                for n, x in enumerate(terms):
                    p.fmul(tmp[n % 2], p.unif('k%d' % k), x)
                    uniforms.append(k)
                    k += 1
                    p.fadd('r0', 'r0', tmp[n % 2])
                if side == 0:
                    p.fmul('ra6', 'r0', S[ch])	# Sc Fs
                else:
                    p.fmul('r1', 'r0', D[ch])	# Dc Fd
                    p.fadd('r0', 'ra6', 'r1')
            p.mov('r3.' + pack, 'r0')
    p.alu('mov', 'tlbc', 'r3', cond=cond_name)
    ending = p.encode()

    new_code = code[:i] + [w] + ending + code[i + 1:]
    new_stream = [list(u) for u in stream] + [[U_BLEND, n, 0] for n in uniforms]
    return new_code, new_stream


def main():
    ap = argparse.ArgumentParser(description='Precompile a GLSL ES program for the Pico GPU')
    ap.add_argument('-n', '--name', default='program', help='C identifier prefix')
    ap.add_argument('--vs')
    ap.add_argument('--fs')
    ap.add_argument('-a', '--attrib', action='append', default=[], help='NAME:TYPE:SIZE')
    ap.add_argument('-v', '--variant', action='append', default=[], help='PRIM')
    ap.add_argument('-o', '--output', help='C header')
    ap.add_argument('--pgl', help='the program for pgl\'s run-time compiler (text)')
    ap.add_argument('--check', action='store_true',
                    help='only compile --vs and/or --fs; print the info log')
    ap.add_argument('--bind', action='append', default=[],
                    help='NAME=LOCATION: without -a, the attributes are found in the '
                         'program; these get the given locations, others the lowest free')
    ap.add_argument('--cache', help='directory of compiled programs (--pgl)')
    ap.add_argument('--keep', action='store_true', help='keep the dump (for debugging)')
    args = ap.parse_args()

    if args.check:
        if args.cache:
            cached = os.path.join(args.cache, cache_key(args) + '.check')
            if os.path.exists(cached):
                status, _, log = open(cached).read().partition('\n')
                print(log, end='')
                sys.exit(int(status))
            status, log = check(args)
            os.makedirs(args.cache, exist_ok=True)
            tmp = f'{cached}.{os.getpid()}'
            open(tmp, 'w').write(f'{status}\n{log}')
            os.replace(tmp, cached)
        else:
            status, log = check(args)
        print(log, end='')
        sys.exit(status)
    if not args.vs or not args.fs or not (args.output or args.pgl):
        fail('need --vs, --fs and -o or --pgl')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.name):
        fail('the name must be a C identifier')

    if args.pgl and args.cache:
        key = cache_key(args)
        cached = os.path.join(args.cache, key + '.pgl')
        if os.path.exists(cached):
            shutil.copyfile(cached, args.pgl)
            return
    compile_program(args)
    if args.pgl and args.cache:
        os.makedirs(args.cache, exist_ok=True)
        tmp = f'{cached}.{os.getpid()}'
        shutil.copyfile(args.pgl, tmp)
        os.replace(tmp, cached)


def cache_key(args):
    h = hashlib.sha256()
    for path in ([p for p in (args.vs, args.fs) if p] + [__file__, os.path.join(HERE, 'harness.c'),
                 os.path.join(HERE, '..', '..', 'patches', 'mesa-vc4-dump.patch'),
                 os.path.join(HERE, '..', '..', 'devtools', 'qpuasm.py')]):
        h.update(open(path, 'rb').read())
        h.update(b'\0')
    h.update(repr((bool(args.vs), bool(args.fs), args.attrib, sorted(args.bind), args.variant)).encode())
    return h.hexdigest()


def harness_env(dump, no_unroll=False):
    extra = {'PGPU_NO_UNROLL': '1'} if no_unroll else {}	# see the Mesa patch
    return dict(os.environ, **extra,
                LD_LIBRARY_PATH=MESA_LIB,
                LD_PRELOAD=os.path.join(MESA_LIB, 'libvc4_noop_drm_shim.so'),
                MESA_LOADER_DRIVER_OVERRIDE='vc4',
                MESA_SHADER_CACHE_DISABLE='true',
                PGPU_VC4_DUMP=dump)


def run_harness(job_lines, tmp, no_unroll=False):
    job = os.path.join(tmp, 'job')
    dump = os.path.join(tmp, 'dump.jsonl')
    with open(job, 'w') as f:
        f.write(''.join(l + '\n' for l in job_lines))
    r = subprocess.run([build_harness(), job, dump], env=harness_env(dump, no_unroll),
                       capture_output=True, text=True)
    return r, r.stdout.splitlines(), dump


def check(args):
    """compile only: (0 if the shaders compile, else 1; the info log)"""
    if not args.vs and not args.fs:
        fail('--check needs --vs and/or --fs')
    with tempfile.TemporaryDirectory() as tmp:
        lines = ['check']
        if args.vs:
            lines.append(f'vs {os.path.abspath(args.vs)}')
        if args.fs:
            lines.append(f'fs {os.path.abspath(args.fs)}')
        r, out, _ = run_harness(lines, tmp)
    log = [l[6:].split(': ', 1)[-1] for l in out if l.startswith('error ')]
    if r.returncode and not log:
        log = [r.stderr.strip()]
    return (0 if r.returncode == 0 and 'compiled' in out else 1), ''.join(l + '\n' for l in log)


# attribute GL types: (locations, components per location)
GL_ATTRIB_SHAPES = {0x1406: (1, 1), 0x8B50: (1, 2), 0x8B51: (1, 3), 0x8B52: (1, 4),
                    0x8B5A: (2, 2), 0x8B5B: (3, 3), 0x8B5C: (4, 4)}


def auto_attribs(args):
    """the program's attributes, at the bound locations or the lowest free
    ones: one entry per location (names, 'float', size); several names at one
    location alias (GL ES 2.0 2.10.4), a matrix takes a location per column"""
    binds = {}
    for b in args.bind:
        name, _, loc = b.partition('=')
        if not loc.isdigit():
            fail(f'bad binding {b!r}, expected NAME=LOCATION')
        binds[name] = int(loc)
    # the probe links with the bindings (the harness binds its attribute
    # lines in order: "a,b" aliases, "-" none): unbound, more attributes than
    # locations would fail although aliasing is legal (GL ES 2.0 2.10.4)
    bound = [','.join(n for n, l in binds.items() if l == loc) or '-'
             for loc in range(max(binds.values()) + 1)] if binds else []
    with tempfile.TemporaryDirectory() as tmp:
        r, out, _ = run_harness([f'vs {os.path.abspath(args.vs)}', f'fs {os.path.abspath(args.fs)}',
                                 'probe'] + [f'attrib {names} float 1 0' for names in bound], tmp)
    errors = [l[6:] for l in out if l.startswith('error ')]
    if errors or r.returncode:
        fail('\n'.join(errors or [r.stderr.strip()]))
    active = []
    for l in out:
        p = l.split()
        if p[0] == 'attribute':
            gltype, size = int(p[2], 16), int(p[3])
            if gltype not in GL_ATTRIB_SHAPES or size != 1:
                fail(f'attribute {p[1]}: type {gltype:#x} is not supported')
            active.append((p[1], *GL_ATTRIB_SHAPES[gltype]))
    names, sizes = {}, {}		# location -> [names bound there], components
    def place(name, loc, columns, size):
        for c in range(columns):
            if c == 0:
                names.setdefault(loc, []).append(name)
            else:
                names.setdefault(loc + c, [])
            sizes[loc + c] = max(sizes.get(loc + c, 1), size)
    for name, columns, size in active:
        if name in binds:
            if binds[name] + columns > 8:
                fail(f'attribute {name}: location {binds[name]} is not available')
            place(name, binds[name], columns, size)
    for name, columns, size in active:
        if name not in binds:
            loc = next((l for l in range(9 - columns)
                        if all(l + c not in sizes for c in range(columns))), None)
            if loc is None:
                fail('not enough attribute locations')
            place(name, loc, columns, size)
    if not sizes:
        sizes[0] = 1			# a program needs one attribute
    return [(','.join(names.get(l, [])) or '-', 'float', sizes.get(l, 1))
            for l in range(max(sizes) + 1)]


def shaders_per_variant(records):
    """the shaders used by each variant draw (fs, vs, cs); a draw that didn't
    re-emit its shader state uses the previous one"""
    per_variant = []
    current = {}
    pending = None
    for o in records:
        if 'param' in o or 'uniform_param' in o:
            continue
        if 'variant' in o:
            if pending is not None:
                per_variant.append(dict(current))
            pending = o['variant']
            continue
        current[o['stage']] = o
    if pending is not None:
        per_variant.append(dict(current))
    return per_variant


def compile_program(args):
    kind_names = uniform_kinds()

    attribs = []
    for a in args.attrib:
        try:
            name, t, size = a.split(':')
            size = int(size)
        except ValueError:
            fail(f'bad attribute {a!r}, expected NAME:TYPE:SIZE')
        if t not in TYPES or not 1 <= size <= 4:
            fail(f'bad attribute type or size in {a!r}')
        attribs.append((name, t, size))
    if not args.attrib:
        attribs = auto_attribs(args)
    if not 1 <= len(attribs) <= 8:
        fail('a program needs 1 to 8 attributes')

    variants = []
    for v in args.variant or ['triangles', 'lines', 'points']:
        if v not in PRIMS:
            fail(f'bad variant {v!r}, expected triangles, lines or points')
        variants.append(v)
    if 'points' in variants:
        # gl_PointCoord's origin is compiled into the fragment shader: a second
        # one for texture targets (rows bottom up)
        variants.append('points_texture')

    def compile_variants(no_unroll):
        with tempfile.TemporaryDirectory() as tmp:
            r, out, dump = run_harness(
                [f'vs {os.path.abspath(args.vs)}', f'fs {os.path.abspath(args.fs)}']
                + [f'attrib {name} {TYPES[t][1]} {size} {TYPES[t][2]}' for name, t, size in attribs]
                + [f'variant {prim}' for prim in variants], tmp, no_unroll)
            errors = [l[6:] for l in out if l.startswith('error ')]
            if errors or r.returncode:
                fail('\n  '.join(['compile failed:'] + (errors or [r.stderr.strip()])))
            records = [json.loads(l) for l in open(dump)] if os.path.exists(dump) else []
            return r, out, records

    def compiled_all(records):
        per_variant = shaders_per_variant(records)
        return len(per_variant) == len(variants) and all(len(v) == 3 for v in per_variant)

    # the driver unrolls loops (up to 32 iterations); when the unrolled shader
    # fails register allocation, the loop may still fit as a loop
    r, out, records = compile_variants(False)
    if not compiled_all(records):
        r, out, records = compile_variants(True)
    driver_log = r.stderr.strip()
    if args.keep:
        open((args.output or args.pgl) + '.dump.jsonl', 'w').write(''.join(json.dumps(o) + '\n' for o in records))

    # harness output: uniforms, markers, samplers
    uniforms = {}		# index -> (name, gl type, array size)
    uniform_arrays = set()	# names declared as arrays
    markers = {}		# marker -> (uniform index, element, component)
    samplers = {}		# unit (= harness order) -> name
    active_attribs = {}		# name -> (gl type, size)
    for l in out:
        p = l.split()
        if p[0] == 'attribute':
            active_attribs[p[1]] = (int(p[2], 16), int(p[3]))
        elif p[0] == 'uniform':
            uniforms[int(p[1])] = (p[2], int(p[3], 16), int(p[4]))
            if len(p) > 5 and p[5] == '1':
                uniform_arrays.add(p[2])		# GL names it "name[0]"
        elif p[0] == 'marker':
            markers[int(p[1])] = tuple(int(x) for x in p[2:5])
        elif p[0] == 'sampler':
            samplers[int(p[2])] = p[1]

    # built-in state variables: constant buffer word -> (name, component)
    state_words = {'v': {}, 'f': {}}
    for o in records:
        if 'param' in o:
            for k in range(o['size']):
                state_words[o['stage']][o['offset'] + k] = (o['param'], k)

    # where Mesa put the uniforms: one parameter per array element and matrix
    # column (in order), components in consecutive words. Bool uniforms can't
    # carry markers (Mesa stores true as ~0, the driver has native integers):
    # their words come from this layout, which the markers of the other
    # uniforms check.
    uniform_params = {'v': {}, 'f': {}}	# name -> [offset of each parameter]
    for o in records:
        if 'uniform_param' in o:
            offsets = uniform_params[o['stage']].setdefault(o['uniform_param'], [])
            if o['offset'] not in offsets:
                offsets.append(o['offset'])
    layout_words = {'v': {}, 'f': {}}	# constant buffer word -> (uniform, element, component)
    for u, (name, gltype, size) in uniforms.items():
        rows, cols = GL_TYPE_SHAPE.get(gltype, (0, 0))
        for stage in ('v', 'f'):
            offsets = sorted(uniform_params[stage].get(name, []))
            if not offsets:
                continue
            if len(offsets) != size * cols:
                fail(f'uniform {name}: {len(offsets)} constant buffer parameters, expected {size * cols}')
            for e in range(size):
                for col in range(cols):
                    for row in range(rows):
                        layout_words[stage][offsets[e * cols + col] + row] = (u, e, col * rows + row)

    per_variant = shaders_per_variant(records)
    if len(per_variant) != len(variants) or any(len(v) != 3 for v in per_variant):
        fail('the driver did not compile every variant (see --keep)'
             + (':\n' + driver_log[-2000:] if driver_log else ''))

    # shader table (deduplicated)
    shaders, shader_index = [], {}
    uniform_slots = {'v': {}, 'f': {}}	# constant buffer word -> (uniform, element, component)
    sampler_units = set()	# program sampler indices the shaders read (= harness units)
    indirect = set()		# stages that read their constant buffer through the TMU
    kinds_used = set()

    def shader_key(o):
        return (o['stage'], tuple(o['code']), tuple((k, d) for k, d, _ in o['stream']),
                o['threaded'], o['num_inputs'], o['vattrs_live'], tuple(o['vattr_offsets']))

    variant_words = []
    n_variants = 0
    for prim, used in zip(variants, per_variant):
        idx = []
        for stage in ('fs', 'vs', 'cs'):
            o = used[stage]
            o['stream'] = []		# (kind, data) as the RPi gets them
            stage_units = {}		# Mesa's sampler index in this stage -> harness unit
            for kind, data, value in o['uniforms']:
                o['stream'].append([kind, data, value])
                kname = kind_names.get(kind, f'#{kind}')
                kinds_used.add(kname)
                if kname not in SUPPORTED_KINDS:
                    fail(f'the {stage.upper()} uses {kname}, which the RPi does not implement')
                if kname == 'QUNIFORM_UNIFORM':
                    f = struct.unpack('<f', struct.pack('<I', value))[0]
                    layout = layout_words['f' if stage == 'fs' else 'v'].get(data)
                    m = None
                    if math.isfinite(f) and f == int(f) and int(f) - MARKER_BASE in markers:
                        m = int(f) - MARKER_BASE
                        if uniforms[markers[m][0]][1] not in FLOAT_TYPES:
                            fail(f'{stage} uniform word {data}: a float marker for a non-float uniform')
                    elif value - MARKER_BASE in markers:
                        m = value - MARKER_BASE	# int uniforms are integers (native integers)
                        if uniforms[markers[m][0]][1] not in INT_TYPES:
                            fail(f'{stage} uniform word {data}: an integer marker for a non-int uniform')
                    elif layout is not None and uniforms[layout[0]][1] in BOOL_TYPES:
                        if value != 0xFFFFFFFF:
                            fail(f'{stage} uniform word {data}: bool uniform word holds {value:#x}')
                        m = next(k for k, v in markers.items() if v == layout)
                    if m is not None and layout != markers[m]:
                        fail(f'{stage} uniform word {data}: Mesa\'s parameter layout says {layout}, '
                             f'the marker {markers[m]}')
                    if m is None:
                        param = state_words['f' if stage == 'fs' else 'v'].get(data)
                        if param is None:
                            fail(f'{stage} uniform word {data} (value {value:#x}) matches no uniform')
                        if param[0] not in STATE_KINDS:
                            fail(f'the {stage.upper()} reads built-in GL state {param[0]}, '
                                 f'which the RPi does not provide')
                        o['stream'][-1][:2] = [STATE_KINDS[param[0]], param[1]]
                        continue
                    slots = uniform_slots['f' if stage == 'fs' else 'v']
                    if slots.setdefault(data, markers[m]) != markers[m]:
                        fail(f'{stage} uniform word {data} is used for two uniforms')
                elif kname == 'QUNIFORM_UBO0_ADDR':
                    # dynamic indexing of a uniform array: the shader reads the
                    # constant buffer through the TMU (data = byte offset)
                    indirect.add('f' if stage == 'fs' else 'v')
                elif kname == 'QUNIFORM_TEXTURE_CONFIG_P1':
                    width = (value >> 8) & 0x7FF
                    unit = {4 << n: n for n in range(8)}.get(width)
                    if unit is None:
                        fail(f'cannot identify sampler {data} (width {width})')
                    if stage_units.setdefault(data, unit) != unit:
                        fail(f'{stage} sampler {data} reads two units')
            # Mesa numbers each stage's samplers apart (VS and FS both from
            # 0); the RPi has one table per program: the index is the
            # sampler's harness unit, which names each element of each
            # sampler uniform once
            for e in o['stream']:
                if kind_names.get(e[0]) in TEXTURE_KINDS:
                    # data: the sampler in the low 16 bits (P2: explicit LOD above)
                    unit = stage_units.get(e[1] & 0xFFFF)
                    if unit is None:
                        fail(f'{stage} texture uniform for sampler {e[1] & 0xFFFF} without its P1')
                    e[1] = e[1] & ~0xFFFF | unit
                    sampler_units.add(unit)
            if stage == 'fs':
                continue
            k = shader_key(o)
            if k not in shader_index:
                shader_index[k] = len(shaders)
                shaders.append(o)
            idx.append(shader_index[k])

        cs = used['cs']
        # CS output: Xc, Yc, Zc, Wc, Xs|Ys, Zs, 1/Wc [, point size]
        point_size = prim.startswith('points') and vpm_writes([int(w, 16) for w in cs['code']]) == 8

        # the fragment shader with its two endings
        fs = used['fs']
        for blend in (BLEND_PLAIN, BLEND_GENERIC):
            code, stream = fs_ending([int(w, 16) for w in fs['code']], fs['stream'],
                                     blend == BLEND_GENERIC)
            o = dict(fs, code=['%016x' % w for w in code], stream=stream)
            k = shader_key(o)
            if k not in shader_index:
                shader_index[k] = len(shaders)
                shaders.append(o)
            key = (  PRIMS[prim.split('_')[0]] | blend << 8 | (VK_POINT_SIZE if point_size else 0)
                   | (VK_TEXTURE_TARGET if prim == 'points_texture' else 0))
            variant_words += [key, shader_index[k] | idx[0] << 8 | idx[1] << 16]
            n_variants += 1

    # every active sampler element has an index, read or not (compiled
    # away, it still holds a unit: glUniform1i, glGetUniform)
    sampler_names = dict(samplers)
    n_samplers = len(samplers)
    if n_samplers > 8:
        fail('too many samplers')
    sampler_index = {n: s for s, n in sampler_names.items()}

    # a stage that indexes uniform arrays dynamically reads any word of them:
    # all its uniforms' words must be in the storage
    for stage in indirect:
        for word, slot in layout_words[stage].items():
            if uniform_slots[stage].setdefault(word, slot) != slot:
                fail(f'{stage} uniform word {word} is used for two uniforms')

    # the program's uniform storage: the vertex constant buffer, then the
    # fragment one (FS uniform stream offsets are moved up by fs_base)
    fs_base = max(uniform_slots['v']) + 1 if uniform_slots['v'] else 0
    uniform_words = fs_base + (max(uniform_slots['f']) + 1 if uniform_slots['f'] else 0)
    if uniform_words > 4096:
        fail('the uniform storage is too large')

    # blob
    blob = [MAGIC, 0, len(attribs) | n_samplers << 8 | n_variants << 16 | len(shaders) << 24,
            uniform_words, 0, 0, 0, 0]
    for name, t, size in attribs:
        blob.append(TYPES[t][0] | size << 8)
    blob += variant_words
    for o in shaders:
        code = [int(w, 16) for w in o['code']]
        stage = STAGES[o['stage']]
        if stage == 0:
            info = (1 if o['threaded'] else 0) | o['num_inputs'] << 8
            offsets = [0, 0]
        else:
            # Mesa numbers the attributes the VS reads (vertex elements) in
            # the order of their locations: element k is the k-th set bit of
            # the VS inputs; the blob and the RPi use GL locations
            locs = [b - VERT_ATTRIB_GENERIC0 if b >= VERT_ATTRIB_GENERIC0 else 0
                    for b in range(64) if o['inputs_read'] >> b & 1]
            if not locs:
                locs = [0]	# vc4 reads a dummy attribute when there are none
            vo = o['vattr_offsets']
            live, loc_offsets = 0, [0] * 8
            for k, loc in enumerate(locs):
                if loc >= 8:
                    fail(f'attribute location {loc}')
                if o['vattrs_live'] >> k & 1:
                    live |= 1 << loc
                loc_offsets[loc] = vo[k]
            if o['vattrs_live'] >> len(locs):
                fail('the vertex shader reads more attributes than it has inputs')
            info = live | vo[8] << 8
            offsets = [loc_offsets[0] | loc_offsets[1] << 8 | loc_offsets[2] << 16 | loc_offsets[3] << 24,
                       loc_offsets[4] | loc_offsets[5] << 8 | loc_offsets[6] << 16 | loc_offsets[7] << 24]
        blob += [len(code) | len(o['stream']) << 16, info | stage << 28] + offsets
        for w in code:
            blob += [w & 0xFFFFFFFF, w >> 32]
        for kind, data, _ in o['stream']:
            if stage == 0 and kind == 1:	# QUNIFORM_UNIFORM
                data += fs_base
            if stage == 0 and kind == 11:	# QUNIFORM_UBO0_ADDR (bytes)
                data += fs_base * 4
            blob += [kind, data]
    blob[1] = len(blob)

    # the uniforms: (name, GL type, size, scalars per element, sampler index,
    # storage words: VS then FS, 0xffff = not used there)
    table = []
    for u in sorted(uniforms):
        name, gltype, size = uniforms[u]
        if name in samplers.values():
            table.append((name, gltype, size, 0, sampler_index.get(name, -1), None, name in uniform_arrays))
            continue
        comps = sum(1 for m in markers.values() if m[0] == u and m[1] == 0)
        offs = []
        for stage, base in (('v', 0), ('f', fs_base)):
            for e in range(size):
                for c in range(comps):
                    word = next((w for w, m in uniform_slots[stage].items() if m == (u, e, c)), None)
                    offs.append(0xFFFF if word is None else base + word)
        table.append((name, gltype, size, comps, -1, offs, name in uniform_arrays))
    # the active attributes (GL lists only those)
    active = [(i, n, *active_attribs[n]) for i, (name, t, size) in enumerate(attribs)
              for n in name.split(',') if n in active_attribs]

    if args.output:
        write_header(args, blob, attribs, variants, shaders, table, active, sampler_names)
    if args.pgl:
        lines = [f'program {len(blob)}']
        for i in range(0, len(blob), 8):
            lines.append('blob ' + ' '.join(f'{w:x}' for w in blob[i:i + 8]))
        for i, name, gltype, gsize in active:
            lines.append(f'attrib {name} {i} {gltype:x} {gsize}')
        for name, gltype, size, comps, sampler, offs, array in table:
            lines.append(f'uniform {name} {gltype:x} {size} {comps} {sampler} {int(array)} '
                         + ' '.join(str(o) for o in offs or []))
        lines.append('end')
        open(args.pgl, 'w').write('\n'.join(lines) + '\n')

    print(f'glslc: {args.output or args.pgl}: {len(blob)} words, {n_variants} variants, '
          f'{len(shaders)} shaders, uniform storage {uniform_words} words, '
          f'{len(sampler_names)} samplers', file=sys.stderr if args.pgl else sys.stdout)


def write_header(args, blob, attribs, variants, shaders, table, active, sampler_names):
    ident = args.name
    lines = [f'/* Generated by tools/glslc/glslc.py from {os.path.basename(args.vs)}, '
             f'{os.path.basename(args.fs)} - do not edit */',
             f'#ifndef {ident.upper()}_PROGRAM_H', f'#define {ident.upper()}_PROGRAM_H', '',
             '#include <stdint.h>', '',
             f'/* variants: {", ".join(variants)} (plain and blending endings); '
             f'{len(shaders)} shaders, {sum(len(o["code"]) for o in shaders)} instructions */',
             f'#define {ident.upper()}_WORDS {len(blob)}',
             f'static const uint32_t {ident}_program[{len(blob)}] =', '{']
    for i in range(0, len(blob), 8):
        lines.append('\t' + ', '.join(f'0x{w:08x}' for w in blob[i:i + 8]) + ',')
    lines += ['};', '', '/* attribute locations (and the array formats they need) */']
    for i, (name, t, size) in enumerate(attribs):
        for n in name.split(','):
            if n != '-':
                lines.append(f'#define {ident.upper()}_{cname(n).upper()} {i}\t/* {t} x {size} */')
    lines += ['', '/* uniforms: the storage word of each scalar (element-major, column-major)',
              '   for the vertex shaders, then for the fragment shader; 0xffff = not used',
              '   there. See pgpu_program_uniform (). */']
    for name, gltype, size, comps, sampler, offs, array in table:
        if offs is not None:
            lines.append(f'static const uint16_t {ident}_{cname(name)}[{len(offs)}] = '
                         f'{{{", ".join(str(o) for o in offs)}}};')
    lines += ['', '/* samplers: index for pgpu_program_sampler () (default texture unit = index) */']
    for s in sorted(sampler_names):
        lines.append(f'#define {ident.upper()}_{cname(sampler_names[s]).upper()} {s}')

    # names and types for the GL layer (gles/pgl.c)
    lines += ['', '/* for glShaderBinary and glProgramBinaryOES (gles/pgl.h) */',
              '#include "pgpu_program_info.h"', '',
              f'static const pgpu_attrib_info_t {ident}_attribs[] =', '{']
    for i, name, gltype, gsize in active:
        lines.append(f'\t{{"{name}", {i}, 0x{gltype:04x}, {gsize}}},')
    if not active:
        lines.append('\t{0}')
    lines += ['};', '', f'static const pgpu_uniform_info_t {ident}_uniforms[] =', '{']
    for name, gltype, size, comps, sampler, offs, array in table:
        ref = f'{ident}_{cname(name)}' if offs is not None else '0'
        lines.append(f'\t{{"{name}", 0x{gltype:04x}, {size}, {comps}, {sampler}, {ref}, {int(array)}}},')
    if not table:
        lines.append('\t{0}')
    lines += ['};', '',
              f'static const pgpu_program_info_t {ident}_info =', '{',
              f'\tPGPU_PROGRAM_INFO_MAGIC, {ident}_program, {len(blob)},',
              f'\t{ident}_attribs, {len(active)}, {ident}_uniforms, {len(table)}', '};',
              '', '#endif', '']
    open(args.output, 'w').write('\n'.join(lines))


def cname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


if __name__ == '__main__':
    main()
