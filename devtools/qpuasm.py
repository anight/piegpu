#!/usr/bin/env python3
#
# qpuasm.py - minimal assembler for VideoCore IV QPU shaders
#
# Programs are written with a Python API as a list of single ALU operations.
# The scheduler pairs add-pipe and mul-pipe operations into dual-issue
# instructions (list scheduling), respecting data dependencies and the QPU
# hazards:
#   - a regfile (ra/rb) result can be read 2 instructions later at the earliest
#   - an SFU result appears in r4 3 instructions after the SFU write
#   - one regfile A read, one regfile B read (or small immediate) per instruction
#   - add writes regfile A and mul writes regfile B, or swapped (WS bit)
#
# Encoding per the "VideoCore IV 3D Architecture Reference Guide" and Mesa's
# vc4_qpu_defines.h.
#

import struct

# signals
SIG_NONE, SIG_THREAD_SWITCH, SIG_PROG_END, SIG_WAIT_SCOREBOARD, SIG_SCOREBOARD_UNLOCK = 1, 2, 3, 4, 5
SIG_SMALL_IMM, SIG_LOAD_IMM = 13, 14

SIGNALS = {'nop': SIG_NONE, 'thrend': SIG_PROG_END, 'sbwait': SIG_WAIT_SCOREBOARD, 'sbdone': SIG_SCOREBOARD_UNLOCK,
           'colorload': 8, 'ldtmu0': 10}
SIG_WRITES_R4 = {8, 10}		# colour load, load TMU0: result in r4 next instruction

# condition codes
CONDS = {'never': 0, 'always': 1, 'zs': 2, 'zc': 3, 'ns': 4, 'nc': 5, 'cs': 6, 'cc': 7}

# r4 unpack modes (PM = 1): convert a colour byte to float 0..1
UNPACK_R4 = {'8a': 4, '8b': 5, '8c': 6, '8d': 7}

ADD_OPS = {'nop': 0, 'fadd': 1, 'fsub': 2, 'fmin': 3, 'fmax': 4, 'fminabs': 5, 'fmaxabs': 6,
           'ftoi': 7, 'itof': 8, 'add': 12, 'sub': 13, 'shr': 14, 'asr': 15, 'ror': 16, 'shl': 17,
           'min': 18, 'max': 19, 'and': 20, 'or': 21, 'xor': 22, 'not': 23, 'clz': 24}
MUL_OPS = {'nop': 0, 'fmul': 1, 'mul24': 2, 'v8muld': 3, 'v8min': 4, 'v8max': 5}

# write addresses (both files unless noted)
WADDR = {'r0': 32, 'r1': 33, 'r2': 34, 'r3': 35, 'r5': 37, 'nop': 39,
         'tlbz': 44, 'tlbc': 46, 'sfu_recip': 52, 'sfu_rsqrt': 53, 'sfu_exp': 54, 'sfu_log': 55,
         'tmu0_s': 56, 'tmu0_t': 57}
TMU_DSTS = {'tmu0_s', 'tmu0_t'}
SFU_DSTS = {'sfu_recip', 'sfu_rsqrt', 'sfu_exp', 'sfu_log'}

# read addresses of special registers: name -> (file, raddr)
RADDR_SPECIAL = {'unif': (None, 32), 'vary': (None, 35),
                 'x_coord': ('a', 41), 'y_coord': ('b', 41),
                 'elem_num': ('a', 38), 'qpu_num': ('b', 38)}

PACK_MUL = {'8888': 3, '8a': 4, '8b': 5, '8c': 6, '8d': 7}

COND_ALWAYS = 1


def small_imm_code(value):
    """Return the small immediate code for a value, or None."""
    if isinstance(value, int) and -16 <= value <= 15:
        return value & 0x1f
    if isinstance(value, float):
        for i in range(8):
            if value == float(1 << i):
                return 32 + i
        for i in range(8):
            if value == 2.0 ** (i - 8):
                return 40 + i
    return None


def f2u(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


class Operand:
    """Parsed source operand."""
    def __init__(self, text):
        self.text = text
        self.kind = None	# 'acc', 'ra', 'rb', 'special', 'imm'
        self.value = None
        self.unpack = None	# r4 unpack mode
        if isinstance(text, str) and text.startswith('r4.'):
            self.unpack = UNPACK_R4[text[3:]]
            text = 'r4'
        if isinstance(text, (int, float)):
            code = small_imm_code(text)
            if code is None:
                raise ValueError('not a small immediate: %r (use ldi)' % (text,))
            self.kind, self.value = 'imm', code
        elif text in ('r0', 'r1', 'r2', 'r3', 'r4', 'r5'):
            self.kind, self.value = 'acc', int(text[1])
        elif text.startswith('ra') and text[2:].isdigit():
            self.kind, self.value = 'ra', int(text[2:])
        elif text.startswith('rb') and text[2:].isdigit():
            self.kind, self.value = 'rb', int(text[2:])
        elif text in RADDR_SPECIAL:
            self.kind, self.value = 'special', text
        else:
            raise ValueError('bad operand %r' % (text,))


class Op:
    """One ALU operation, a load immediate or a signal."""
    def __init__(self, kind, opcode=None, dst=None, srcs=(), value=None, sig=None,
                 cond='always', sf=False):
        self.kind = kind	# 'alu', 'ldi', 'sig'
        self.opcode = opcode
        self.cond = CONDS[cond]
        self.sf = sf
        self.consumes_unif = False	# TMU writes take a config uniform from the stream
        self.pipe = None
        self.pack = None
        self.dst = None
        self.srcs = [Operand(s) for s in srcs]
        self.value = value
        self.sig = sig
        if dst is not None:
            if '.' in dst:
                dst, pack = dst.split('.')
                self.pack = PACK_MUL[pack]
            self.dst = dst
        if kind == 'alu':
            if opcode == 'mov':
                self.pipe = 'mul' if self.pack is not None else 'any'
            elif opcode in ADD_OPS:
                self.pipe = 'add'
            elif opcode in MUL_OPS:
                self.pipe = 'mul'
            else:
                raise ValueError('unknown opcode ' + opcode)
            if self.pack is not None and self.pipe != 'mul':
                raise ValueError('pack only on mul pipe')

    def reads(self):
        res = set()
        if self.cond not in (CONDS['always'], CONDS['never']):
            res.add('@flags')
        if self.consumes_unif:
            res.add('@unif')
        for s in self.srcs:
            if s.kind == 'acc':
                res.add('r%d' % s.value)
            elif s.kind in ('ra', 'rb'):
                res.add('%s%d' % (s.kind, s.value))
            elif s.kind == 'special':
                if s.value == 'unif':
                    res.add('@unif')
                elif s.value == 'vary':
                    res.add('@vary')
                    res.add('r5')
                else:
                    res.add('@' + s.value)
        return res

    def writes(self):
        res = set()
        if self.dst is not None:
            if self.dst in SFU_DSTS:
                res.add('r4')
            elif self.dst in ('tlbz', 'tlbc'):
                res.add('@tlb')
            elif self.dst in TMU_DSTS:
                res.add('@tmu')
            elif self.dst != 'nop':
                res.add(self.dst)
        for s in self.srcs:
            if s.kind == 'special' and s.value in ('unif', 'vary'):
                res.add('@' + s.value)	# reading advances the stream
                if s.value == 'vary':
                    res.add('r5')
        if getattr(self, 'dst2', None):
            res.add(self.dst2)
        if self.sf:
            res.add('@flags')
        if self.consumes_unif:
            res.add('@unif')
        if self.kind == 'sig':
            res.add('@barrier')
            if self.sig in SIG_WRITES_R4:
                res.add('r4')
        return res


def latency(producer, reg):
    """Minimum instruction distance between a write and a read of reg."""
    if reg == 'r4' and producer.dst in SFU_DSTS:
        return 3
    if reg.startswith('ra') or reg.startswith('rb'):
        return 2
    return 1


class Program:
    def __init__(self, name):
        self.name = name
        self.ops = []
        self.uniforms = []	# names in read order

    # --- API ---------------------------------------------------------------

    def alu(self, opcode, dst, *srcs, cond='always', sf=False):
        if opcode in ('mov', 'itof', 'ftoi', 'not', 'clz') and len(srcs) == 1:
            srcs = (srcs[0], srcs[0])
        op = Op('alu', opcode, dst, srcs, cond=cond, sf=sf)
        self.ops.append(op)
        return op

    def tex(self, s, t, uniform_p0, uniform_p1):
        """2D texture lookup on TMU0: write T, then S (which starts the lookup).
        Each write takes one texture config uniform from the stream (P0, P1).
        The result is loaded into r4 by sig ('ldtmu0')."""
        op = self.alu('mov', 'tmu0_t', t)
        op.consumes_unif = True
        self.uniforms.append(uniform_p0)
        op = self.alu('mov', 'tmu0_s', s)
        op.consumes_unif = True
        self.uniforms.append(uniform_p1)

    def unif(self, name):
        """Return operand text for reading the next uniform, recording its name."""
        self.uniforms.append(name)
        return 'unif'

    def ldi(self, dst, value, dst2=None):
        """Load a 32-bit immediate into dst (and dst2, which must be in the other file)."""
        op = Op('ldi', dst=dst, value=value)
        op.dst2 = dst2
        self.ops.append(op)

    def sig(self, name):
        self.ops.append(Op('sig', sig=SIGNALS[name]))

    def __getattr__(self, opcode):
        if opcode in ADD_OPS or opcode in MUL_OPS or opcode == 'mov':
            return lambda dst, *srcs, **kw: self.alu(opcode, dst, *srcs, **kw)
        raise AttributeError(opcode)

    # --- scheduling --------------------------------------------------------

    def schedule(self):
        ops = self.ops
        n = len(ops)
        # dependencies: deps[j] = list of (i, min distance)
        deps = [[] for _ in range(n)]
        for j in range(n):
            rj, wj = ops[j].reads(), ops[j].writes()
            for i in range(j):
                ri, wi = ops[i].reads(), ops[i].writes()
                dist = None
                for reg in rj & wi:				# RAW
                    d = latency(ops[i], reg)
                    dist = d if dist is None else max(dist, d)
                if wj & wi:					# WAW
                    dist = max(dist or 0, 1)
                if (wj & ri) and dist is None:			# WAR (same instr ok)
                    dist = 0
                if ops[i].kind == 'sig' or ops[j].kind == 'sig' or ops[i].kind == 'ldi' or ops[j].kind == 'ldi':
                    dist = max(dist or 0, 1)
                if dist is not None:
                    deps[j].append((i, dist))

        issued = [None] * n	# instruction index
        instrs = []		# list of lists of op indices
        cycle = 0
        remaining = set(range(n))
        while remaining:
            ready = []
            for j in sorted(remaining):
                ok = True
                for (i, d) in deps[j]:
                    if issued[i] is None or issued[i] + d > cycle:
                        ok = False
                        break
                if ok:
                    ready.append(j)
            chosen = []
            for j in ready:
                if self._fits(chosen, j):
                    chosen.append(j)
                    if ops[j].kind != 'alu' or len(chosen) == 2:
                        break
            # never let a later op overtake a ready earlier op on the same resources:
            # ready[] respects dependencies, so any order among ready ops is legal
            for j in chosen:
                issued[j] = cycle
                remaining.discard(j)
            instrs.append(chosen)
            cycle += 1
        self.instrs = instrs

    def _fits(self, chosen, j):
        ops = self.ops
        cand = [ops[k] for k in chosen] + [ops[j]]
        if len(cand) == 1:
            return True
        if any(o.kind != 'alu' for o in cand):
            return False
        return self._assign(cand) is not None

    def _assign(self, cand):
        """Assign pipes/ports for 1 or 2 alu ops; return encoding info or None."""
        pipes_options = []
        if len(cand) == 1:
            p = cand[0].pipe
            pipes_options = [('add',)] if p in ('add', 'any') else [('mul',)]
            if p == 'any':
                pipes_options = [('add',), ('mul',)]
        else:
            a, b = cand
            for pa in (['add', 'mul'] if a.pipe == 'any' else [a.pipe]):
                for pb in (['add', 'mul'] if b.pipe == 'any' else [b.pipe]):
                    if pa != pb:
                        pipes_options.append((pa, pb))
        for pipes in pipes_options:
            info = self._ports(cand, pipes)
            if info is not None:
                return info
        return None

    def _ports(self, cand, pipes):
        raddr_a = raddr_b = None
        small_imm = None
        muxes = {}
        # operands with a fixed port first, so that unif/vary take the free one
        order = []
        for op, pipe in zip(cand, pipes):
            for idx, s in enumerate(op.srcs):
                flexible = s.kind == 'special' and RADDR_SPECIAL[s.value][0] is None
                order.append((flexible, id(op), idx, s))
        order.sort(key=lambda e: e[0])
        slot = {}
        for flexible, key, idx, s in order:
            if s.kind == 'acc':
                mux = s.value
            elif s.kind == 'ra':
                if raddr_a not in (None, s.value):
                    return None
                raddr_a = s.value
                mux = 6
            elif s.kind == 'rb':
                if raddr_b not in (None, s.value) or small_imm is not None:
                    return None
                raddr_b = s.value
                mux = 7
            elif s.kind == 'imm':
                if raddr_b is not None or small_imm not in (None, s.value):
                    return None
                small_imm = s.value
                mux = 7
            else:	# special
                f, addr = RADDR_SPECIAL[s.value]
                if f in (None, 'a') and raddr_a in (None, addr):
                    raddr_a = addr
                    mux = 6
                elif f in (None, 'b') and raddr_b in (None, addr) and small_imm is None:
                    raddr_b = addr
                    mux = 7
                else:
                    return None
            slot[(key, idx)] = mux
        for op, pipe in zip(cand, pipes):
            m = [slot[(id(op), idx)] for idx in range(len(op.srcs))]
            while len(m) < 2:
                m.append(0)
            muxes[pipe] = (op, m)

        # r4 unpack is per instruction: all r4 reads must agree
        unpacks = {s.unpack for op in cand for s in op.srcs if s.kind == 'acc' and s.value == 4}
        if len(unpacks) > 1:
            return None
        unpack = unpacks.pop() if unpacks else None

        # set flags: at most one op; the flags come from the add pipe if it is used
        sf_ops = [op for op in cand if op.sf]
        if len(sf_ops) > 1:
            return None
        if sf_ops and len(cand) == 2 and muxes.get('add', (None,))[0] is not sf_ops[0]:
            return None
        # write swap
        def dst_file(op):
            if op is None or op.dst is None:
                return None
            if op.dst.startswith('ra'):
                return 'a'
            if op.dst.startswith('rb'):
                return 'b'
            return None
        add_op = muxes.get('add', (None, None))[0]
        mul_op = muxes.get('mul', (None, None))[0]
        fa, fm = dst_file(add_op), dst_file(mul_op)
        ws = None
        for w in (0, 1):
            add_file = 'a' if w == 0 else 'b'
            mul_file = 'b' if w == 0 else 'a'
            if fa not in (None, add_file) or fm not in (None, mul_file):
                continue
            ws = w
            break
        if ws is None:
            return None
        return dict(raddr_a=raddr_a, raddr_b=raddr_b, small_imm=small_imm, muxes=muxes, ws=ws,
                    unpack=unpack, sf=bool(sf_ops))

    # --- encoding ----------------------------------------------------------

    @staticmethod
    def _waddr(op):
        if op is None or op.dst is None:
            return 39
        d = op.dst
        if d.startswith('ra') or d.startswith('rb'):
            return int(d[2:])
        return WADDR[d]

    def encode(self):
        self.schedule()
        words = []
        pending_sig = None
        for chosen in self.instrs:
            ops = [self.ops[k] for k in chosen]
            if not ops:
                words.append(self._alu_word(None, SIG_NONE))
                continue
            if ops[0].kind == 'sig':
                # attach the signal to a nop (simple and always legal)
                words.append(self._alu_word(None, ops[0].sig))
                continue
            if ops[0].kind == 'ldi':
                op = ops[0]
                value = op.value if isinstance(op.value, int) else f2u(op.value)
                dsts = [op.dst] + ([op.dst2] if op.dst2 else [])
                waddr_add = waddr_mul = 39
                for d in dsts:
                    if d.startswith('rb'):
                        waddr_mul = int(d[2:])
                    elif d.startswith('ra'):
                        waddr_add = int(d[2:])
                    elif waddr_add == 39:
                        waddr_add = WADDR[d]
                    else:
                        waddr_mul = WADDR[d]
                w = (0b1110000 << 57) | (COND_ALWAYS << 49) | (COND_ALWAYS << 46) \
                    | (waddr_add << 38) | (waddr_mul << 32) | (value & 0xffffffff)
                words.append(w)
                continue
            words.append(self._alu_word(self._assign(ops), SIG_NONE))
        self.words = words
        return words

    def _alu_word(self, info, sig):
        if info is None:
            return (sig << 60) | (39 << 38) | (39 << 32) | (39 << 18) | (39 << 12)
        add_op, add_m = info['muxes'].get('add', (None, [0, 0]))
        mul_op, mul_m = info['muxes'].get('mul', (None, [0, 0]))
        raddr_a = info['raddr_a'] if info['raddr_a'] is not None else 39
        if info['small_imm'] is not None:
            sig = SIG_SMALL_IMM
            raddr_b = info['small_imm']
        else:
            raddr_b = info['raddr_b'] if info['raddr_b'] is not None else 39
        op_add = ADD_OPS['or'] if add_op is not None and add_op.opcode == 'mov' else \
            ADD_OPS[add_op.opcode] if add_op is not None else 0
        op_mul = MUL_OPS['v8min'] if mul_op is not None and mul_op.opcode == 'mov' else \
            MUL_OPS[mul_op.opcode] if mul_op is not None else 0
        pm, pack, unpack = 0, 0, 0
        if mul_op is not None and mul_op.pack is not None:
            pm, pack = 1, mul_op.pack
        if info['unpack'] is not None:
            pm, unpack = 1, info['unpack']
        cond_add = add_op.cond if add_op is not None else 0
        cond_mul = mul_op.cond if mul_op is not None else 0
        sf = 1 if info['sf'] else 0
        return (sig << 60) | (unpack << 57) | (pm << 56) | (pack << 52) | (cond_add << 49) | (cond_mul << 46) \
            | (sf << 45) \
            | (info['ws'] << 44) | (self._waddr(add_op) << 38) | (self._waddr(mul_op) << 32) \
            | (op_mul << 29) | (op_add << 24) | (raddr_a << 18) | (raddr_b << 12) \
            | (add_m[0] << 9) | (add_m[1] << 6) | (mul_m[0] << 3) | mul_m[1]

    def c_source(self):
        words = self.encode()
        lines = ['// %s: %d instructions, generated by qpuasm.py' % (self.name, len(words)),
                 'static const u32 %s[] __attribute__ ((aligned (16))) =' % self.name, '{']
        for w in words:
            lines.append('\t0x%08x, 0x%08x,' % (w & 0xffffffff, w >> 32))
        lines.append('};')
        if self.uniforms:
            lines.append('enum T%sUniform' % self.name)
            lines.append('{')
            for i, u in enumerate(self.uniforms):
                lines.append('\t%s_%s = %d,' % (self.name, u, i))
            lines.append('\t%s_Count = %d' % (self.name, len(self.uniforms)))
            lines.append('};')
        return '\n'.join(lines) + '\n'
