#!/usr/bin/env python3
"""glslc - precompile a GLSL ES 1.00 program for the Pico GPU (docs/protocol.md 7.10).

Compiles a vertex and a fragment shader with Mesa's vc4 driver (the VideoCore
IV compiler, run on the host through its no-hardware DRM shim, built by
tools/glslc/build-mesa.sh) and writes a C header for the Pico with the
program blob (protocol/pgpu_program.h), the attribute locations, the uniform
storage offsets and the sampler indices.

usage:
  glslc.py -n NAME --vs FILE --fs FILE -a NAME:TYPE:SIZE [-a ...]
           [-v PRIM:BLEND ...] -o OUT.h

  TYPE   float, short, short_norm, ubyte_norm, byte_norm, ubyte, byte,
         ushort, ushort_norm (docs/protocol.md 10.5)
  PRIM   triangles, lines, points
  BLEND  none, alpha, add, premul, multiply
  Default variants: triangles:none triangles:alpha.

The attribute formats are compiled into the vertex shaders: arrays used with
the program must have exactly these types and sizes.
"""
import argparse, json, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
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
}
PRIMS = {'triangles': 0, 'lines': 1, 'points': 2}
BLENDS = {'none': 0, 'alpha': 1, 'add': 2, 'premul': 3, 'multiply': 4}
STAGES = {'fs': 0, 'vs': 1, 'cs': 2}

# Mesa built-in state variables the Zero provides -> pgpu uniform kind
STATE_KINDS = {
    'state.FbWposYTransform': 32,	# gl_FragCoord
    'state.depth.range': 33,		# gl_DepthRange
    'state.FbPntcYTransform': 34,	# gl_PointCoord
}

# uniform kinds the Zero implements (protocol/pgpu_program.h)
SUPPORTED_KINDS = {
    'QUNIFORM_CONSTANT', 'QUNIFORM_UNIFORM',
    'QUNIFORM_VIEWPORT_X_SCALE', 'QUNIFORM_VIEWPORT_Y_SCALE',
    'QUNIFORM_VIEWPORT_Z_OFFSET', 'QUNIFORM_VIEWPORT_Z_SCALE',
    'QUNIFORM_TEXTURE_CONFIG_P0', 'QUNIFORM_TEXTURE_CONFIG_P1',
    'QUNIFORM_TEXTURE_CONFIG_P2', 'QUNIFORM_TEXTURE_FIRST_LEVEL',
    'QUNIFORM_UNIFORMS_ADDRESS',
}
EXPECTED_KINDS = {
    'QUNIFORM_CONSTANT': 0, 'QUNIFORM_UNIFORM': 1,
    'QUNIFORM_VIEWPORT_X_SCALE': 2, 'QUNIFORM_VIEWPORT_Y_SCALE': 3,
    'QUNIFORM_VIEWPORT_Z_OFFSET': 4, 'QUNIFORM_VIEWPORT_Z_SCALE': 5,
    'QUNIFORM_TEXTURE_CONFIG_P0': 6, 'QUNIFORM_TEXTURE_CONFIG_P1': 7,
    'QUNIFORM_TEXTURE_CONFIG_P2': 8, 'QUNIFORM_TEXTURE_FIRST_LEVEL': 9,
    'QUNIFORM_UNIFORMS_ADDRESS': 24,
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
        subprocess.check_call(['gcc', '-O1', '-Wall', '-o', exe, src,
                               '-I' + os.path.join(MESA_PREFIX, 'include'),
                               '-L' + MESA_LIB, '-Wl,-rpath-link,' + MESA_LIB,
                               '-lEGL', '-lGLESv2'])
    return exe


def vpm_writes(code):
    """Instructions writing the VPM (write address 48 on the add or mul unit)."""
    n = 0
    for w in code:
        if w >> 60 == 15:		# branch
            continue
        n += (((w >> 38) & 63) == 48) + (((w >> 32) & 63) == 48)
    return n


def main():
    ap = argparse.ArgumentParser(description='Precompile a GLSL ES program for the Pico GPU')
    ap.add_argument('-n', '--name', required=True, help='C identifier prefix')
    ap.add_argument('--vs', required=True)
    ap.add_argument('--fs', required=True)
    ap.add_argument('-a', '--attrib', action='append', default=[], help='NAME:TYPE:SIZE')
    ap.add_argument('-v', '--variant', action='append', default=[], help='PRIM:BLEND')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--keep', action='store_true', help='keep the dump (for debugging)')
    args = ap.parse_args()

    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.name):
        fail('the name must be a C identifier')
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
    if not 1 <= len(attribs) <= 8:
        fail('a program needs 1 to 8 attributes')

    variants = []
    for v in args.variant or ['triangles:none', 'triangles:alpha']:
        try:
            prim, blend = v.split(':')
        except ValueError:
            fail(f'bad variant {v!r}, expected PRIM:BLEND')
        if prim not in PRIMS or blend not in BLENDS:
            fail(f'bad variant {v!r}')
        variants.append((prim, blend))

    harness = build_harness()
    with tempfile.TemporaryDirectory() as tmp:
        job = os.path.join(tmp, 'job')
        dump = os.path.join(tmp, 'dump.jsonl')
        with open(job, 'w') as f:
            f.write(f'vs {os.path.abspath(args.vs)}\nfs {os.path.abspath(args.fs)}\n')
            for name, t, size in attribs:
                f.write(f'attrib {name} {TYPES[t][1]} {size} {TYPES[t][2]}\n')
            for prim, blend in variants:
                f.write(f'variant {prim} {blend}\n')
        env = dict(os.environ,
                   LD_LIBRARY_PATH=MESA_LIB,
                   LD_PRELOAD=os.path.join(MESA_LIB, 'libvc4_noop_drm_shim.so'),
                   MESA_LOADER_DRIVER_OVERRIDE='vc4',
                   MESA_SHADER_CACHE_DISABLE='true',
                   PGPU_VC4_DUMP=dump)
        r = subprocess.run([harness, job, dump], env=env, capture_output=True, text=True)
        out = r.stdout.splitlines()
        errors = [l[6:] for l in out if l.startswith('error ')]
        if errors or r.returncode:
            fail('\n  '.join(['compile failed:'] + (errors or [r.stderr.strip()])))
        records = [json.loads(l) for l in open(dump)] if os.path.exists(dump) else []
        if args.keep:
            open(args.output + '.dump.jsonl', 'w').write(''.join(json.dumps(o) + '\n' for o in records))

    # harness output: uniforms, markers, samplers
    uniforms = {}		# index -> (name, gl type, array size)
    markers = {}		# marker -> (uniform index, element, component)
    samplers = {}		# unit (= harness order) -> name
    for l in out:
        p = l.split()
        if p[0] == 'uniform':
            uniforms[int(p[1])] = (p[2], int(p[3], 16), int(p[4]))
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

    # the shaders used by each variant draw (fs, vs, cs); a draw that didn't
    # re-emit its shader state uses the previous one
    per_variant = []
    current = {}
    pending = None
    for o in records:
        if 'param' in o:
            continue
        if 'variant' in o:
            if pending is not None:
                per_variant.append(dict(current))
            pending = o['variant']
            continue
        current[o['stage']] = o
    if pending is not None:
        per_variant.append(dict(current))
    if len(per_variant) != len(variants) or any(len(v) != 3 for v in per_variant):
        fail('the driver did not compile every variant (see --keep)')

    # shader table (deduplicated)
    shaders, shader_index = [], {}
    uniform_slots = {'v': {}, 'f': {}}	# constant buffer word -> (uniform, element, component)
    sampler_units = {}		# sampler index (texture uniforms data) -> harness unit
    kinds_used = set()

    def shader_key(o):
        return (o['stage'], tuple(o['code']), tuple((k, d) for k, d, _ in o['stream']),
                o['threaded'], o['num_inputs'], o['vattrs_live'], tuple(o['vattr_offsets']))

    variant_words = []
    for (prim, blend), used in zip(variants, per_variant):
        idx = []
        for stage in ('fs', 'vs', 'cs'):
            o = used[stage]
            o['stream'] = []		# (kind, data) as the Zero gets them
            for kind, data, value in o['uniforms']:
                o['stream'].append([kind, data, value])
                kname = kind_names.get(kind, f'#{kind}')
                kinds_used.add(kname)
                if kname not in SUPPORTED_KINDS:
                    fail(f'the {stage.upper()} uses {kname}, which the Zero does not implement')
                if kname == 'QUNIFORM_UNIFORM':
                    f = struct.unpack('<f', struct.pack('<I', value))[0]
                    m = None
                    if f == int(f) and int(f) - MARKER_BASE in markers:
                        m = int(f) - MARKER_BASE
                    elif value - MARKER_BASE in markers:
                        m = value - MARKER_BASE
                        print(f'glslc: warning: uniform word {data} holds an integer', file=sys.stderr)
                    if m is None:
                        param = state_words['f' if stage == 'fs' else 'v'].get(data)
                        if param is None:
                            fail(f'{stage} uniform word {data} (value {value:#x}) matches no uniform')
                        if param[0] not in STATE_KINDS:
                            fail(f'the {stage.upper()} reads built-in GL state {param[0]}, '
                                 f'which the Zero does not provide')
                        o['stream'][-1][:2] = [STATE_KINDS[param[0]], param[1]]
                        continue
                    slots = uniform_slots['f' if stage == 'fs' else 'v']
                    if slots.setdefault(data, markers[m]) != markers[m]:
                        fail(f'{stage} uniform word {data} is used for two uniforms')
                elif kname == 'QUNIFORM_TEXTURE_CONFIG_P1':
                    width = (value >> 8) & 0x7FF
                    unit = {4 << n: n for n in range(8)}.get(width)
                    if unit is None:
                        fail(f'cannot identify sampler {data} (width {width})')
                    sampler_units[data] = unit
            k = shader_key(o)
            if k not in shader_index:
                shader_index[k] = len(shaders)
                shaders.append(o)
            idx.append(shader_index[k])

        cs = used['cs']
        # CS output: Xc, Yc, Zc, Wc, Xs|Ys, Zs, 1/Wc [, point size]
        point_size = prim == 'points' and vpm_writes([int(w, 16) for w in cs['code']]) == 8
        key = PRIMS[prim] | BLENDS[blend] << 8 | (1 << 16 if point_size else 0)
        variant_words += [key, idx[0] | idx[1] << 8 | idx[2] << 16]

    for s, unit in sampler_units.items():
        if s >= 8:
            fail('too many samplers')
    sampler_names = {s: samplers[u] for s, u in sampler_units.items()}

    # the program's uniform storage: the vertex constant buffer, then the
    # fragment one (FS uniform stream offsets are moved up by fs_base)
    fs_base = max(uniform_slots['v']) + 1 if uniform_slots['v'] else 0
    uniform_words = fs_base + (max(uniform_slots['f']) + 1 if uniform_slots['f'] else 0)
    if uniform_words > 4096:
        fail('the uniform storage is too large')

    # blob
    blob = [MAGIC, 0, len(attribs) | len(sampler_names) << 8 | len(variants) << 16 | len(shaders) << 24,
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
            vo = o['vattr_offsets']
            info = o['vattrs_live'] | vo[8] << 8
            offsets = [vo[0] | vo[1] << 8 | vo[2] << 16 | vo[3] << 24,
                       vo[4] | vo[5] << 8 | vo[6] << 16 | vo[7] << 24]
        blob += [len(code) | len(o['stream']) << 16, info | stage << 28] + offsets
        for w in code:
            blob += [w & 0xFFFFFFFF, w >> 32]
        for kind, data, _ in o['stream']:
            if stage == 0 and kind == 1:	# QUNIFORM_UNIFORM
                data += fs_base
            blob += [kind, data]
    blob[1] = len(blob)

    # C header
    ident = args.name
    lines = [f'/* Generated by tools/glslc/glslc.py from {os.path.basename(args.vs)}, '
             f'{os.path.basename(args.fs)} - do not edit */',
             f'#ifndef {ident.upper()}_PROGRAM_H', f'#define {ident.upper()}_PROGRAM_H', '',
             '#include <stdint.h>', '',
             f'/* variants: {", ".join(p + ":" + b for p, b in variants)}; '
             f'{len(shaders)} shaders, {sum(len(o["code"]) for o in shaders)} instructions */',
             f'#define {ident.upper()}_WORDS {len(blob)}',
             f'static const uint32_t {ident}_program[{len(blob)}] =', '{']
    for i in range(0, len(blob), 8):
        lines.append('\t' + ', '.join(f'0x{w:08x}' for w in blob[i:i + 8]) + ',')
    lines += ['};', '', '/* attribute locations (and the array formats they need) */']
    for i, (name, t, size) in enumerate(attribs):
        lines.append(f'#define {ident.upper()}_{cname(name).upper()} {i}\t/* {t} x {size} */')
    lines += ['', '/* uniforms: the storage word of each scalar (element-major, column-major)',
              '   for the vertex shaders, then for the fragment shader; 0xffff = not used',
              '   there. See pgpu_program_uniform (). */']
    for u in sorted(uniforms):
        name, gltype, size = uniforms[u]
        if name in samplers.values():
            continue
        comps = sum(1 for m in markers.values() if m[0] == u and m[1] == 0)
        offs = []
        for stage, base in (('v', 0), ('f', fs_base)):
            for e in range(size):
                for c in range(comps):
                    word = next((w for w, m in uniform_slots[stage].items() if m == (u, e, c)), None)
                    offs.append(0xFFFF if word is None else base + word)
        lines.append(f'static const uint16_t {ident}_{cname(name)}[{len(offs)}] = '
                     f'{{{", ".join(str(o) for o in offs)}}};')
    lines += ['', '/* samplers: index for pgpu_program_sampler () (default texture unit = index) */']
    for s in sorted(sampler_names):
        lines.append(f'#define {ident.upper()}_{cname(sampler_names[s]).upper()} {s}')
    lines += ['', '#endif', '']
    open(args.output, 'w').write('\n'.join(lines))

    print(f'glslc: {args.output}: {len(blob)} words, {len(variants)} variants, '
          f'{len(shaders)} shaders, uniform storage {uniform_words} words, '
          f'{len(sampler_names)} samplers')


def cname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


if __name__ == '__main__':
    main()
