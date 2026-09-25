#!/usr/bin/env python3
#
# shaders.py - QPU fragment shaders of the fixed-function pipeline
#
# Generates shaders.h with one program per state combination:
#   texture environment: none, MODULATE, REPLACE, DECAL, BLEND   (5)
#   fog, alpha test, blending (incl. colour mask)                  (2 x 2 x 2)
# = 40 variants, indexed by  tex * 8 + fog * 4 + alpha * 2 + blend.
#
# Everything that changes per draw comes from uniforms. The renderer fills
# the uniform block from the kinds listed per variant (enum TUniformKind).
#
# Varyings (in this order): r, g, b, a  [, s, t]  [, fog factor]
#
# Registers:
#   ra0-ra3  fragment colour        rb0-rb3   texel
#   ra4-ra5  s, t                   rb4-rb7   destination colour (blend)
#   ra6      fog factor             rb8-rb10  fog colour
#                                   rb11-rb13 texture environment colour
#
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'devtools'))
from qpuasm import Program

TEX_MODES = ['NONE', 'MODULATE', 'REPLACE', 'DECAL', 'BLEND']
CHANNELS = 'rgba'

# uniform kinds (shared with renderer.cpp through shaders.h)
UNIFORM_KINDS = ['TEX_P0', 'TEX_P1', 'ENV_R', 'ENV_G', 'ENV_B', 'FOG_R', 'FOG_G', 'FOG_B',
                 'ALPHA_REF', 'ALPHA_KG', 'ALPHA_KL', 'ALPHA_KE'] + \
                ['BLEND_%s%d' % (c.upper(), k) for c in CHANNELS for k in range(10)]


def fragment_shader(tex, fog, alpha, blend):
    name = 'Frag_%s%s%s%s' % (TEX_MODES[tex], '_FOG' if fog else '', '_ALPHA' if alpha else '',
                             '_BLEND' if blend else '')
    p = Program(name)
    color = ['ra0', 'ra1', 'ra2', 'ra3']

    # --- varyings: value = vary * W + C --------------------------------------
    regs = color + (['ra4', 'ra5'] if tex else []) + (['ra6'] if fog else [])
    for reg in regs:
        p.fmul('r0', 'vary', 'ra15')
        p.fadd(reg, 'r0', 'r5')

    # --- texture ---------------------------------------------------------------
    if tex:
        p.tex('ra4', 'ra5', 'TEX_P0', 'TEX_P1')
        p.sig('ldtmu0')
        texel = ['rb0', 'rb1', 'rb2', 'rb3']
        for i, byte in enumerate(('8a', '8b', '8c', '8d')):	# R in byte 0
            p.mov(texel[i], 'r4.' + byte)

        mode = TEX_MODES[tex]
        if mode == 'MODULATE':				# C = Cf * Ct, A = Af * At
            for i in range(4):
                p.fmul(color[i], color[i], texel[i])
        elif mode == 'REPLACE':				# C = Ct, A = At
            for i in range(4):
                p.mov(color[i], texel[i])
        elif mode == 'DECAL':				# C = Cf + At (Ct - Cf), A = Af
            for i in range(3):
                p.fsub('r0', texel[i], color[i])
                p.fmul('r0', 'r0', 'rb3')
                p.fadd(color[i], color[i], 'r0')
        elif mode == 'BLEND':				# C = Cf + Ct (Cc - Cf), A = Af * At
            env = ['rb11', 'rb12', 'rb13']
            for i, u in enumerate(('ENV_R', 'ENV_G', 'ENV_B')):
                p.mov(env[i], p.unif(u))
            for i in range(3):
                p.fsub('r0', env[i], color[i])
                p.fmul('r0', 'r0', texel[i])
                p.fadd(color[i], color[i], 'r0')
            p.fmul(color[3], color[3], texel[3])

    # --- fog: C = Cfog + f (C - Cfog) ------------------------------------------
    if fog:
        fogc = ['rb8', 'rb9', 'rb10']
        for i, u in enumerate(('FOG_R', 'FOG_G', 'FOG_B')):
            p.mov(fogc[i], p.unif(u))
        for i in range(3):
            p.fsub('r0', color[i], fogc[i])
            p.fmul('r0', 'r0', 'ra6')
            p.fadd(color[i], 'r0', fogc[i])

    # --- alpha test: pass = kg (a > ref), kl (a < ref), ke (a == ref) ---------
    write_cond = 'always'
    if alpha:
        p.fsub('r0', 'ra3', p.unif('ALPHA_REF'), sf=True)
        p.mov('r1', p.unif('ALPHA_KG'))
        p.mov('r1', p.unif('ALPHA_KL'), cond='ns')
        p.mov('r1', p.unif('ALPHA_KE'), cond='zs')
        p.mov('r1', 'r1', sf=True)			# Z set: fragment fails
        write_cond = 'zc'

    # --- blending (and colour mask) --------------------------------------------
    # out = Cs * Fs + Cd * Fd, F = k0 + k1 As + k2 Ad + k3 Cs + k4 Cd (per channel)
    p.sig('sbwait')
    if blend:
        p.sig('colorload')
        dst = ['rb4', 'rb5', 'rb6', 'rb7']
        for i, byte in enumerate(('8a', '8b', '8c', '8d')):
            p.mov(dst[i], 'r4.' + byte)
        for i, c in enumerate(CHANNELS):
            k = ['BLEND_%s%d' % (c.upper(), n) for n in range(10)]
            for half, target in ((0, 'r2'), (5, 'r0')):	# Fs -> r2, Fd -> r0
                p.fmul(target, 'ra3', p.unif(k[half + 1]))
                p.fadd(target, target, p.unif(k[half + 0]))
                p.fmul('r1', 'rb7', p.unif(k[half + 2]))
                p.fadd(target, target, 'r1')
                p.fmul('r1', color[i], p.unif(k[half + 3]))
                p.fadd(target, target, 'r1')
                p.fmul('r1', dst[i], p.unif(k[half + 4]))
                p.fadd(target, target, 'r1')
            p.fmul('r2', 'r2', color[i])
            p.fmul('r0', 'r0', dst[i])
            p.fadd('r2', 'r2', 'r0')
            p.mov('r3.8' + 'abcd'[i], 'r2')
    else:
        for i in range(4):
            p.mov('r3.8' + 'abcd'[i], color[i])

    p.mov('tlbz', 'rb15', cond=write_cond)
    p.mov('tlbc', 'r3', cond=write_cond)
    p.sig('thrend')
    p.sig('nop')
    p.sig('sbdone')

    varyings = 4 + (2 if tex else 0) + (1 if fog else 0)
    return p, varyings


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'shaders.h'
    text = ['// generated by shaders.py - do not edit', '#ifndef _shaders_h', '#define _shaders_h', '',
            '#include <circle/types.h>', '',
            'enum TUniformKind', '{']
    text += ['\tUniform%s,' % k.title().replace('_', '') if False else '\tUNIFORM_%s,' % k for k in UNIFORM_KINDS]
    text += ['\tUNIFORM_KINDS', '};', '']

    table = []
    total = 0
    for tex in range(5):
        for fog in range(2):
            for alpha in range(2):
                for blend in range(2):
                    p, varyings = fragment_shader(tex, fog, alpha, blend)
                    text.append(p.c_source().split('enum')[0])	# code only
                    kinds = ', '.join('UNIFORM_%s' % u for u in p.uniforms) or '0'
                    text.append('static const u8 %s_Uniforms[] = {%s};' % (p.name, kinds))
                    table.append('\t{%s, sizeof %s / 8, %s_Uniforms, %d, %d},'
                                 % (p.name, p.name, p.name, len(p.uniforms), varyings))
                    total += len(p.words)

    text += ['', 'struct TFragmentShader',
             '{',
             '\tconst u32 *pCode;',
             '\tunsigned nInstructions;',
             '\tconst u8 *pUniformKinds;',
             '\tunsigned nUniforms;',
             '\tunsigned nVaryings;',
             '};', '',
             '// index: tex * 8 + fog * 4 + alpha * 2 + blend',
             '#define FRAGMENT_SHADERS %d' % len(table),
             'static const TFragmentShader FragmentShaders[FRAGMENT_SHADERS] =', '{'] + table + ['};', '',
             '// %d instructions in total' % total, '', '#endif']
    open(out, 'w').write('\n'.join(text) + '\n')


if __name__ == '__main__':
    main()
