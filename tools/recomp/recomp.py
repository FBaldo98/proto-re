#!/usr/bin/env python3
"""Static recompiler: x86-32 functions of prototypeenginef.dll -> C (see src/recomp/runtime.h).

Each function becomes `void f_<va>(Ctx* c)`. Control flow inside the function maps to labels and goto; jumps
leaving the function are tail calls through RecompDispatch; calls push the real guest return address and go
through RecompDispatch; `ret n` pops the return address into c->ret_target and returns. Flags are computed
exactly for every instruction that defines them (no liveness elimination yet). Unsupported instructions are
emitted as RecompUnimplemented() calls and counted in the summary, never silently dropped.

Usage:
  recomp.py --functions <rva list file | all> --out <file.c>
"""
import argparse
import collections
import os
import sys

import capstone
from capstone import x86_const as X
import pefile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DLL = os.environ.get("PROTORE_ENGINE",
                     os.path.expanduser("~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll"))
BASE = 0x10000000

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

COND = {
    "o": "c->of", "no": "!c->of",
    "b": "c->cf", "ae": "!c->cf",
    "e": "c->zf", "ne": "!c->zf",
    "be": "(c->cf || c->zf)", "a": "(!c->cf && !c->zf)",
    "s": "c->sf", "ns": "!c->sf",
    "p": "c->pf", "np": "!c->pf",
    "l": "(c->sf != c->of)", "ge": "(c->sf == c->of)",
    "le": "(c->zf || c->sf != c->of)", "g": "(!c->zf && c->sf == c->of)",
}


class Unsupported(Exception):
    pass


def load_functions():
    sizes = {}
    with open(os.path.join(ROOT, "re", "functions.csv")) as f:
        next(f)
        for row in f:
            p = row.split(",")
            if p[2] == "false":
                sizes[int(p[0], 16)] = int(p[1])
    return sizes


class FunctionCompiler:
    def __init__(self, md, pe, va, size, known_entries):
        self.md = md
        self.va = va
        self.size = size
        self.known = known_entries
        self.code = pe.get_data(va - BASE, size)
        self.ins = list(md.disasm(self.code, va))
        self.out = []
        self.unsupported = collections.Counter()
        self.table_reads = 0

    # ---- operands ----
    def reg_get(self, name):
        if name in REG32:
            return f"c->{name}"
        if name in REG16:
            return f"(c->{REG16[name]} & 0xffffu)"
        if name in REG8L:
            return f"(c->{REG8L[name]} & 0xffu)"
        if name in REG8H:
            return f"((c->{REG8H[name]} >> 8) & 0xffu)"
        raise Unsupported(f"register {name}")

    def reg_set(self, name, value):
        if name in REG32:
            return f"c->{name} = (uint32_t)({value});"
        if name in REG16:
            r = REG16[name]
            return f"c->{r} = (c->{r} & 0xffff0000u) | ((uint32_t)({value}) & 0xffffu);"
        if name in REG8L:
            r = REG8L[name]
            return f"c->{r} = (c->{r} & 0xffffff00u) | ((uint32_t)({value}) & 0xffu);"
        if name in REG8H:
            r = REG8H[name]
            return f"c->{r} = (c->{r} & 0xffff00ffu) | (((uint32_t)({value}) & 0xffu) << 8);"
        raise Unsupported(f"register {name}")

    def address(self, ins, op):
        m = op.mem
        if m.segment != 0:
            raise Unsupported(f"segment {ins.reg_name(m.segment)}")
        parts = []
        if m.base != 0:
            parts.append(self.reg_get(ins.reg_name(m.base)))
        if m.index != 0:
            parts.append(f"{self.reg_get(ins.reg_name(m.index))} * {m.scale}u")
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xffffffff:x}u")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read(self, ins, op, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            return self.reg_get(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & Mask(bits):x}u"
        if op.type == X.X86_OP_MEM:
            fn = {8: "R8", 16: "R16", 32: "R32"}.get(bits)
            if not fn:
                raise Unsupported(f"{bits}-bit memory read")
            return f"{fn}({self.address(ins, op)})"
        raise Unsupported("operand type")

    def write(self, ins, op, value, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            return self.reg_set(ins.reg_name(op.reg), value)
        if op.type == X.X86_OP_MEM:
            fn = {8: "W8", 16: "W16", 32: "W32"}.get(bits)
            if not fn:
                raise Unsupported(f"{bits}-bit memory write")
            cast = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}[bits]
            return f"{fn}({self.address(ins, op)}, ({cast})({value}));"
        raise Unsupported("write to non-lvalue")

    # ---- control flow ----
    def targets(self):
        t = set()
        lo, hi = self.va, self.va + self.size
        for i in self.ins:
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                d = i.operands[0].imm
                if lo <= d < hi:
                    t.add(d)
        return t

    def inside(self, d):
        return self.va <= d < self.va + self.size

    def jump_to(self, d):
        if self.inside(d):
            return f"goto L_{d:08x};"
        return f"RecompDispatch(c, 0x{d:08x}u); return;"

    # ---- instructions ----
    def emit(self, ins):
        m = ins.mnemonic
        ops = ins.operands
        nxt = ins.address + ins.size
        rep = X.X86_PREFIX_REP in ins.prefix or X.X86_PREFIX_REPE in ins.prefix
        repne = X.X86_PREFIX_REPNE in ins.prefix
        base = m.split()[-1]
        bits = ops[0].size * 8 if ops else 32

        if base in ("nop", "int3"):
            return "/* " + m + " */" if base == "nop" else f'RecompUnimplemented(c, 0x{ins.address:08x}u, "int3"); return;'
        if base == "mov" or base == "movabs":
            return self.write(ins, ops[0], self.read(ins, ops[1], bits))
        if base in ("movzx", "movsx"):
            sb = ops[1].size * 8
            v = self.read(ins, ops[1], sb)
            if base == "movsx":
                v = f"(uint32_t)(int32_t)(int{sb}_t)({v})"
            return self.write(ins, ops[0], v)
        if base == "lea":
            return self.write(ins, ops[0], self.address(ins, ops[1]))
        if base == "push":
            if ops[0].type == X.X86_OP_IMM:
                return f"Push32(c, 0x{ops[0].imm & 0xffffffff:x}u);"
            return f"{{ uint32_t v = {self.read(ins, ops[0])}; Push32(c, v); }}"
        if base == "pop":
            return "{ uint32_t v = Pop32(c); " + self.write(ins, ops[0], "v") + " }"
        if base in ("add", "sub", "adc", "sbb", "cmp"):
            a = self.read(ins, ops[0], bits)
            b = self.read(ins, ops[1], bits)
            if ops[1].type == X.X86_OP_IMM:
                b = f"0x{ops[1].imm & Mask(bits):x}u"
            fn = "AddFlags" if base in ("add", "adc") else "SubFlags"
            carry = "c->cf" if base in ("adc", "sbb") else "0"
            expr = f"{fn}(c, {a}, {b}, {carry}, {bits})"
            if base == "cmp":
                return f"(void){expr};"
            return "{ uint32_t r = " + expr + "; " + self.write(ins, ops[0], "r") + " }"
        if base in ("and", "or", "xor", "test"):
            op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[base]
            a = self.read(ins, ops[0], bits)
            b = self.read(ins, ops[1], bits)
            if ops[1].type == X.X86_OP_IMM:
                b = f"0x{ops[1].imm & Mask(bits):x}u"
            s = f"{{ uint32_t r = ({a} {op} {b}) & 0x{Mask(bits):x}u; RECOMP_FLAGS_LOGIC(c, r, {bits});"
            if base != "test":
                s += " " + self.write(ins, ops[0], "r")
            return s + " }"
        if base in ("inc", "dec"):
            a = self.read(ins, ops[0], bits)
            fn = "AddFlags" if base == "inc" else "SubFlags"
            return ("{ uint8_t cf = c->cf; uint32_t r = " + f"{fn}(c, {a}, 1u, 0, {bits}); c->cf = cf; "
                    + self.write(ins, ops[0], "r") + " }")
        if base == "neg":
            a = self.read(ins, ops[0], bits)
            return ("{ uint32_t r = " + f"SubFlags(c, 0u, {a}, 0, {bits}); " + self.write(ins, ops[0], "r") + " }")
        if base == "not":
            return self.write(ins, ops[0], f"~{self.read(ins, ops[0], bits)}")
        if base in ("shl", "sal", "shr", "sar"):
            a = self.read(ins, ops[0], bits)
            n = self.read(ins, ops[1], 8) if len(ops) > 1 else "1u"
            sign = 1 << (bits - 1)
            body = f"uint32_t n = ({n}) & 31u; uint32_t a = {a} & 0x{Mask(bits):x}u; if (n) {{ uint32_t r; "
            if base in ("shl", "sal"):
                body += (f"r = (uint32_t)((uint64_t)a << n) & 0x{Mask(bits):x}u; c->cf = (uint32_t)(((uint64_t)a << n) "
                         f">> {bits}) & 1u; c->of = ((r >> {bits - 1}) & 1u) ^ c->cf; ")
            elif base == "shr":
                body += f"r = a >> n; c->cf = (a >> (n - 1)) & 1u; c->of = (a >> {bits - 1}) & 1u; "
            else:
                body += (f"int32_t sa = (int32_t)(a << {32 - bits}) >> {32 - bits}; r = (uint32_t)(n >= {bits} ? (sa >> "
                         f"{bits - 1}) : (sa >> n)) & 0x{Mask(bits):x}u; c->cf = (uint32_t)(sa >> (n > {bits} ? {bits - 1} : n - 1)) & 1u; c->of = 0; ")
            body += (f"c->zf = r == 0; c->sf = (r & 0x{sign:x}u) != 0; c->pf = Parity(r); "
                     + self.write(ins, ops[0], "r") + " }")
            return "{ " + body + " }"
        if base in ("rol", "ror"):
            a = self.read(ins, ops[0], bits)
            n = self.read(ins, ops[1], 8) if len(ops) > 1 else "1u"
            if base == "rol":
                expr = f"((a << n) | (a >> (({bits} - n) % {bits}))) & 0x{Mask(bits):x}u"
                cf = "r & 1u"
            else:
                expr = f"((a >> n) | (a << (({bits} - n) % {bits}))) & 0x{Mask(bits):x}u"
                cf = f"(r >> {bits - 1}) & 1u"
            return ("{ uint32_t n = (" + n + f") & 31u % {bits}; uint32_t a = {a} & 0x{Mask(bits):x}u; "
                    f"if (n) {{ uint32_t r = {expr}; c->cf = {cf}; " + self.write(ins, ops[0], "r") + " } }")
        if base == "imul":
            if len(ops) == 1:
                if bits != 32:
                    raise Unsupported("imul r/m8/16")
                v = self.read(ins, ops[0])
                return ("{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t)" + v + "; c->eax = (uint32_t)p; "
                        "c->edx = (uint32_t)((uint64_t)p >> 32); c->cf = c->of = p != (int64_t)(int32_t)p; }")
            a = self.read(ins, ops[1 if len(ops) == 3 else 0])
            b = f"0x{ops[2].imm & 0xffffffff:x}u" if len(ops) == 3 else self.read(ins, ops[1])
            return ("{ int64_t p = (int64_t)(int32_t)" + a + " * (int32_t)" + b + "; uint32_t r = (uint32_t)p; "
                    "c->cf = c->of = p != (int64_t)(int32_t)r; " + self.write(ins, ops[0], "r") + " }")
        if base == "mul":
            if bits != 32:
                raise Unsupported("mul r/m8/16")
            v = self.read(ins, ops[0])
            return ("{ uint64_t p = (uint64_t)c->eax * " + v + "; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32); "
                    "c->cf = c->of = c->edx != 0; }")
        if base in ("div", "idiv"):
            if bits != 32:
                raise Unsupported(f"{base} r/m8/16")
            v = self.read(ins, ops[0])
            if base == "div":
                return ("{ uint64_t n = ((uint64_t)c->edx << 32) | c->eax; uint32_t d = " + v + "; "
                        "c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }")
            return ("{ int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); int32_t d = (int32_t)" + v + "; "
                    "c->eax = (uint32_t)(int32_t)(n / d); c->edx = (uint32_t)(int32_t)(n % d); }")
        if base == "cdq":
            return "c->edx = (c->eax & 0x80000000u) ? 0xffffffffu : 0u;"
        if base == "xchg":
            a, b = self.read(ins, ops[0]), self.read(ins, ops[1])
            return "{ uint32_t a = " + a + ", b = " + b + "; " + self.write(ins, ops[0], "b") + " " + self.write(ins, ops[1], "a") + " }"
        if base.startswith("set") and base[3:] in COND:
            return self.write(ins, ops[0], f"({COND[base[3:]]}) ? 1u : 0u", 8)
        if base.startswith("cmov") and base[4:] in COND:
            return f"if ({COND[base[4:]]}) {{ " + self.write(ins, ops[0], self.read(ins, ops[1])) + " }"
        if base.startswith("j") and base != "jmp" and base[1:] in COND:
            return f"if ({COND[base[1:]]}) {{ {self.jump_to(ops[0].imm)} }}"
        if base in ("jecxz",):
            return f"if (c->ecx == 0) {{ {self.jump_to(ops[0].imm)} }}"
        if base == "jmp":
            if ops[0].type == X.X86_OP_IMM:
                return self.jump_to(ops[0].imm)
            target = self.read(ins, ops[0])
            # Jump table or computed jump: switch over every instruction address of this function, otherwise
            # treat it as a tail call.
            cases = " ".join(f"case 0x{i.address:08x}u: goto L_{i.address:08x};" for i in self.ins)
            self.table_reads += 1
            self.computed_targets = True
            return (f"{{ uint32_t t = {target}; switch (t) {{ {cases} default: RecompDispatch(c, t); return; }} }}")
        if base == "call":
            if ops[0].type == X.X86_OP_IMM:
                return f"Push32(c, 0x{nxt:08x}u); RecompDispatch(c, 0x{ops[0].imm & 0xffffffff:08x}u);"
            return f"{{ uint32_t t = {self.read(ins, ops[0])}; Push32(c, 0x{nxt:08x}u); RecompDispatch(c, t); }}"
        if base == "ret":
            n = ops[0].imm if ops else 0
            return f"c->ret_target = Pop32(c); c->esp += {n}u; return;"
        if base == "leave":
            return "c->esp = c->ebp; c->ebp = Pop32(c);"
        if base in ("movsd", "movsb", "movsw", "stosd", "stosb", "stosw") and not ops_are_xmm(ins):
            width = {"d": 4, "b": 1, "w": 2}[base[-1]]
            rd, wr = {4: ("R32", "W32"), 1: ("R8", "W8"), 2: ("R16", "W16")}[width]
            step = f"(c->df ? -{width} : {width})"
            if base.startswith("movs"):
                one = f"{wr}(c->edi, {rd}(c->esi)); c->esi += {step}; c->edi += {step};"
            else:
                val = {4: "c->eax", 1: "(uint8_t)c->eax", 2: "(uint16_t)c->eax"}[width]
                one = f"{wr}(c->edi, {val}); c->edi += {step};"
            if rep:
                return f"while (c->ecx) {{ {one} c->ecx--; }}"
            return one
        if base in ("cmpsb", "cmpsd", "cmpsw", "scasb", "scasd"):
            width = {"b": 1, "d": 4, "w": 2}[base[-1]]
            rd = {4: "R32", 1: "R8", 2: "R16"}[width]
            step = f"(c->df ? -{width} : {width})"
            if base.startswith("cmps"):
                one = f"(void)SubFlags(c, {rd}(c->esi), {rd}(c->edi), 0, {width * 8}); c->esi += {step}; c->edi += {step};"
            else:
                one = f"(void)SubFlags(c, c->eax, {rd}(c->edi), 0, {width * 8}); c->edi += {step};"
            if rep:
                return f"while (c->ecx) {{ {one} c->ecx--; if (!c->zf) break; }}"
            if repne:
                return f"while (c->ecx) {{ {one} c->ecx--; if (c->zf) break; }}"
            return one
        if base == "cmpxchg":
            if bits != 32:
                raise Unsupported("cmpxchg 8/16")
            if ops[0].type == X.X86_OP_MEM:
                addr = self.address(ins, ops[0])
                src = self.read(ins, ops[1])
                return ("{ uint32_t expected = c->eax; uint32_t* p = (uint32_t*)G(" + addr + "); "
                        "uint32_t old = __sync_val_compare_and_swap(p, expected, " + src + "); "
                        "(void)SubFlags(c, expected, old, 0, 32); if (!c->zf) c->eax = old; }")
            raise Unsupported("cmpxchg reg")
        if base == "cld":
            return "c->df = 0;"
        if base == "std":
            return "c->df = 1;"
        # ---- SSE scalar float ----
        if base in ("movss", "movd") and ops_are_xmm(ins):
            return self.sse_move(ins, base)
        if base in ("addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss"):
            return self.sse_arith(ins, base)
        if base in ("cvtsi2ss", "cvttss2si", "cvtss2sd", "cvtsd2ss", "comiss", "ucomiss", "xorps", "andps"):
            return self.sse_misc(ins, base)
        # ---- x87 (prototype subset) ----
        if base in ("fld", "fstp", "fst", "fild", "fistp", "fldz", "fld1", "fxch", "fchs", "fabs", "faddp", "fadd",
                    "fsubp", "fsub", "fmulp", "fmul", "fdivp", "fdiv"):
            return self.x87(ins, base)
        raise Unsupported(m)

    # ---- SSE helpers ----
    def xmm(self, ins, op):
        name = ins.reg_name(op.reg)
        if not name.startswith("xmm"):
            raise Unsupported(f"register {name}")
        return f"c->xmm[{int(name[3:])}]"

    def sse_move(self, ins, base):
        d, s = ins.operands
        if base == "movss":
            if d.type == X.X86_OP_REG and s.type == X.X86_OP_MEM:
                return f"{{ {self.xmm(ins, d)}.f[0] = RF32({self.address(ins, s)}); {self.xmm(ins, d)}.u32[1] = {self.xmm(ins, d)}.u32[2] = {self.xmm(ins, d)}.u32[3] = 0; }}"
            if d.type == X.X86_OP_MEM:
                return f"WF32({self.address(ins, d)}, {self.xmm(ins, s)}.f[0]);"
            return f"{self.xmm(ins, d)}.f[0] = {self.xmm(ins, s)}.f[0];"
        # movd
        if d.type == X.X86_OP_REG and ins.reg_name(d.reg).startswith("xmm"):
            v = self.read(ins, s, 32)
            return f"{{ {self.xmm(ins, d)}.u64[0] = {v}; {self.xmm(ins, d)}.u64[1] = 0; }}"
        return self.write(ins, d, f"{self.xmm(ins, s)}.u32[0]", 32)

    def sse_src_f32(self, ins, op):
        if op.type == X.X86_OP_MEM:
            return f"RF32({self.address(ins, op)})"
        return f"{self.xmm(ins, op)}.f[0]"

    def sse_arith(self, ins, base):
        d, s = ins.operands
        dst = f"{self.xmm(ins, d)}.f[0]"
        src = self.sse_src_f32(ins, s)
        op = {"addss": "+", "subss": "-", "mulss": "*", "divss": "/"}.get(base)
        if op:
            return f"{dst} = {dst} {op} {src};"
        if base == "minss":
            return f"{dst} = ({dst} < {src}) ? {dst} : {src};"
        if base == "maxss":
            return f"{dst} = ({dst} > {src}) ? {dst} : {src};"
        return f"{dst} = sqrtf({src});"

    def sse_misc(self, ins, base):
        d, s = ins.operands
        if base == "cvtsi2ss":
            return f"{self.xmm(ins, d)}.f[0] = (float)(int32_t){self.read(ins, s, 32)};"
        if base == "cvttss2si":
            return self.write(ins, d, f"(uint32_t)RecompTruncF32({self.sse_src_f32(ins, s)})")
        if base in ("comiss", "ucomiss"):
            a = f"{self.xmm(ins, d)}.f[0]"
            b = self.sse_src_f32(ins, s)
            return (f"{{ float a = {a}, b = {b}; int un = a != a || b != b; c->zf = un || a == b; c->pf = un; "
                    f"c->cf = un || a < b; c->of = c->sf = 0; }}")
        if base in ("xorps", "andps"):
            op = "^" if base == "xorps" else "&"
            if s.type == X.X86_OP_MEM:
                a = self.address(ins, s)
                return (f"{{ {self.xmm(ins, d)}.u64[0] {op}= R64({a}); {self.xmm(ins, d)}.u64[1] {op}= R64({a} + 8u); }}")
            return (f"{{ {self.xmm(ins, d)}.u64[0] {op}= {self.xmm(ins, s)}.u64[0]; "
                    f"{self.xmm(ins, d)}.u64[1] {op}= {self.xmm(ins, s)}.u64[1]; }}")
        if base == "cvtss2sd":
            return f"{self.xmm(ins, d)}.d[0] = (double){self.sse_src_f32(ins, s)};"
        raise Unsupported(base)

    # ---- x87 helpers (double precision prototype) ----
    def x87(self, ins, base):
        ops = ins.operands
        st = lambda i: f"c->st[(c->st_top + {i}) & 7]"
        push = "c->st_top = (c->st_top - 1) & 7;"
        pop = "c->st_top = (c->st_top + 1) & 7;"
        if base in ("fld", "fild"):
            op = ops[0]
            if op.type == X.X86_OP_REG:
                idx = int(ins.reg_name(op.reg)[3:-1]) if "(" in ins.reg_name(op.reg) else 0
                return f"{{ double v = {st(idx)}; {push} {st(0)} = v; }}"
            a = self.address(ins, op)
            if base == "fild":
                v = {2: f"(double)(int16_t)R16({a})", 4: f"(double)(int32_t)R32({a})", 8: f"(double)(int64_t)R64({a})"}[op.size]
            else:
                v = {4: f"(double)RF32({a})", 8: f"RF64({a})"}.get(op.size)
                if v is None:
                    raise Unsupported("fld m80")
            return f"{{ double v = {v}; {push} {st(0)} = v; }}"
        if base in ("fst", "fstp", "fistp"):
            op = ops[0]
            tail = f" {pop}" if base != "fst" else ""
            if op.type == X.X86_OP_REG:
                idx = int(ins.reg_name(op.reg)[3:-1]) if "(" in ins.reg_name(op.reg) else 0
                return f"{st(idx)} = {st(0)};{tail}"
            a = self.address(ins, op)
            if base == "fistp":
                w = {2: f"W16({a}, (uint16_t)RecompRoundToInt({st(0)}))", 4: f"W32({a}, (uint32_t)RecompRoundToInt({st(0)}))",
                     8: f"W64({a}, (uint64_t)RecompRoundToInt64({st(0)}))"}[op.size]
            else:
                w = {4: f"WF32({a}, (float){st(0)})", 8: f"WF64({a}, {st(0)})"}.get(op.size)
                if w is None:
                    raise Unsupported("fst m80")
            return f"{w};{tail}"
        if base == "fldz":
            return f"{push} {st(0)} = 0.0;"
        if base == "fld1":
            return f"{push} {st(0)} = 1.0;"
        if base == "fchs":
            return f"{st(0)} = -{st(0)};"
        if base == "fabs":
            return f"{st(0)} = fabs({st(0)});"
        if base == "fxch":
            idx = 1
            if ops:
                idx = int(ins.reg_name(ops[0].reg)[3:-1]) if "(" in ins.reg_name(ops[0].reg) else 1
            return f"{{ double t = {st(0)}; {st(0)} = {st(idx)}; {st(idx)} = t; }}"
        sym = {"fadd": "+", "faddp": "+", "fsub": "-", "fsubp": "-", "fmul": "*", "fmulp": "*", "fdiv": "/", "fdivp": "/"}[base]
        if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
            a = self.address(ins, ops[0])
            v = {4: f"(double)RF32({a})", 8: f"RF64({a})"}[ops[0].size]
            return f"{st(0)} = {st(0)} {sym} {v};"
        raise Unsupported(f"{base} register form")

    def compile(self):
        labels = self.targets()
        self.computed_targets = False
        body = []
        for i in self.ins:
            line = ""
            try:
                line = self.emit(i)
            except Unsupported as e:
                self.unsupported[str(e)] += 1
                line = f'RecompUnimplemented(c, 0x{i.address:08x}u, "{i.mnemonic} {i.op_str}"); return;'
            body.append((i, line))
        out = [f"void f_{self.va:08x}(Ctx* c) {{"]
        for i, line in body:
            if i.address in labels or self.computed_targets:
                out.append(f"L_{i.address:08x}:;")
            out.append(f"    {line}  /* {i.address:08x}: {i.mnemonic} {i.op_str} */")
        last = self.ins[-1] if self.ins else None
        if last is None or (last.address + last.size) - self.va < self.size:
            out.append(f'    RecompUnimplemented(c, 0x{self.va:08x}u, "decode stopped early");')
        out.append("}")
        return "\n".join(out)


def ops_are_xmm(ins):
    return any(o.type == X.X86_OP_REG and ins.reg_name(o.reg).startswith("xmm") for o in ins.operands)


def Mask(bits):
    return (1 << bits) - 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    sizes = load_functions()
    if args.functions == "all":
        vas = sorted(sizes)
    else:
        vas = sorted(int(l.strip(), 16) + (BASE if int(l.strip(), 16) < BASE else 0)
                     for l in open(args.functions) if l.strip())
    pe = pefile.PE(DLL, fast_load=True)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    unsupported = collections.Counter()
    funcs = []
    for va in vas:
        if va not in sizes:
            print(f"skip {va:#x}: not a function start", file=sys.stderr)
            continue
        fc = FunctionCompiler(md, pe, va, sizes[va], set(sizes))
        funcs.append((va, fc.compile()))
        unsupported.update(fc.unsupported)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("/* Generated by tools/recomp/recomp.py from prototypeenginef.dll; do not edit. */\n")
        f.write('#include <math.h>\n#include "runtime.h"\n#include "runtime_fp.h"\n\n')
        for va, _ in funcs:
            f.write(f"void f_{va:08x}(Ctx* c);\n")
        f.write("\n")
        for _, text in funcs:
            f.write(text + "\n\n")
        f.write("const struct { uint32_t va; RecompFn fn; } g_recomp_table[] = {\n")
        for va, _ in funcs:
            f.write(f"    {{0x{va:08x}u, f_{va:08x}}},\n")
        f.write("};\nconst uint32_t g_recomp_count = sizeof(g_recomp_table) / sizeof(g_recomp_table[0]);\n")
    total = sum(unsupported.values())
    print(f"{len(funcs)} functions recompiled; {total} unsupported instructions")
    for k, v in unsupported.most_common(30):
        print(f"  {v:6d}  {k}")


if __name__ == "__main__":
    main()
