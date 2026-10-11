//! SSE/SSE2/SSE3/SSSE3 and MMX.
//!
//! Floating point follows x86 semantics bit-exactly: MXCSR rounding mode, FTZ
//! (tininess detected after rounding, like x86), DAZ, x86 NaN rules (first
//! operand's NaN wins and is quietened; invalid operations produce the
//! negative default QNaN), min/max returning the second operand on NaN or
//! equal, "integer indefinite" 0x80000000 for out-of-range conversions.
//! Round-to-nearest operations on ordinary operands use the host FPU; all
//! other cases go through a small exact software implementation.
//!
//! MXCSR exception flags (IE/DE/ZE/OE/UE/PE) are accumulated; PE is not
//! reported by the host fast path. Unmasked SIMD exceptions (#XM) and
//! alignment faults of aligned moves are not modelled.
//!
//! MMX registers live in `CpuState::mm`; an MMX instruction sets x87 top=0
//! and all tags valid like hardware (emms, handled in interp, empties them).

use crate::interp::{ExecResult, Step, ea, read_reg, write_reg};
use crate::mem;
use crate::state::flags::*;
use crate::state::{CpuState, EDI, Exit};
use iced_x86::{Instruction, Mnemonic, OpKind, Register};

// --- MXCSR ----------------------------------------------------------------------------------

const IE: u32 = 1 << 0;
const DE: u32 = 1 << 1;
const ZE: u32 = 1 << 2;
const OE: u32 = 1 << 3;
const UE: u32 = 1 << 4;
const PE: u32 = 1 << 5;
const DAZ: u32 = 1 << 6;
const FZ: u32 = 1 << 15;

const RN: u32 = 0;
const RD: u32 = 1;
const RU: u32 = 2;
const RZ: u32 = 3;

// --- register / operand access ----------------------------------------------------------------

#[inline]
fn xi(r: Register) -> usize {
    r as usize - Register::XMM0 as usize
}
#[inline]
fn mi(r: Register) -> usize {
    r as usize - Register::MM0 as usize
}
#[inline]
fn get_xmm(st: &CpuState, i: usize) -> u128 {
    u128::from_le_bytes(st.xmm[i])
}
#[inline]
fn set_xmm(st: &mut CpuState, i: usize, v: u128) {
    st.xmm[i] = v.to_le_bytes();
}
#[inline]
fn mmx_enter(st: &mut CpuState) {
    st.x87.top = 0;
    st.x87.empty = 0;
}

fn load(a: u32, n: usize) -> u128 {
    match n {
        16 => u128::from_le_bytes(mem::r128(a)),
        8 => mem::r64(a) as u128,
        4 => mem::r32(a) as u128,
        2 => mem::r16(a) as u128,
        1 => mem::r8(a) as u128,
        _ => {
            let mut v = 0u128;
            for i in 0..n.min(16) {
                v |= (mem::r8(a.wrapping_add(i as u32)) as u128) << (8 * i);
            }
            v
        }
    }
}

fn store(a: u32, n: usize, v: u128) {
    match n {
        16 => mem::w128(a, v.to_le_bytes()),
        8 => mem::w64(a, v as u64),
        4 => mem::w32(a, v as u32),
        2 => mem::w16(a, v as u16),
        1 => mem::w8(a, v as u8),
        _ => {
            for i in 0..n.min(16) {
                mem::w8(a.wrapping_add(i as u32), (v >> (8 * i)) as u8);
            }
        }
    }
}

fn reg_rd(st: &mut CpuState, r: Register) -> u128 {
    if r.is_xmm() {
        get_xmm(st, xi(r))
    } else if r.is_mm() {
        mmx_enter(st);
        st.mm[mi(r)] as u128
    } else {
        read_reg(st, r) as u128
    }
}

fn reg_wr(st: &mut CpuState, r: Register, v: u128) {
    if r.is_xmm() {
        set_xmm(st, xi(r), v)
    } else if r.is_mm() {
        mmx_enter(st);
        st.mm[mi(r)] = v as u64;
    } else {
        write_reg(st, r, v as u32)
    }
}

/// Operand i, zero-extended (memory operands read memory_size bytes).
fn rd(st: &mut CpuState, insn: &Instruction, i: u32) -> u128 {
    match insn.op_kind(i) {
        OpKind::Register => reg_rd(st, insn.op_register(i)),
        OpKind::Memory => load(ea(st, insn), insn.memory_size().size()),
        _ => insn.immediate(i) as u128,
    }
}

/// Write operand i (xmm: full register, mm: 64 bits, gpr: 32 bits, memory: memory_size bytes).
fn wr(st: &mut CpuState, insn: &Instruction, i: u32, v: u128) {
    match insn.op_kind(i) {
        OpKind::Register => reg_wr(st, insn.op_register(i), v),
        OpKind::Memory => store(ea(st, insn), insn.memory_size().size(), v),
        _ => {}
    }
}

/// Vector width in bytes of the instruction (16 for xmm, 8 for mm).
fn width(insn: &Instruction) -> u32 {
    for i in 0..insn.op_count() {
        if insn.op_kind(i) == OpKind::Register {
            let r = insn.op_register(i);
            if r.is_xmm() {
                return 16;
            }
            if r.is_mm() {
                return 8;
            }
        }
    }
    16
}

fn is_xmm_op(insn: &Instruction, i: u32) -> bool {
    insn.op_kind(i) == OpKind::Register && insn.op_register(i).is_xmm()
}

fn imm8(insn: &Instruction) -> u32 {
    for i in 0..insn.op_count() {
        if matches!(insn.op_kind(i), OpKind::Immediate8) {
            return insn.immediate8() as u32;
        }
    }
    0
}

// --- lanes ---------------------------------------------------------------------------------------

#[inline]
fn lmask(bits: u32) -> u128 {
    if bits >= 128 { !0 } else { (1u128 << bits) - 1 }
}
#[inline]
fn lane(v: u128, bits: u32, i: u32) -> u64 {
    ((v >> (bits * i)) & lmask(bits)) as u64
}
#[inline]
fn put(v: &mut u128, bits: u32, i: u32, x: u64) {
    let m = lmask(bits) << (bits * i);
    *v = (*v & !m) | (((x as u128) & lmask(bits)) << (bits * i));
}
#[inline]
fn sx(x: u64, bits: u32) -> i64 {
    ((x << (64 - bits)) as i64) >> (64 - bits)
}
#[inline]
fn sat_s(v: i64, bits: u32) -> u64 {
    let lo = -(1i64 << (bits - 1));
    let hi = (1i64 << (bits - 1)) - 1;
    v.clamp(lo, hi) as u64
}
#[inline]
fn sat_u(v: i64, bits: u32) -> u64 {
    v.clamp(0, (1i64 << bits) - 1) as u64
}

fn map2(a: u128, b: u128, w: u32, bits: u32, f: impl Fn(u64, u64) -> u64) -> u128 {
    let mut r = 0;
    for i in 0..w * 8 / bits {
        put(&mut r, bits, i, f(lane(a, bits, i), lane(b, bits, i)));
    }
    r
}

fn map1(b: u128, w: u32, bits: u32, f: impl Fn(u64) -> u64) -> u128 {
    let mut r = 0;
    for i in 0..w * 8 / bits {
        put(&mut r, bits, i, f(lane(b, bits, i)));
    }
    r
}

// --- floating point --------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
struct Fmt {
    /// Significand bits including the hidden bit.
    p: i32,
    bias: i32,
    /// Total width.
    w: u32,
}
const F32: Fmt = Fmt { p: 24, bias: 127, w: 32 };
const F64: Fmt = Fmt { p: 53, bias: 1023, w: 64 };

impl Fmt {
    #[inline]
    fn sign(self) -> u64 {
        1u64 << (self.w - 1)
    }
    #[inline]
    fn mbits(self) -> u32 {
        (self.p - 1) as u32
    }
    #[inline]
    fn emax_field(self) -> u64 {
        (1u64 << (self.w - self.mbits() - 1)) - 1
    }
    #[inline]
    fn exp_field(self, x: u64) -> u64 {
        (x >> self.mbits()) & self.emax_field()
    }
    #[inline]
    fn man(self, x: u64) -> u64 {
        x & ((1u64 << self.mbits()) - 1)
    }
    #[inline]
    fn is_nan(self, x: u64) -> bool {
        self.exp_field(x) == self.emax_field() && self.man(x) != 0
    }
    #[inline]
    fn is_snan(self, x: u64) -> bool {
        self.is_nan(x) && x & self.qbit() == 0
    }
    #[inline]
    fn is_denormal(self, x: u64) -> bool {
        self.exp_field(x) == 0 && self.man(x) != 0
    }
    #[inline]
    fn qbit(self) -> u64 {
        1u64 << (self.mbits() - 1)
    }
    #[inline]
    fn quiet(self, x: u64) -> u64 {
        x | self.qbit()
    }
    #[inline]
    fn default_nan(self) -> u64 {
        if self.w == 32 { 0xFFC0_0000 } else { 0xFFF8_0000_0000_0000 }
    }
    #[inline]
    fn inf(self, neg: bool) -> u64 {
        (self.emax_field() << self.mbits()) | if neg { self.sign() } else { 0 }
    }
    #[inline]
    fn zero(self, neg: bool) -> u64 {
        if neg { self.sign() } else { 0 }
    }
    #[inline]
    fn max_finite(self, neg: bool) -> u64 {
        self.inf(neg) - 1
    }
    #[inline]
    fn neg(self, x: u64) -> bool {
        x & self.sign() != 0
    }
    /// Finite, non-denormal (zero allowed): safe for the host fast path.
    #[inline]
    fn plain(self, x: u64) -> bool {
        let e = self.exp_field(x);
        e != self.emax_field() && (e != 0 || self.man(x) == 0)
    }
    /// Value as f64 (exact for both formats; not for NaN payloads).
    #[inline]
    fn to_f64(self, x: u64) -> f64 {
        if self.w == 32 { f32::from_bits(x as u32) as f64 } else { f64::from_bits(x) }
    }
}

#[derive(Clone, Copy)]
enum Cls {
    Nan,
    Inf,
    Zero,
    /// value = sig * 2^exp, sig != 0
    Fin(i32, u64),
}

struct Fp {
    rc: u32,
    ftz: bool,
    daz: bool,
    fl: u32,
}

impl Fp {
    fn new(mxcsr: u32) -> Fp {
        Fp { rc: (mxcsr >> 13) & 3, ftz: mxcsr & FZ != 0, daz: mxcsr & DAZ != 0, fl: 0 }
    }

    /// DAZ handling for operations that return an operand (min/max) or compare.
    fn flush_in(&mut self, f: Fmt, x: u64) -> u64 {
        if f.is_denormal(x) {
            if self.daz {
                return x & f.sign();
            }
            self.fl |= DE;
        }
        x
    }

    fn unpack(&mut self, f: Fmt, x: u64) -> Cls {
        let e = f.exp_field(x);
        let m = f.man(x);
        if e == f.emax_field() {
            if m == 0 { Cls::Inf } else { Cls::Nan }
        } else if e == 0 {
            if m == 0 {
                Cls::Zero
            } else if self.daz {
                Cls::Zero
            } else {
                self.fl |= DE;
                Cls::Fin(1 - f.bias - f.mbits() as i32, m)
            }
        } else {
            Cls::Fin(e as i32 - f.bias - f.mbits() as i32, m | (1u64 << f.mbits()))
        }
    }

    /// x86 NaN propagation for binary operations: first NaN operand, quietened.
    fn nan2(&mut self, f: Fmt, a: u64, b: u64) -> Option<u64> {
        let (na, nb) = (f.is_nan(a), f.is_nan(b));
        if !na && !nb {
            return None;
        }
        if f.is_snan(a) || f.is_snan(b) {
            self.fl |= IE;
        }
        Some(f.quiet(if na { a } else { b }))
    }

    fn invalid(&mut self, f: Fmt) -> u64 {
        self.fl |= IE;
        f.default_nan()
    }

    /// Round sig*2^exp to a multiple of 2^lsb. Returns (multiple, inexact).
    fn round_at(&self, sig: u128, exp: i32, lsb: i32, neg: bool, rc: u32) -> (u128, bool) {
        let shift = lsb as i64 - exp as i64;
        if shift <= 0 {
            return (sig << (-shift) as u32, false);
        }
        let (m, rem, half_cmp) = if shift > 128 {
            (0u128, sig, std::cmp::Ordering::Less)
        } else if shift == 128 {
            (0u128, sig, sig.cmp(&(1u128 << 127)))
        } else {
            let s = shift as u32;
            let rem = sig & ((1u128 << s) - 1);
            (sig >> s, rem, rem.cmp(&(1u128 << (s - 1))))
        };
        if rem == 0 {
            return (m, false);
        }
        let inc = match rc {
            RN => half_cmp == std::cmp::Ordering::Greater || (half_cmp == std::cmp::Ordering::Equal && m & 1 == 1),
            RD => neg,
            RU => !neg,
            _ => false,
        };
        (if inc { m + 1 } else { m }, true)
    }

    /// Round and encode sign * sig * 2^exp (sig != 0, may carry a sticky low bit).
    fn round_pack(&mut self, f: Fmt, neg: bool, exp: i32, sig: u128) -> u64 {
        debug_assert!(sig != 0);
        let p = f.p;
        let emin = 1 - f.bias;
        let emax = f.bias;
        let msb = 127 - sig.leading_zeros() as i32;
        let e = msb + exp;
        // x86 detects tininess after rounding (to unbounded exponent range).
        let tiny = if e < emin {
            let (m, _) = self.round_at(sig, exp, e - (p - 1), neg, self.rc);
            let e2 = if m >> p != 0 { e + 1 } else { e };
            e2 < emin
        } else {
            false
        };
        if tiny && self.ftz {
            self.fl |= UE | PE;
            return f.zero(neg);
        }
        let mut lsb = e.max(emin) - (p - 1);
        let (mut m, inexact) = self.round_at(sig, exp, lsb, neg, self.rc);
        if m >> p != 0 {
            m >>= 1;
            lsb += 1;
        }
        if inexact {
            self.fl |= PE;
            if tiny {
                self.fl |= UE;
            }
        }
        if m == 0 {
            return f.zero(neg);
        }
        let s = if neg { f.sign() } else { 0 };
        if m >> (p - 1) != 0 {
            let er = lsb + p - 1;
            if er > emax {
                self.fl |= OE | PE;
                return match self.rc {
                    RN => f.inf(neg),
                    RZ => f.max_finite(neg),
                    RD => if neg { f.inf(true) } else { f.max_finite(false) },
                    _ => if neg { f.max_finite(true) } else { f.inf(false) },
                };
            }
            s | (((er + f.bias) as u64) << f.mbits()) | (m as u64 & ((1u64 << f.mbits()) - 1))
        } else {
            s | m as u64
        }
    }

    // -- arithmetic ------------------------------------------------------------------------

    fn fast(&self, f: Fmt, a: u64, b: u64) -> bool {
        self.rc == RN && f.plain(a) && f.plain(b)
    }

    fn add(&mut self, f: Fmt, a: u64, b: u64, sub: bool) -> u64 {
        if self.fast(f, a, b) {
            let r = if f.w == 32 { fast_add::<f32>(self, a, b, sub) } else { fast_add::<f64>(self, a, b, sub) };
            if let Some(r) = r {
                return r;
            }
        }
        if let Some(n) = self.nan2(f, a, b) {
            return n;
        }
        let sa = f.neg(a);
        let sb = f.neg(b) ^ sub;
        let (ca, cb) = (self.unpack(f, a), self.unpack(f, b));
        match (ca, cb) {
            (Cls::Inf, Cls::Inf) => {
                if sa != sb { self.invalid(f) } else { f.inf(sa) }
            }
            (Cls::Inf, _) => f.inf(sa),
            (_, Cls::Inf) => f.inf(sb),
            (Cls::Zero, Cls::Zero) => {
                if sa == sb { f.zero(sa) } else { f.zero(self.rc == RD) }
            }
            (Cls::Zero, Cls::Fin(e, m)) => self.round_pack(f, sb, e, m as u128),
            (Cls::Fin(e, m), Cls::Zero) => self.round_pack(f, sa, e, m as u128),
            (Cls::Fin(ea_, ma), Cls::Fin(eb, mb)) => {
                let (mut xa, mut xb) = (ma as u128, mb as u128);
                let na = xa.leading_zeros() as i32 - 2; // msb at bit 125
                let nb = xb.leading_zeros() as i32 - 2;
                xa <<= na;
                xb <<= nb;
                let (ea2, eb2) = (ea_ - na, eb - nb);
                let exp;
                if ea2 >= eb2 {
                    xb = jam(xb, (ea2 - eb2) as u32);
                    exp = ea2;
                } else {
                    xa = jam(xa, (eb2 - ea2) as u32);
                    exp = eb2;
                }
                if sa == sb {
                    self.round_pack(f, sa, exp, xa + xb)
                } else if xa > xb {
                    self.round_pack(f, sa, exp, xa - xb)
                } else if xb > xa {
                    self.round_pack(f, sb, exp, xb - xa)
                } else {
                    f.zero(self.rc == RD)
                }
            }
            _ => unreachable!(),
        }
    }

    fn mul(&mut self, f: Fmt, a: u64, b: u64) -> u64 {
        if self.fast(f, a, b) {
            let r = if f.w == 32 { fast_mul::<f32>(self, a, b) } else { fast_mul::<f64>(self, a, b) };
            if let Some(r) = r {
                return r;
            }
        }
        if let Some(n) = self.nan2(f, a, b) {
            return n;
        }
        let s = f.neg(a) ^ f.neg(b);
        match (self.unpack(f, a), self.unpack(f, b)) {
            (Cls::Inf, Cls::Zero) | (Cls::Zero, Cls::Inf) => self.invalid(f),
            (Cls::Inf, _) | (_, Cls::Inf) => f.inf(s),
            (Cls::Zero, _) | (_, Cls::Zero) => f.zero(s),
            (Cls::Fin(ea_, ma), Cls::Fin(eb, mb)) => self.round_pack(f, s, ea_ + eb, ma as u128 * mb as u128),
            _ => unreachable!(),
        }
    }

    fn div(&mut self, f: Fmt, a: u64, b: u64) -> u64 {
        if self.fast(f, a, b) && f.exp_field(b) != 0 {
            let r = if f.w == 32 { fast_div::<f32>(self, a, b) } else { fast_div::<f64>(self, a, b) };
            if let Some(r) = r {
                return r;
            }
        }
        if let Some(n) = self.nan2(f, a, b) {
            return n;
        }
        let s = f.neg(a) ^ f.neg(b);
        match (self.unpack(f, a), self.unpack(f, b)) {
            (Cls::Inf, Cls::Inf) | (Cls::Zero, Cls::Zero) => self.invalid(f),
            (Cls::Inf, _) => f.inf(s),
            (_, Cls::Inf) => f.zero(s),
            (Cls::Zero, _) => f.zero(s),
            (_, Cls::Zero) => {
                self.fl |= ZE;
                f.inf(s)
            }
            (Cls::Fin(ea_, ma), Cls::Fin(eb, mb)) => {
                let na = ma.leading_zeros() as i32 - 11; // msb at bit 52
                let nb = mb.leading_zeros() as i32 - 11;
                let xa = (ma << na) as u128;
                let xb = (mb << nb) as u128;
                let n = xa << 74;
                let mut q = n / xb;
                if n % xb != 0 {
                    q |= 1;
                }
                self.round_pack(f, s, (ea_ - na) - (eb - nb) - 74, q)
            }
            _ => unreachable!(),
        }
    }

    fn sqrt(&mut self, f: Fmt, a: u64) -> u64 {
        if self.rc == RN && f.plain(a) && !f.neg(a) {
            let r = if f.w == 32 { fast_sqrt::<f32>(self, a) } else { fast_sqrt::<f64>(self, a) };
            if let Some(r) = r {
                return r;
            }
        }
        if f.is_nan(a) {
            if f.is_snan(a) {
                self.fl |= IE;
            }
            return f.quiet(a);
        }
        let neg = f.neg(a);
        match self.unpack(f, a) {
            Cls::Zero => f.zero(neg),
            _ if neg => self.invalid(f),
            Cls::Inf => f.inf(false),
            Cls::Fin(e, m) => {
                let nz = m.leading_zeros() as i32 - 11;
                let mut n = (m << nz) as u128;
                let mut e = e - nz;
                if e & 1 != 0 {
                    n <<= 1;
                    e -= 1;
                }
                n <<= 70;
                let r = isqrt(n);
                let r = if r * r != n { r | 1 } else { r };
                self.round_pack(f, false, (e - 70) / 2, r)
            }
            _ => unreachable!(),
        }
    }

    /// rcpps/rcpss. Hardware returns a 12-bit approximation; we return the
    /// correctly rounded reciprocal (within the architectural error bound).
    /// Like hardware: denormal inputs count as zero, tiny results flush to
    /// zero, MXCSR rounding/FTZ/DAZ are ignored and no exception flags are set.
    fn rcp(&mut self, a: u32) -> u32 {
        let f = F32;
        if f.is_nan(a as u64) {
            return a | 0x0040_0000;
        }
        let neg = a & 0x8000_0000 != 0;
        let a = if f.is_denormal(a as u64) { a & 0x8000_0000 } else { a };
        let r = (1.0 / f32::from_bits(a) as f64) as f32;
        // |x| >= 2^126: the hardware approximation is below 2^-126 and flushed
        if (r != 0.0 && !r.is_normal() && r.is_finite()) || (a >> 23) & 0xff >= 253 && (a >> 23) & 0xff != 255 {
            return if neg { 0x8000_0000 } else { 0 };
        }
        r.to_bits()
    }

    /// rsqrtps/rsqrtss: correctly rounded 1/sqrt(x), same conventions as `rcp`.
    fn rsqrt(&mut self, a: u32) -> u32 {
        let f = F32;
        if f.is_nan(a as u64) {
            return a | 0x0040_0000;
        }
        let a = if f.is_denormal(a as u64) { a & 0x8000_0000 } else { a };
        if a == 0x8000_0000 {
            return 0xff80_0000; // -0 -> -inf
        }
        if a & 0x8000_0000 != 0 {
            return 0xffc0_0000;
        }
        let x = f32::from_bits(a) as f64;
        ((1.0 / x.sqrt()) as f32).to_bits()
    }

    fn minmax(&mut self, f: Fmt, a: u64, b: u64, max: bool) -> u64 {
        let a = self.flush_in(f, a);
        let b = self.flush_in(f, b);
        if f.is_nan(a) || f.is_nan(b) {
            self.fl |= IE;
            return b;
        }
        let (x, y) = (f.to_f64(a), f.to_f64(b));
        if (max && x > y) || (!max && x < y) { a } else { b }
    }

    /// Returns None for unordered, else Some(ordering of a vs b).
    fn compare(&mut self, f: Fmt, a: u64, b: u64, quiet_ok: bool) -> Option<std::cmp::Ordering> {
        let a = self.flush_in(f, a);
        let b = self.flush_in(f, b);
        if f.is_nan(a) || f.is_nan(b) {
            if f.is_snan(a) || f.is_snan(b) || !quiet_ok {
                self.fl |= IE;
            }
            return None;
        }
        f.to_f64(a).partial_cmp(&f.to_f64(b))
    }

    fn cmp_pred(&mut self, f: Fmt, a: u64, b: u64, pred: u32) -> bool {
        use std::cmp::Ordering::*;
        let signaling = matches!(pred & 7, 1 | 2 | 5 | 6);
        let o = self.compare(f, a, b, !signaling);
        let (un, lt, eq) = match o {
            None => (true, false, false),
            Some(Less) => (false, true, false),
            Some(Equal) => (false, false, true),
            Some(Greater) => (false, false, false),
        };
        match pred & 7 {
            0 => eq,
            1 => lt,
            2 => lt || eq,
            3 => un,
            4 => !eq,
            5 => !lt,
            6 => !(lt || eq),
            _ => !un,
        }
    }

    // -- conversions ------------------------------------------------------------------------

    fn cvt_ff(&mut self, from: Fmt, to: Fmt, x: u64) -> u64 {
        let neg = from.neg(x);
        if from.is_nan(x) {
            if from.is_snan(x) {
                self.fl |= IE;
            }
            let payload = from.man(x);
            let p = if to.mbits() > from.mbits() {
                payload << (to.mbits() - from.mbits())
            } else {
                payload >> (from.mbits() - to.mbits())
            };
            return to.quiet(to.inf(neg) | p);
        }
        match self.unpack(from, x) {
            Cls::Inf => to.inf(neg),
            Cls::Zero => to.zero(neg),
            Cls::Fin(e, m) => self.round_pack(to, neg, e, m as u128),
            Cls::Nan => unreachable!(),
        }
    }

    fn from_i32(&mut self, to: Fmt, v: i32) -> u64 {
        if v == 0 {
            return 0;
        }
        self.round_pack(to, v < 0, 0, v.unsigned_abs() as u128)
    }

    fn to_i32(&mut self, f: Fmt, x: u64, trunc: bool) -> u32 {
        let neg = f.neg(x);
        let de = self.fl & DE; // float->int conversions do not report denormal operands
        let c = self.unpack(f, x);
        self.fl = (self.fl & !DE) | de;
        match c {
            Cls::Nan | Cls::Inf => {
                self.fl |= IE;
                0x8000_0000
            }
            Cls::Zero => 0,
            Cls::Fin(e, m) => {
                if e > 40 {
                    self.fl |= IE;
                    return 0x8000_0000;
                }
                let rc = if trunc { RZ } else { self.rc };
                let (r, inexact) = self.round_at(m as u128, e, 0, neg, rc);
                if r > 0x8000_0000 || (r == 0x8000_0000 && !neg) {
                    self.fl |= IE;
                    return 0x8000_0000;
                }
                if inexact {
                    self.fl |= PE;
                }
                if neg { (r as u32).wrapping_neg() } else { r as u32 }
            }
        }
    }
}

// --- host fast paths (round-to-nearest, normal operands and results) ---------------------------
// The inexact flag comes from exact error terms (TwoSum / fused multiply-add),
// valid because operands and results are kept away from the subnormal range.

trait Hf: Copy + PartialEq + std::ops::Add<Output = Self> + std::ops::Sub<Output = Self>
    + std::ops::Mul<Output = Self> + std::ops::Div<Output = Self> + std::ops::Neg<Output = Self>
{
    const FMT: Fmt;
    const ZERO: Self;
    fn fb(b: u64) -> Self;
    fn tb(self) -> u64;
    fn fma(self, b: Self, c: Self) -> Self;
    fn sq(self) -> Self;
    fn norm(self) -> bool;
    /// Unbiased exponent (for normal values).
    fn uexp(self) -> i32 {
        let f = Self::FMT;
        f.exp_field(self.tb()) as i32 - f.bias
    }
}
impl Hf for f32 {
    const FMT: Fmt = F32;
    const ZERO: Self = 0.0;
    fn fb(b: u64) -> Self { f32::from_bits(b as u32) }
    fn tb(self) -> u64 { self.to_bits() as u64 }
    fn fma(self, b: Self, c: Self) -> Self { self.mul_add(b, c) }
    fn sq(self) -> Self { self.sqrt() }
    fn norm(self) -> bool { self.is_normal() }
}
impl Hf for f64 {
    const FMT: Fmt = F64;
    const ZERO: Self = 0.0;
    fn fb(b: u64) -> Self { f64::from_bits(b) }
    fn tb(self) -> u64 { self.to_bits() }
    fn fma(self, b: Self, c: Self) -> Self { self.mul_add(b, c) }
    fn sq(self) -> Self { self.sqrt() }
    fn norm(self) -> bool { self.is_normal() }
}

/// Smallest exponent for which error terms are exactly representable.
fn safe_exp<T: Hf>(k: i32) -> i32 {
    1 - T::FMT.bias + k * T::FMT.p
}

fn fast_add<T: Hf>(fp: &mut Fp, a: u64, b: u64, sub: bool) -> Option<u64> {
    let x = T::fb(a);
    let y = if sub { -T::fb(b) } else { T::fb(b) };
    let s = x + y;
    if !(s.norm() || s == T::ZERO) {
        return None;
    }
    let bb = s - x;
    let err = (x - (s - bb)) + (y - bb);
    if err != T::ZERO {
        fp.fl |= PE;
    }
    Some(s.tb())
}

fn fast_mul<T: Hf>(fp: &mut Fp, a: u64, b: u64) -> Option<u64> {
    let (x, y) = (T::fb(a), T::fb(b));
    let p = x * y;
    if p == T::ZERO {
        return if x == T::ZERO || y == T::ZERO { Some(p.tb()) } else { None };
    }
    if !p.norm() || p.uexp() < safe_exp::<T>(1) {
        return None;
    }
    if x.fma(y, -p) != T::ZERO {
        fp.fl |= PE;
    }
    Some(p.tb())
}

fn fast_div<T: Hf>(fp: &mut Fp, a: u64, b: u64) -> Option<u64> {
    let (x, y) = (T::fb(a), T::fb(b));
    let q = x / y;
    if x == T::ZERO {
        return Some(q.tb());
    }
    if !q.norm() || q.uexp() < safe_exp::<T>(1) || x.uexp() < safe_exp::<T>(2) {
        return None;
    }
    if (-q).fma(y, x) != T::ZERO {
        fp.fl |= PE;
    }
    Some(q.tb())
}

fn fast_sqrt<T: Hf>(fp: &mut Fp, a: u64) -> Option<u64> {
    let x = T::fb(a);
    if x == T::ZERO {
        return Some(a);
    }
    if x.uexp() < safe_exp::<T>(2) {
        return None;
    }
    let r = x.sq();
    if (-r).fma(r, x) != T::ZERO {
        fp.fl |= PE;
    }
    Some(r.tb())
}

fn jam(x: u128, d: u32) -> u128 {
    if d == 0 {
        x
    } else if d >= 128 {
        (x != 0) as u128
    } else {
        (x >> d) | ((x & ((1u128 << d) - 1) != 0) as u128)
    }
}

fn isqrt(n: u128) -> u128 {
    if n == 0 {
        return 0;
    }
    // Newton iteration from an f64 estimate.
    let mut x = (n as f64).sqrt() as u128;
    if x == 0 {
        x = 1;
    }
    loop {
        let y = (x + n / x) >> 1;
        if y >= x {
            break;
        }
        x = y;
    }
    while x * x > n {
        x -= 1;
    }
    while (x + 1) * (x + 1) <= n {
        x += 1;
    }
    x
}

// --- float instruction helpers ---------------------------------------------------------------

/// Binary float op: packed (all lanes) or scalar (lane 0, upper lanes of dst kept).
fn fp_bin(st: &mut CpuState, insn: &Instruction, f: Fmt, scalar: bool, op: impl Fn(&mut Fp, u64, u64) -> u64) {
    let d = xi(insn.op0_register());
    let a = get_xmm(st, d);
    let b = rd(st, insn, 1);
    let mut fp = Fp::new(st.mxcsr);
    let n = if scalar { 1 } else { 128 / f.w };
    let mut r = a;
    for i in 0..n {
        put(&mut r, f.w, i, op(&mut fp, lane(a, f.w, i), lane(b, f.w, i)));
    }
    set_xmm(st, d, r);
    st.mxcsr |= fp.fl;
}

fn fp_un(st: &mut CpuState, insn: &Instruction, f: Fmt, scalar: bool, op: impl Fn(&mut Fp, u64) -> u64) {
    let d = xi(insn.op0_register());
    let a = get_xmm(st, d);
    let b = rd(st, insn, 1);
    let mut fp = Fp::new(st.mxcsr);
    let n = if scalar { 1 } else { 128 / f.w };
    let mut r = a;
    for i in 0..n {
        put(&mut r, f.w, i, op(&mut fp, lane(b, f.w, i)));
    }
    set_xmm(st, d, r);
    st.mxcsr |= fp.fl;
}

// --- integer helpers ----------------------------------------------------------------------------

fn shift_count(st: &mut CpuState, insn: &Instruction) -> u64 {
    match insn.op1_kind() {
        OpKind::Immediate8 => insn.immediate8() as u64,
        _ => rd(st, insn, 1) as u64,
    }
}

fn pack(a: u128, b: u128, w: u32, bits_in: u32, f: impl Fn(i64) -> u64) -> u128 {
    let n = w * 8 / bits_in;
    let bo = bits_in / 2;
    let mut r = 0;
    for i in 0..n {
        put(&mut r, bo, i, f(sx(lane(a, bits_in, i), bits_in)));
        put(&mut r, bo, n + i, f(sx(lane(b, bits_in, i), bits_in)));
    }
    r
}

fn unpack_lanes(a: u128, b: u128, w: u32, bits: u32, high: bool) -> u128 {
    let n = w * 8 / bits / 2;
    let off = if high { n } else { 0 };
    let mut r = 0;
    for i in 0..n {
        put(&mut r, bits, 2 * i, lane(a, bits, off + i));
        put(&mut r, bits, 2 * i + 1, lane(b, bits, off + i));
    }
    r
}

fn horiz(a: u128, b: u128, w: u32, bits: u32, f: impl Fn(u64, u64) -> u64) -> u128 {
    let n = w * 8 / bits;
    let h = n / 2;
    let mut r = 0;
    for i in 0..h {
        put(&mut r, bits, i, f(lane(a, bits, 2 * i), lane(a, bits, 2 * i + 1)));
        put(&mut r, bits, h + i, f(lane(b, bits, 2 * i), lane(b, bits, 2 * i + 1)));
    }
    r
}

/// Generic "dst = f(dst, src)" for integer SIMD on mm or xmm.
fn int2(st: &mut CpuState, insn: &Instruction, f: impl Fn(u128, u128, u32) -> u128) {
    let w = width(insn);
    let a = rd(st, insn, 0);
    let b = rd(st, insn, 1);
    let r = f(a, b, w) & lmask(w * 8);
    wr(st, insn, 0, r);
}

// --- dispatcher -----------------------------------------------------------------------------------

/// Execute an SSE/MMX instruction; None if `insn` is not handled here.
pub fn exec(st: &mut CpuState, insn: &Instruction) -> Option<ExecResult> {
    use Mnemonic::*;
    let m = insn.mnemonic();
    match m {
        // ---- data movement ----
        Movaps | Movups | Movapd | Movupd | Movdqa | Movdqu | Lddqu | Movntps | Movntpd | Movntdq | Movntq
        | Movnti => {
            let v = rd(st, insn, 1);
            wr(st, insn, 0, v);
        }
        Movq | Movd => {
            let v = rd(st, insn, 1);
            let v = if m == Movq { v & lmask(64) } else { v & lmask(32) };
            wr(st, insn, 0, v);
        }
        Movq2dq | Movdq2q => {
            let v = rd(st, insn, 1) & lmask(64);
            wr(st, insn, 0, v);
        }
        Movss | Movsd => {
            if insn.op_count() != 2 || !(is_xmm_op(insn, 0) || is_xmm_op(insn, 1)) {
                return None; // string movsd is handled by the integer core
            }
            let bits = if m == Movss { 32 } else { 64 };
            let v = rd(st, insn, 1) & lmask(bits);
            if is_xmm_op(insn, 0) && is_xmm_op(insn, 1) {
                let d = xi(insn.op0_register());
                let mut a = get_xmm(st, d);
                put(&mut a, bits, 0, v as u64);
                set_xmm(st, d, a);
            } else {
                wr(st, insn, 0, v); // load zero-extends, store writes the low element
            }
        }
        Movlps | Movlpd | Movhps | Movhpd => {
            let high = matches!(m, Movhps | Movhpd);
            if is_xmm_op(insn, 0) {
                let d = xi(insn.op0_register());
                let mut a = get_xmm(st, d);
                let v = rd(st, insn, 1) as u64;
                put(&mut a, 64, high as u32, v);
                set_xmm(st, d, a);
            } else {
                let v = rd(st, insn, 1);
                wr(st, insn, 0, if high { v >> 64 } else { v });
            }
        }
        Movhlps | Movlhps => {
            let d = xi(insn.op0_register());
            let mut a = get_xmm(st, d);
            let b = rd(st, insn, 1);
            if m == Movhlps {
                put(&mut a, 64, 0, lane(b, 64, 1));
            } else {
                put(&mut a, 64, 1, lane(b, 64, 0));
            }
            set_xmm(st, d, a);
        }
        Movmskps | Movmskpd | Pmovmskb => {
            let b = rd(st, insn, 1);
            let bits = match m {
                Movmskps => 32,
                Movmskpd => 64,
                _ => 8,
            };
            let n = width(insn) * 8 / bits;
            let mut r = 0u32;
            for i in 0..n {
                r |= ((lane(b, bits, i) >> (bits - 1)) as u32 & 1) << i;
            }
            wr(st, insn, 0, r as u128);
        }
        Maskmovq | Maskmovdqu => {
            let mut regs = (0..insn.op_count())
                .filter(|&i| insn.op_kind(i) == OpKind::Register)
                .map(|i| insn.op_register(i));
            let (rv, rm) = (regs.next()?, regs.next()?);
            let v = reg_rd(st, rv);
            let mk = reg_rd(st, rm);
            let addr = if (0..insn.op_count()).any(|i| insn.op_kind(i) == OpKind::MemorySegDI) {
                st.gpr[EDI] & 0xffff
            } else {
                st.gpr[EDI]
            };
            let n = if rv.is_xmm() { 16 } else { 8 };
            for i in 0..n {
                if lane(mk, 8, i) & 0x80 != 0 {
                    mem::w8(addr.wrapping_add(i), lane(v, 8, i) as u8);
                }
            }
        }
        Movsldup | Movshdup => {
            let b = rd(st, insn, 1);
            let o = (m == Movshdup) as u32;
            let mut r = 0;
            for i in 0..2 {
                let x = lane(b, 32, 2 * i + o);
                put(&mut r, 32, 2 * i, x);
                put(&mut r, 32, 2 * i + 1, x);
            }
            wr(st, insn, 0, r);
        }
        Movddup => {
            let b = lane(rd(st, insn, 1), 64, 0) as u128;
            wr(st, insn, 0, b | (b << 64));
        }
        Stmxcsr => {
            let v = st.mxcsr;
            mem::w32(ea(st, insn), v);
        }
        Ldmxcsr => {
            let v = mem::r32(ea(st, insn));
            if v & 0xffff_0000 != 0 {
                return Some(Err(Exit::Fault));
            }
            st.mxcsr = v;
        }

        // ---- float arithmetic ----
        Addps | Addss | Addpd | Addsd | Subps | Subss | Subpd | Subsd | Mulps | Mulss | Mulpd | Mulsd | Divps
        | Divss | Divpd | Divsd | Minps | Minss | Minpd | Minsd | Maxps | Maxss | Maxpd | Maxsd => {
            let f = if matches!(m, Addpd | Addsd | Subpd | Subsd | Mulpd | Mulsd | Divpd | Divsd | Minpd | Minsd | Maxpd | Maxsd) {
                F64
            } else {
                F32
            };
            let scalar = matches!(m, Addss | Addsd | Subss | Subsd | Mulss | Mulsd | Divss | Divsd | Minss | Minsd | Maxss | Maxsd);
            match m {
                Addps | Addss | Addpd | Addsd => fp_bin(st, insn, f, scalar, |fp, a, b| fp.add(f, a, b, false)),
                Subps | Subss | Subpd | Subsd => fp_bin(st, insn, f, scalar, |fp, a, b| fp.add(f, a, b, true)),
                Mulps | Mulss | Mulpd | Mulsd => fp_bin(st, insn, f, scalar, |fp, a, b| fp.mul(f, a, b)),
                Divps | Divss | Divpd | Divsd => fp_bin(st, insn, f, scalar, |fp, a, b| fp.div(f, a, b)),
                Minps | Minss | Minpd | Minsd => fp_bin(st, insn, f, scalar, |fp, a, b| fp.minmax(f, a, b, false)),
                _ => fp_bin(st, insn, f, scalar, |fp, a, b| fp.minmax(f, a, b, true)),
            }
        }
        Sqrtps | Sqrtss | Sqrtpd | Sqrtsd => {
            let f = if matches!(m, Sqrtpd | Sqrtsd) { F64 } else { F32 };
            fp_un(st, insn, f, matches!(m, Sqrtss | Sqrtsd), |fp, a| fp.sqrt(f, a));
        }
        Rcpps | Rcpss => fp_un(st, insn, F32, m == Rcpss, |fp, a| fp.rcp(a as u32) as u64),
        Rsqrtps | Rsqrtss => fp_un(st, insn, F32, m == Rsqrtss, |fp, a| fp.rsqrt(a as u32) as u64),
        Addsubps | Addsubpd => {
            let f = if m == Addsubpd { F64 } else { F32 };
            let d = xi(insn.op0_register());
            let a = get_xmm(st, d);
            let b = rd(st, insn, 1);
            let mut fp = Fp::new(st.mxcsr);
            let mut r = 0;
            for i in 0..128 / f.w {
                put(&mut r, f.w, i, fp.add(f, lane(a, f.w, i), lane(b, f.w, i), i % 2 == 0));
            }
            set_xmm(st, d, r);
            st.mxcsr |= fp.fl;
        }
        Haddps | Haddpd | Hsubps | Hsubpd => {
            let f = if matches!(m, Haddpd | Hsubpd) { F64 } else { F32 };
            let sub = matches!(m, Hsubps | Hsubpd);
            let d = xi(insn.op0_register());
            let a = get_xmm(st, d);
            let b = rd(st, insn, 1);
            let mut fp = Fp::new(st.mxcsr);
            let n = 128 / f.w;
            let h = n / 2;
            let mut r = 0;
            for i in 0..h {
                put(&mut r, f.w, i, fp.add(f, lane(a, f.w, 2 * i), lane(a, f.w, 2 * i + 1), sub));
            }
            for i in 0..h {
                put(&mut r, f.w, h + i, fp.add(f, lane(b, f.w, 2 * i), lane(b, f.w, 2 * i + 1), sub));
            }
            set_xmm(st, d, r);
            st.mxcsr |= fp.fl;
        }

        // ---- compares ----
        Cmpps | Cmpss | Cmppd | Cmpsd => {
            if !is_xmm_op(insn, 0) {
                return None; // string cmpsd
            }
            let f = if matches!(m, Cmppd | Cmpsd) { F64 } else { F32 };
            let pred = imm8(insn);
            let ones = lmask(f.w) as u64;
            fp_bin(st, insn, f, matches!(m, Cmpss | Cmpsd), |fp, a, b| {
                if fp.cmp_pred(f, a, b, pred) { ones } else { 0 }
            });
        }
        Comiss | Ucomiss | Comisd | Ucomisd => {
            let f = if matches!(m, Comisd | Ucomisd) { F64 } else { F32 };
            let a = lane(rd(st, insn, 0), f.w, 0);
            let b = lane(rd(st, insn, 1), f.w, 0);
            let mut fp = Fp::new(st.mxcsr);
            let fl = match fp.compare(f, a, b, matches!(m, Ucomiss | Ucomisd)) {
                None => ZF | PF | CF,
                Some(std::cmp::Ordering::Less) => CF,
                Some(std::cmp::Ordering::Equal) => ZF,
                Some(std::cmp::Ordering::Greater) => 0,
            };
            st.eflags = (st.eflags & !ARITH) | fl;
            st.mxcsr |= fp.fl;
        }

        // ---- logic ----
        Andps | Andpd | Pand => int2(st, insn, |a, b, _| a & b),
        Andnps | Andnpd | Pandn => int2(st, insn, |a, b, _| !a & b),
        Orps | Orpd | Por => int2(st, insn, |a, b, _| a | b),
        Xorps | Xorpd | Pxor => int2(st, insn, |a, b, _| a ^ b),

        // ---- shuffles / unpacks ----
        Shufps => {
            let i = imm8(insn);
            int2(st, insn, |a, b, _| {
                let mut r = 0;
                put(&mut r, 32, 0, lane(a, 32, i & 3));
                put(&mut r, 32, 1, lane(a, 32, (i >> 2) & 3));
                put(&mut r, 32, 2, lane(b, 32, (i >> 4) & 3));
                put(&mut r, 32, 3, lane(b, 32, (i >> 6) & 3));
                r
            })
        }
        Shufpd => {
            let i = imm8(insn);
            int2(st, insn, |a, b, _| lane(a, 64, i & 1) as u128 | (lane(b, 64, (i >> 1) & 1) as u128) << 64)
        }
        Pshufd | Pshufw => {
            let i = imm8(insn);
            let bits = if m == Pshufd { 32 } else { 16 };
            int2(st, insn, |_, b, _| {
                let mut r = 0;
                for k in 0..4 {
                    put(&mut r, bits, k, lane(b, bits, (i >> (2 * k)) & 3));
                }
                r
            })
        }
        Pshufhw | Pshuflw => {
            let i = imm8(insn);
            let o = if m == Pshufhw { 4 } else { 0 };
            int2(st, insn, |_, b, _| {
                let mut r = b;
                for k in 0..4 {
                    put(&mut r, 16, o + k, lane(b, 16, o + ((i >> (2 * k)) & 3)));
                }
                r
            })
        }
        Pshufb => int2(st, insn, |a, b, w| {
            let mut r = 0;
            for k in 0..w {
                let idx = lane(b, 8, k);
                if idx & 0x80 == 0 {
                    put(&mut r, 8, k, lane(a, 8, idx as u32 & (w - 1)));
                }
            }
            r
        }),
        Palignr => {
            let sh = imm8(insn);
            int2(st, insn, |a, b, w| {
                let mut r = 0;
                for k in 0..w {
                    let j = sh + k;
                    let byte = if j < w {
                        lane(b, 8, j)
                    } else if j < 2 * w {
                        lane(a, 8, j - w)
                    } else {
                        0
                    };
                    put(&mut r, 8, k, byte);
                }
                r
            })
        }
        Unpcklps | Punpckldq => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 32, false)),
        Unpckhps | Punpckhdq => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 32, true)),
        Unpcklpd | Punpcklqdq => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 64, false)),
        Unpckhpd | Punpckhqdq => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 64, true)),
        Punpcklbw => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 8, false)),
        Punpckhbw => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 8, true)),
        Punpcklwd => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 16, false)),
        Punpckhwd => int2(st, insn, |a, b, w| unpack_lanes(a, b, w, 16, true)),
        Pextrw => {
            if insn.op0_kind() != OpKind::Register || insn.code() == iced_x86::Code::Pextrw_r32m16_xmm_imm8 {
                return None; // 66 0F 3A 15 is SSE4.1
            }
            let src = insn.op_register(1);
            let v = reg_rd(st, src);
            let n = if src.is_xmm() { 8 } else { 4 };
            let x = lane(v, 16, imm8(insn) & (n - 1));
            wr(st, insn, 0, x as u128);
        }
        Pinsrw => {
            let w = width(insn);
            let n = w / 2;
            let mut a = rd(st, insn, 0);
            let x = rd(st, insn, 1) as u64 & 0xffff;
            put(&mut a, 16, imm8(insn) & (n - 1), x);
            wr(st, insn, 0, a);
        }

        // ---- conversions ----
        Cvtsi2ss | Cvtsi2sd => {
            let f = if m == Cvtsi2sd { F64 } else { F32 };
            let d = xi(insn.op0_register());
            let v = rd(st, insn, 1) as u32 as i32;
            let mut fp = Fp::new(st.mxcsr);
            let mut a = get_xmm(st, d);
            put(&mut a, f.w, 0, fp.from_i32(f, v));
            set_xmm(st, d, a);
            st.mxcsr |= fp.fl;
        }
        Cvtss2si | Cvttss2si | Cvtsd2si | Cvttsd2si => {
            let f = if matches!(m, Cvtsd2si | Cvttsd2si) { F64 } else { F32 };
            let x = lane(rd(st, insn, 1), f.w, 0);
            let mut fp = Fp::new(st.mxcsr);
            let r = fp.to_i32(f, x, matches!(m, Cvttss2si | Cvttsd2si));
            st.mxcsr |= fp.fl;
            wr(st, insn, 0, r as u128);
        }
        Cvtps2pd | Cvtpd2ps | Cvtss2sd | Cvtsd2ss => {
            let (from, to) = if matches!(m, Cvtps2pd | Cvtss2sd) { (F32, F64) } else { (F64, F32) };
            let d = xi(insn.op0_register());
            let b = rd(st, insn, 1);
            let mut fp = Fp::new(st.mxcsr);
            let mut r = match m {
                Cvtss2sd | Cvtsd2ss => get_xmm(st, d),
                _ => 0,
            };
            let n = if matches!(m, Cvtss2sd | Cvtsd2ss) { 1 } else { 2 };
            for i in 0..n {
                put(&mut r, to.w, i, fp.cvt_ff(from, to, lane(b, from.w, i)));
            }
            set_xmm(st, d, r);
            st.mxcsr |= fp.fl;
        }
        Cvtdq2ps | Cvtdq2pd | Cvtpi2ps | Cvtpi2pd => {
            let to = if matches!(m, Cvtdq2pd | Cvtpi2pd) { F64 } else { F32 };
            let d = xi(insn.op0_register());
            let b = rd(st, insn, 1);
            let mut fp = Fp::new(st.mxcsr);
            let mut r = if m == Cvtpi2ps { get_xmm(st, d) } else { 0 };
            let n = if m == Cvtdq2ps { 4 } else { 2 };
            for i in 0..n {
                put(&mut r, to.w, i, fp.from_i32(to, lane(b, 32, i) as u32 as i32));
            }
            set_xmm(st, d, r);
            st.mxcsr |= fp.fl;
        }
        Cvtps2dq | Cvttps2dq | Cvtpd2dq | Cvttpd2dq | Cvtps2pi | Cvttps2pi | Cvtpd2pi | Cvttpd2pi => {
            let from = if matches!(m, Cvtpd2dq | Cvttpd2dq | Cvtpd2pi | Cvttpd2pi) { F64 } else { F32 };
            let trunc = matches!(m, Cvttps2dq | Cvttpd2dq | Cvttps2pi | Cvttpd2pi);
            let n = if matches!(m, Cvtps2dq | Cvttps2dq) { 4 } else { 2 };
            let b = rd(st, insn, 1);
            let mut fp = Fp::new(st.mxcsr);
            let mut r = 0;
            for i in 0..n {
                put(&mut r, 32, i, fp.to_i32(from, lane(b, from.w, i), trunc) as u64);
            }
            st.mxcsr |= fp.fl;
            wr(st, insn, 0, r);
        }

        // ---- integer SIMD ----
        Paddb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| x.wrapping_add(y))),
        Paddw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| x.wrapping_add(y))),
        Paddd => int2(st, insn, |a, b, w| map2(a, b, w, 32, |x, y| x.wrapping_add(y))),
        Paddq => int2(st, insn, |a, b, w| map2(a, b, w, 64, |x, y| x.wrapping_add(y))),
        Psubb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| x.wrapping_sub(y))),
        Psubw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| x.wrapping_sub(y))),
        Psubd => int2(st, insn, |a, b, w| map2(a, b, w, 32, |x, y| x.wrapping_sub(y))),
        Psubq => int2(st, insn, |a, b, w| map2(a, b, w, 64, |x, y| x.wrapping_sub(y))),
        Paddsb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| sat_s(sx(x, 8) + sx(y, 8), 8))),
        Paddsw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sat_s(sx(x, 16) + sx(y, 16), 16))),
        Psubsb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| sat_s(sx(x, 8) - sx(y, 8), 8))),
        Psubsw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sat_s(sx(x, 16) - sx(y, 16), 16))),
        Paddusb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| sat_u(x as i64 + y as i64, 8))),
        Paddusw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sat_u(x as i64 + y as i64, 16))),
        Psubusb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| sat_u(x as i64 - y as i64, 8))),
        Psubusw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sat_u(x as i64 - y as i64, 16))),
        Pmullw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| (sx(x, 16) * sx(y, 16)) as u64)),
        Pmulhw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| ((sx(x, 16) * sx(y, 16)) >> 16) as u64)),
        Pmulhuw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| (x * y) >> 16)),
        Pmulhrsw => int2(st, insn, |a, b, w| {
            map2(a, b, w, 16, |x, y| ((((sx(x, 16) * sx(y, 16)) >> 14) + 1) >> 1) as u64)
        }),
        Pmuludq => int2(st, insn, |a, b, w| map2(a, b, w, 64, |x, y| (x & 0xffff_ffff) * (y & 0xffff_ffff))),
        Pmaddwd => int2(st, insn, |a, b, w| {
            map2(a, b, w, 32, |x, y| {
                (sx(x & 0xffff, 16) * sx(y & 0xffff, 16) + sx(x >> 16, 16) * sx(y >> 16, 16)) as u64
            })
        }),
        Pmaddubsw => int2(st, insn, |a, b, w| {
            map2(a, b, w, 16, |x, y| {
                sat_s((x & 0xff) as i64 * sx(y & 0xff, 8) + (x >> 8) as i64 * sx(y >> 8, 8), 16)
            })
        }),
        Pavgb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| (x + y + 1) >> 1)),
        Pavgw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| (x + y + 1) >> 1)),
        Pminub => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| x.min(y))),
        Pmaxub => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| x.max(y))),
        Pminsw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sx(x, 16).min(sx(y, 16)) as u64)),
        Pmaxsw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| sx(x, 16).max(sx(y, 16)) as u64)),
        Psadbw => int2(st, insn, |a, b, w| {
            map2(a, b, w, 64, |x, y| (0..8).map(|k| ((x >> (8 * k)) as u8).abs_diff((y >> (8 * k)) as u8) as u64).sum())
        }),
        Pcmpeqb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| if x == y { !0 } else { 0 })),
        Pcmpeqw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| if x == y { !0 } else { 0 })),
        Pcmpeqd => int2(st, insn, |a, b, w| map2(a, b, w, 32, |x, y| if x == y { !0 } else { 0 })),
        Pcmpgtb => int2(st, insn, |a, b, w| map2(a, b, w, 8, |x, y| if sx(x, 8) > sx(y, 8) { !0 } else { 0 })),
        Pcmpgtw => int2(st, insn, |a, b, w| map2(a, b, w, 16, |x, y| if sx(x, 16) > sx(y, 16) { !0 } else { 0 })),
        Pcmpgtd => int2(st, insn, |a, b, w| map2(a, b, w, 32, |x, y| if sx(x, 32) > sx(y, 32) { !0 } else { 0 })),
        Psllw | Pslld | Psllq | Psrlw | Psrld | Psrlq | Psraw | Psrad => {
            let bits = match m {
                Psllw | Psrlw | Psraw => 16,
                Pslld | Psrld | Psrad => 32,
                _ => 64,
            };
            let c = shift_count(st, insn);
            let w = width(insn);
            let a = rd(st, insn, 0);
            let r = match m {
                Psllw | Pslld | Psllq => map1(a, w, bits, |x| if c >= bits as u64 { 0 } else { x << c }),
                Psrlw | Psrld | Psrlq => map1(a, w, bits, |x| if c >= bits as u64 { 0 } else { x >> c }),
                _ => map1(a, w, bits, |x| (sx(x, bits) >> c.min(bits as u64 - 1)) as u64),
            };
            wr(st, insn, 0, r);
        }
        Pslldq | Psrldq => {
            let c = imm8(insn);
            let d = xi(insn.op0_register());
            let a = get_xmm(st, d);
            let r = if c > 15 {
                0
            } else if m == Pslldq {
                a << (8 * c)
            } else {
                a >> (8 * c)
            };
            set_xmm(st, d, r);
        }
        Packsswb => int2(st, insn, |a, b, w| pack(a, b, w, 16, |v| sat_s(v, 8))),
        Packssdw => int2(st, insn, |a, b, w| pack(a, b, w, 32, |v| sat_s(v, 16))),
        Packuswb => int2(st, insn, |a, b, w| pack(a, b, w, 16, |v| sat_u(v, 8))),
        Pabsb | Pabsw | Pabsd => {
            let bits = match m {
                Pabsb => 8,
                Pabsw => 16,
                _ => 32,
            };
            int2(st, insn, |_, b, w| map1(b, w, bits, |x| sx(x, bits).unsigned_abs()))
        }
        Psignb | Psignw | Psignd => {
            let bits = match m {
                Psignb => 8,
                Psignw => 16,
                _ => 32,
            };
            int2(st, insn, |a, b, w| {
                map2(a, b, w, bits, |x, y| {
                    let s = sx(y, bits);
                    if s < 0 {
                        x.wrapping_neg()
                    } else if s == 0 {
                        0
                    } else {
                        x
                    }
                })
            })
        }
        Phaddw => int2(st, insn, |a, b, w| horiz(a, b, w, 16, |x, y| x.wrapping_add(y))),
        Phaddd => int2(st, insn, |a, b, w| horiz(a, b, w, 32, |x, y| x.wrapping_add(y))),
        Phaddsw => int2(st, insn, |a, b, w| horiz(a, b, w, 16, |x, y| sat_s(sx(x, 16) + sx(y, 16), 16))),
        Phsubw => int2(st, insn, |a, b, w| horiz(a, b, w, 16, |x, y| x.wrapping_sub(y))),
        Phsubd => int2(st, insn, |a, b, w| horiz(a, b, w, 32, |x, y| x.wrapping_sub(y))),
        Phsubsw => int2(st, insn, |a, b, w| horiz(a, b, w, 16, |x, y| sat_s(sx(x, 16) - sx(y, 16), 16))),

        _ => return None,
    }
    Some(Ok(Step::Next))
}
