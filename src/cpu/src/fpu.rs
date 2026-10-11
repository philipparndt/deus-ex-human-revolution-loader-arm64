//! x87 floating point unit.
//!
//! Registers are kept as f64 (`CpuState::x87.st`, physical order); the 80-bit
//! extended format is only used at the memory boundary (fld/fstp m80,
//! fnsave/frstor, fxsave/fxrstor). Consequences:
//! - arithmetic is IEEE double (53-bit mantissa, 11-bit exponent) regardless of
//!   the precision-control bits; values outside the double range overflow to
//!   inf / underflow to (denormal) zero where real hardware would keep them;
//! - rounding control (cw bits 10-11) is honoured for fist/fistp/frndint/fbstp,
//!   for fst m32, and for the basic arithmetic (fadd/fsub/fmul/fdiv/fsqrt and
//!   their integer/pop forms); fisttp always truncates;
//! - transcendental functions use the host libm on doubles.
//!
//! Stack faults: values are pushed/read like QEMU does (no masked-response
//! NaNs), but IE|SF are recorded in the status word when a push hits a
//! non-empty register or an operand register is empty.
//!
//! The status word in `x87.sw` never contains TOP (bits 11-13); TOP lives in
//! `x87.top` and is merged by [`status_word`].

use crate::interp::{ea, ExecResult, Step};
use crate::mem;
use crate::state::flags::*;
use crate::state::{CpuState, EAX};
use crate::Exit;
use iced_x86::{Instruction, MemorySize, Mnemonic, OpKind, Register};

pub const C0: u16 = 0x0100;
pub const C1: u16 = 0x0200;
pub const C2: u16 = 0x0400;
pub const C3: u16 = 0x4000;
pub const CC_MASK: u16 = C0 | C1 | C2 | C3;
pub const SW_IE: u16 = 0x0001;
pub const SW_ZE: u16 = 0x0004;
pub const SW_SF: u16 = 0x0040;
pub const SW_ES: u16 = 0x0080;
pub const SW_B: u16 = 0x8000;
pub const SW_TOP: u16 = 0x3800;

/// The x87 "real indefinite" QNaN (negative, quiet, zero payload).
pub const INDEFINITE: f64 = f64::from_bits(0xFFF8_0000_0000_0000);
const QBIT: u64 = 1 << 51;
const MANT: u64 = (1 << 52) - 1;

const RC_NEAR: u16 = 0;
const RC_DOWN: u16 = 1;
const RC_UP: u16 = 2;
const RC_CHOP: u16 = 3;

// --- 80-bit conversions -----------------------------------------------------------------------

/// f64 -> 80-bit extended (mantissa with explicit integer bit, sign/exponent). Exact.
pub fn f64_to_f80(v: f64) -> (u64, u16) {
    let b = v.to_bits();
    let sign = ((b >> 63) as u16) << 15;
    let e = ((b >> 52) & 0x7ff) as i32;
    let m = b & MANT;
    if e == 0x7ff {
        return ((1 << 63) | (m << 11), sign | 0x7fff);
    }
    if e == 0 {
        if m == 0 {
            return (0, sign);
        }
        let lz = m.leading_zeros() as i32;
        let p = 63 - lz; // position of the top bit
        return (m << lz, sign | (p - 1074 + 16383) as u16);
    }
    ((1 << 63) | (m << 11), sign | (e - 1023 + 16383) as u16)
}

/// 80-bit extended -> f64, rounding to nearest-even. NaN payloads keep their
/// top 52 bits (signalling NaNs stay signalling); unsupported encodings
/// (unnormals, pseudo-NaN/inf) become the indefinite NaN.
pub fn f80_to_f64(mant: u64, se: u16) -> f64 {
    let sign = ((se >> 15) as u64) << 63;
    let e = (se & 0x7fff) as i32;
    if e == 0x7fff {
        if mant >> 63 == 0 {
            return INDEFINITE; // pseudo-infinity / pseudo-NaN
        }
        if mant << 1 == 0 {
            return f64::from_bits(sign | 0x7ff0_0000_0000_0000);
        }
        let mut m = (mant >> 11) & MANT;
        if m == 0 {
            m = 1; // payload only in the low bits: keep it a NaN
        }
        return f64::from_bits(sign | 0x7ff0_0000_0000_0000 | m);
    }
    if e != 0 && mant >> 63 == 0 {
        return INDEFINITE; // unnormal
    }
    if mant == 0 {
        return f64::from_bits(sign);
    }
    let lz = mant.leading_zeros() as i32;
    let m = mant << lz;
    let exp = (if e == 0 { 1 } else { e }) - 16383 - lz; // exponent of bit 63
    if exp > 1023 {
        return f64::from_bits(sign | 0x7ff0_0000_0000_0000);
    }
    let shift: u32 = if exp >= -1022 { 11 } else { (11 + (-1022 - exp)) as u32 };
    if shift > 65 {
        return f64::from_bits(sign); // below half the smallest denormal
    }
    let m128 = m as u128;
    let mut keep = m128 >> shift;
    let rem = m128 & ((1u128 << shift) - 1);
    let half = 1u128 << (shift - 1);
    if rem > half || (rem == half && keep & 1 == 1) {
        keep += 1;
    }
    let keep = keep as u64;
    if exp >= -1022 {
        let (keep, exp) = if keep == 1 << 53 { (keep >> 1, exp + 1) } else { (keep, exp) };
        if exp > 1023 {
            return f64::from_bits(sign | 0x7ff0_0000_0000_0000);
        }
        f64::from_bits(sign | (((exp + 1023) as u64) << 52) | (keep & MANT))
    } else {
        f64::from_bits(sign | keep) // keep == 1<<52 encodes the smallest normal
    }
}

fn load_f80(a: u32) -> f64 {
    f80_to_f64(mem::r64(a), mem::r16(a.wrapping_add(8)))
}
fn store_f80(a: u32, v: f64) {
    let (m, se) = f64_to_f80(v);
    mem::w64(a, m);
    mem::w16(a.wrapping_add(8), se);
}

// --- small helpers ----------------------------------------------------------------------------

#[inline]
fn is_snan(x: f64) -> bool {
    x.is_nan() && x.to_bits() & QBIT == 0
}
#[inline]
fn quiet(x: f64) -> f64 {
    f64::from_bits(x.to_bits() | QBIT)
}

/// x87 NaN propagation for two operands, at least one a NaN: SNaN+QNaN -> the
/// QNaN; two NaNs of the same kind -> larger significand (tie: the positive one);
/// a single NaN -> that NaN. The result is always quiet.
fn pick_nan(a: f64, b: f64) -> f64 {
    let r = if a.is_nan() && b.is_nan() {
        let (sa, sb) = (is_snan(a), is_snan(b));
        if sa != sb {
            if sa { b } else { a }
        } else {
            let (ma, mb) = (a.to_bits() & MANT, b.to_bits() & MANT);
            if ma > mb {
                a
            } else if mb > ma {
                b
            } else if !a.is_sign_negative() && b.is_sign_negative() {
                a
            } else {
                b
            }
        }
    } else if a.is_nan() {
        a
    } else {
        b
    };
    quiet(r)
}

/// Result for an operation whose host result was NaN.
fn nan_result(st: &mut CpuState, a: f64, b: f64) -> f64 {
    if a.is_nan() || b.is_nan() {
        if is_snan(a) || is_snan(b) {
            st.x87.sw |= SW_IE;
        }
        pick_nan(a, b)
    } else {
        st.x87.sw |= SW_IE;
        INDEFINITE
    }
}

#[inline]
fn phys(st: &CpuState, i: u32) -> usize {
    ((st.x87.top + i) & 7) as usize
}
#[inline]
fn get(st: &mut CpuState, i: u32) -> f64 {
    let p = phys(st, i);
    if st.x87.empty & (1 << p) != 0 {
        st.x87.sw |= SW_IE | SW_SF;
    }
    st.x87.st[p]
}
#[inline]
fn set(st: &mut CpuState, i: u32, v: f64) {
    let p = phys(st, i);
    st.x87.st[p] = v;
}
#[inline]
fn push(st: &mut CpuState, v: f64) {
    let t = (st.x87.top.wrapping_sub(1)) & 7;
    if st.x87.empty & (1 << t) == 0 {
        st.x87.sw |= SW_IE | SW_SF;
    }
    st.x87.st[t as usize] = v;
    st.x87.empty &= !(1 << t);
    st.x87.top = t;
}
#[inline]
fn pop(st: &mut CpuState) {
    st.x87.empty |= 1 << st.x87.top;
    st.x87.top = (st.x87.top + 1) & 7;
}
#[inline]
fn rc(st: &CpuState) -> u16 {
    (st.x87.cw >> 10) & 3
}
#[inline]
fn set_cc(st: &mut CpuState, mask: u16, v: u16) {
    st.x87.sw = (st.x87.sw & !mask) | v;
}

/// Full status word including TOP (what fnstsw stores).
pub fn status_word(st: &CpuState) -> u16 {
    (st.x87.sw & !SW_TOP) | ((st.x87.top as u16 & 7) << 11)
}

/// Load a full status word (TOP included), as fldenv/frstor/fxrstor do.
pub fn set_status_word(st: &mut CpuState, v: u16) {
    st.x87.top = ((v >> 11) & 7) as u32;
    let mut sw = v & !SW_TOP & !SW_B;
    if sw & SW_ES != 0 {
        sw |= SW_B;
    }
    st.x87.sw = sw;
}

/// Full 2-bit-per-register tag word (fnstenv/fnsave format).
pub fn tag_word(st: &CpuState) -> u16 {
    let mut t = 0u16;
    for i in (0..8).rev() {
        t <<= 2;
        if st.x87.empty & (1 << i) != 0 {
            t |= 3;
        } else {
            let v = st.x87.st[i];
            if v == 0.0 {
                t |= 1;
            } else if !v.is_finite() {
                t |= 2; // NaN/inf (f64 denormals are normal numbers in 80-bit)
            }
        }
    }
    t
}

fn set_tag_word(st: &mut CpuState, mut t: u16) {
    let mut e = 0;
    for i in 0..8 {
        if t & 3 == 3 {
            e |= 1 << i;
        }
        t >>= 2;
    }
    st.x87.empty = e;
}

fn fninit(st: &mut CpuState) {
    st.x87.cw = 0x37f;
    st.x87.sw = 0;
    st.x87.top = 0;
    st.x87.empty = 0xff;
}

#[inline]
fn sti(r: Register) -> u32 {
    (r as u32).wrapping_sub(Register::ST0 as u32) & 7
}

fn has_mem(insn: &Instruction) -> bool {
    (0..insn.op_count()).any(|i| insn.op_kind(i) == OpKind::Memory)
}

// --- rounding ---------------------------------------------------------------------------------

/// Directed rounding (x87 RC down/up/chop) of a basic arithmetic operation.
/// On arm64 the host FPU does it: FPCR.RMode is switched around the single
/// instruction. Elsewhere a software fallback adjusts the round-to-nearest
/// result using the exact error term.
#[cfg(target_arch = "aarch64")]
mod directed {
    use std::arch::asm;

    #[inline]
    fn mode(rc: u16) -> u64 {
        match rc {
            1 => 2 << 22, // down -> RM
            2 => 1 << 22, // up -> RP
            _ => 3 << 22, // chop -> RZ
        }
    }

    macro_rules! binop {
        ($name:ident, $insn:literal) => {
            pub fn $name(a: f64, b: f64, rc: u16) -> f64 {
                let r: f64;
                unsafe {
                    asm!(
                        "mrs {o}, fpcr",
                        "bic {t}, {o}, #0xc00000",
                        "orr {t}, {t}, {m}",
                        "msr fpcr, {t}",
                        concat!($insn, " {r:d}, {a:d}, {b:d}"),
                        "msr fpcr, {o}",
                        o = out(reg) _,
                        t = out(reg) _,
                        m = in(reg) mode(rc),
                        a = in(vreg) a,
                        b = in(vreg) b,
                        r = lateout(vreg) r,
                        options(nomem, nostack)
                    );
                }
                r
            }
        };
    }
    binop!(add, "fadd");
    binop!(sub, "fsub");
    binop!(mul, "fmul");
    binop!(div, "fdiv");

    pub fn sqrt(a: f64, rc: u16) -> f64 {
        let r: f64;
        unsafe {
            asm!(
                "mrs {o}, fpcr",
                "bic {t}, {o}, #0xc00000",
                "orr {t}, {t}, {m}",
                "msr fpcr, {t}",
                "fsqrt {r:d}, {a:d}",
                "msr fpcr, {o}",
                o = out(reg) _,
                t = out(reg) _,
                m = in(reg) mode(rc),
                a = in(vreg) a,
                r = lateout(vreg) r,
                options(nomem, nostack)
            );
        }
        r
    }
}

#[cfg(not(target_arch = "aarch64"))]
mod directed {
    #[inline]
    fn sgn(x: f64) -> i32 {
        if x > 0.0 {
            1
        } else if x < 0.0 {
            -1
        } else {
            0
        }
    }

    /// Adjust the round-to-nearest result `r` given the sign `e` of (exact - r).
    /// (Approximate when the error term itself underflows.)
    fn adjust(r: f64, e: i32, rc: u16) -> f64 {
        match rc {
            1 if e < 0 => r.next_down(),
            2 if e > 0 => r.next_up(),
            3 if r > 0.0 && e < 0 => r.next_down(),
            3 if r < 0.0 && e > 0 => r.next_up(),
            _ => r,
        }
    }

    pub fn add(x: f64, y: f64, rc: u16) -> f64 {
        let s = x + y;
        if s.is_infinite() {
            return if x.is_finite() && y.is_finite() { adjust(s, -sgn(s), rc) } else { s };
        }
        let bv = s - x;
        let av = s - bv;
        let err = (x - av) + (y - bv);
        if err == 0.0 {
            if s == 0.0 && rc == 1 && !(x == 0.0 && y == 0.0 && !x.is_sign_negative() && !y.is_sign_negative()) {
                return -0.0;
            }
            return s;
        }
        adjust(s, sgn(err), rc)
    }

    pub fn sub(x: f64, y: f64, rc: u16) -> f64 {
        add(x, -y, rc)
    }

    pub fn mul(x: f64, y: f64, rc: u16) -> f64 {
        let p = x * y;
        if !x.is_finite() || !y.is_finite() || x == 0.0 || y == 0.0 {
            return p;
        }
        if p.is_infinite() {
            return adjust(p, -sgn(p), rc);
        }
        if p == 0.0 {
            return adjust(p, if p.is_sign_negative() { -1 } else { 1 }, rc);
        }
        adjust(p, sgn(x.mul_add(y, -p)), rc)
    }

    pub fn div(x: f64, y: f64, rc: u16) -> f64 {
        let q = x / y;
        if !x.is_finite() || !y.is_finite() || x == 0.0 || y == 0.0 {
            return q;
        }
        if q.is_infinite() {
            return adjust(q, -sgn(q), rc);
        }
        if q == 0.0 {
            return adjust(q, if q.is_sign_negative() { -1 } else { 1 }, rc);
        }
        adjust(q, sgn((-q).mul_add(y, x)) * sgn(y), rc)
    }

    pub fn sqrt(x: f64, rc: u16) -> f64 {
        let s = x.sqrt();
        if !(x > 0.0) || !x.is_finite() {
            return s;
        }
        adjust(s, sgn((-s).mul_add(s, x)), rc)
    }
}

fn sqrt_rc(x: f64, rc: u16) -> f64 {
    if rc == RC_NEAR { x.sqrt() } else { directed::sqrt(x, rc) }
}

/// Round to an integral value with the given rounding control.
fn round_int(x: f64, rc: u16) -> f64 {
    match rc {
        RC_NEAR => x.round_ties_even(),
        RC_DOWN => x.floor(),
        RC_UP => x.ceil(),
        _ => x.trunc(),
    }
}

/// f64 -> f32 bits honouring rounding control (fst m32).
fn to_f32_bits(st: &mut CpuState, x: f64) -> u32 {
    if x.is_nan() {
        if is_snan(x) {
            st.x87.sw |= SW_IE;
        }
        let b = x.to_bits();
        return ((b >> 32) as u32 & 0x8000_0000) | 0x7fc0_0000 | ((b & MANT) >> 29) as u32;
    }
    let f = x as f32;
    let back = f as f64;
    let f = match rc(st) {
        RC_DOWN if back > x => f.next_down(),
        RC_UP if back < x => f.next_up(),
        RC_CHOP if back.abs() > x.abs() => {
            if f > 0.0 { f.next_down() } else { f.next_up() }
        }
        _ => f,
    };
    f.to_bits()
}

fn f32_to_f64(st: &mut CpuState, b: u32) -> f64 {
    let f = f32::from_bits(b);
    if f.is_nan() {
        if b & 0x0040_0000 == 0 {
            st.x87.sw |= SW_IE;
        }
        return f64::from_bits((((b >> 31) as u64) << 63) | 0x7ff8_0000_0000_0000 | (((b & 0x7f_ffff) as u64) << 29));
    }
    f as f64
}

fn f64_load(st: &mut CpuState, b: u64) -> f64 {
    let v = f64::from_bits(b);
    if is_snan(v) {
        st.x87.sw |= SW_IE;
        return quiet(v);
    }
    v
}

/// Load the memory operand (float or integer) as f64.
fn load_mem(st: &mut CpuState, insn: &Instruction) -> f64 {
    let a = ea(st, insn);
    match insn.memory_size() {
        MemorySize::Float32 => f32_to_f64(st, mem::r32(a)),
        MemorySize::Float64 => f64_load(st, mem::r64(a)),
        MemorySize::Float80 => load_f80(a),
        MemorySize::Int16 => mem::r16(a) as i16 as f64,
        MemorySize::Int32 => mem::r32(a) as i32 as f64,
        MemorySize::Int64 => mem::r64(a) as i64 as f64,
        MemorySize::Bcd => {
            let mut v: u64 = 0;
            for i in (0..9).rev() {
                let b = mem::r8(a.wrapping_add(i)) as u64;
                v = v.wrapping_mul(100).wrapping_add((b >> 4) * 10 + (b & 15));
            }
            let f = v as i64 as f64;
            if mem::r8(a.wrapping_add(9)) & 0x80 != 0 { -f } else { f }
        }
        _ => f64::NAN,
    }
}

/// Convert to an integer of `bits` width with rounding control `rc`; out of
/// range or NaN gives the integer indefinite value and IE.
fn to_int(st: &mut CpuState, x: f64, rc: u16, bits: u32) -> i64 {
    let lo = -((1u64 << (bits - 1)) as f64);
    let r = round_int(x, rc);
    if r >= lo && r < -lo {
        r as i64
    } else {
        st.x87.sw |= SW_IE;
        lo as i64
    }
}

fn store_int(st: &mut CpuState, insn: &Instruction, x: f64, rc: u16) {
    let a = ea(st, insn);
    match insn.memory_size() {
        MemorySize::Int16 => {
            let v = to_int(st, x, rc, 16);
            mem::w16(a, v as u16)
        }
        MemorySize::Int32 => {
            let v = to_int(st, x, rc, 32);
            mem::w32(a, v as u32)
        }
        _ => {
            let v = to_int(st, x, rc, 64);
            mem::w64(a, v as u64)
        }
    }
}

fn store_bcd(st: &mut CpuState, a: u32, x: f64) {
    let r = round_int(x, rc(st));
    if !(r.abs() <= 999_999_999_999_999_999.0) {
        st.x87.sw |= SW_IE;
        for i in 0..7 {
            mem::w8(a.wrapping_add(i), 0);
        }
        mem::w8(a.wrapping_add(7), 0xc0);
        mem::w8(a.wrapping_add(8), 0xff);
        mem::w8(a.wrapping_add(9), 0xff);
        return;
    }
    let iv = r as i64;
    let mut v = iv.unsigned_abs();
    for i in 0..9 {
        let d = v % 100;
        v /= 100;
        mem::w8(a.wrapping_add(i), (((d / 10) << 4) | (d % 10)) as u8);
    }
    mem::w8(a.wrapping_add(9), if iv < 0 { 0x80 } else { 0 });
}

// --- arithmetic -------------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
enum Op {
    Add,
    Sub,
    SubR,
    Mul,
    Div,
    DivR,
}

fn arith(st: &mut CpuState, op: Op, a: f64, b: f64) -> f64 {
    let (x, y) = match op {
        Op::SubR | Op::DivR => (b, a),
        _ => (a, b),
    };
    let r = match op {
        Op::Add => x + y,
        Op::Sub | Op::SubR => x - y,
        Op::Mul => x * y,
        Op::Div | Op::DivR => x / y,
    };
    if r.is_nan() {
        return nan_result(st, x, y);
    }
    let is_div = matches!(op, Op::Div | Op::DivR);
    if is_div && y == 0.0 && x.is_finite() {
        st.x87.sw |= SW_ZE;
    }
    let rc = rc(st);
    if rc == RC_NEAR {
        return r;
    }
    match op {
        Op::Add => directed::add(x, y, rc),
        Op::Sub | Op::SubR => directed::sub(x, y, rc),
        Op::Mul => directed::mul(x, y, rc),
        _ => directed::div(x, y, rc),
    }
}

/// Compare for fcom/fucom/ficom/ftst: returns the C3/C2/C0 bits.
fn compare(st: &mut CpuState, a: f64, b: f64, quiet_cmp: bool) -> u16 {
    if a.is_nan() || b.is_nan() {
        if !quiet_cmp || is_snan(a) || is_snan(b) {
            st.x87.sw |= SW_IE;
        }
        C3 | C2 | C0
    } else if a < b {
        C0
    } else if a == b {
        C3
    } else {
        0
    }
}

/// True exponent of a finite nonzero f64 (denormals included), as in 80-bit.
fn exponent(x: f64) -> i32 {
    let b = x.to_bits();
    let e = ((b >> 52) & 0x7ff) as i32;
    if e == 0 { (63 - (b & MANT).leading_zeros() as i32) - 1074 } else { e - 1023 }
}

/// x * 2^n with a single rounding (musl's scalbn).
fn scalbn(mut x: f64, mut n: i32) -> f64 {
    let p1023 = f64::from_bits(0x7fe0_0000_0000_0000);
    let pm969 = f64::from_bits(0x0010_0000_0000_0000) * 9007199254740992.0; // 2^-1022 * 2^53
    if n > 1023 {
        x *= p1023;
        n -= 1023;
        if n > 1023 {
            x *= p1023;
            n -= 1023;
            if n > 1023 {
                n = 1023;
            }
        }
    } else if n < -1022 {
        x *= pm969;
        n += 1022 - 53;
        if n < -1022 {
            x *= pm969;
            n += 1022 - 53;
            if n < -1022 {
                n = -1022;
            }
        }
    }
    x * f64::from_bits(((0x3ff + n) as u64) << 52)
}

fn fprem(st: &mut CpuState, ieee: bool) {
    let a = get(st, 0);
    let b = get(st, 1);
    if a.is_nan() || b.is_nan() {
        let r = nan_result(st, a, b);
        set(st, 0, r);
        set_cc(st, CC_MASK, 0);
        return;
    }
    if a.is_infinite() || b == 0.0 {
        st.x87.sw |= SW_IE;
        set(st, 0, INDEFINITE);
        set_cc(st, CC_MASK, 0);
        return;
    }
    if a == 0.0 || b.is_infinite() {
        set_cc(st, CC_MASK, 0);
        return;
    }
    let d = exponent(a) - exponent(b);
    if d >= 64 {
        // Partial remainder: reduce the exponent difference by 32..63.
        let n = 32 + d % 32;
        let r = a % scalbn(b, d - n);
        set(st, 0, r);
        st.x87.sw |= C2;
        return;
    }
    let mut r = a % b;
    let b8 = b * 8.0;
    let r8 = if b8.is_finite() { a % b8 } else { a };
    let mut q = (((r8 - r) / b).abs().round() as u32) & 7;
    if ieee && r != 0.0 {
        let (ar, ab) = (r.abs(), b.abs());
        let gt = ar > f64::MAX / 2.0 || 2.0 * ar > ab;
        let eq = ar <= f64::MAX / 2.0 && 2.0 * ar == ab;
        if gt || (eq && q & 1 == 1) {
            r = if r > 0.0 { r - ab } else { r + ab };
            q += 1;
        }
    }
    set(st, 0, r);
    let cc = if q & 4 != 0 { C0 } else { 0 } | if q & 2 != 0 { C3 } else { 0 } | if q & 1 != 0 { C1 } else { 0 };
    set_cc(st, CC_MASK, cc);
}

#[derive(Clone, Copy)]
enum Trig {
    Sin,
    Cos,
    SinCos,
    Tan,
}

fn trig(st: &mut CpuState, kind: Trig) {
    let x = get(st, 0);
    if x.is_nan() {
        let r = nan_result(st, x, x);
        set(st, 0, r);
        match kind {
            Trig::SinCos => push(st, r),
            Trig::Tan => push(st, 1.0),
            _ => {}
        }
        set_cc(st, C2, 0);
        return;
    }
    if x.is_infinite() {
        st.x87.sw |= SW_IE;
        set(st, 0, INDEFINITE);
        match kind {
            Trig::SinCos => push(st, INDEFINITE),
            Trig::Tan => push(st, INDEFINITE),
            _ => {}
        }
        set_cc(st, C2, 0);
        return;
    }
    if x.abs() >= 9223372036854775808.0 {
        set_cc(st, C2, C2); // out of range: operand unchanged
        return;
    }
    match kind {
        Trig::Sin => set(st, 0, x.sin()),
        Trig::Cos => set(st, 0, x.cos()),
        Trig::SinCos => {
            let (s, c) = x.sin_cos();
            set(st, 0, s);
            push(st, c);
        }
        Trig::Tan => {
            set(st, 0, x.tan());
            push(st, 1.0);
        }
    }
    set_cc(st, C2, 0);
}

// --- environment ------------------------------------------------------------------------------

fn store_env(st: &mut CpuState, a: u32, data32: bool) {
    let (cw, sw, tw) = (st.x87.cw, status_word(st), tag_word(st));
    if data32 {
        mem::w32(a, cw as u32);
        mem::w32(a.wrapping_add(4), sw as u32);
        mem::w32(a.wrapping_add(8), tw as u32);
        for off in [12u32, 16, 20, 24] {
            mem::w32(a.wrapping_add(off), 0); // FIP/FCS+FOP/FDP/FDS: not tracked
        }
    } else {
        mem::w16(a, cw);
        mem::w16(a.wrapping_add(2), sw);
        mem::w16(a.wrapping_add(4), tw);
        for off in [6u32, 8, 10, 12] {
            mem::w16(a.wrapping_add(off), 0);
        }
    }
}

fn load_env(st: &mut CpuState, a: u32, data32: bool) {
    let step = if data32 { 4 } else { 2 };
    st.x87.cw = mem::r16(a);
    let sw = mem::r16(a.wrapping_add(step));
    let tw = mem::r16(a.wrapping_add(2 * step));
    set_status_word(st, sw);
    set_tag_word(st, tw);
}

fn fxsave(st: &mut CpuState, a: u32) -> ExecResult {
    if a & 15 != 0 {
        st.fault_addr = a;
        return Err(Exit::Fault);
    }
    mem::w16(a, st.x87.cw);
    mem::w16(a.wrapping_add(2), status_word(st));
    mem::w16(a.wrapping_add(4), (!st.x87.empty & 0xff) as u16); // abridged tag + reserved byte
    mem::w16(a.wrapping_add(6), 0); // FOP
    for off in [8u32, 12, 16, 20] {
        mem::w32(a.wrapping_add(off), 0); // FIP, FCS, FDP, FDS
    }
    mem::w32(a.wrapping_add(24), st.mxcsr);
    mem::w32(a.wrapping_add(28), 0xffff); // MXCSR_MASK
    for i in 0..8 {
        let v = st.x87.st[phys(st, i)];
        store_f80(a.wrapping_add(32 + 16 * i), v);
    }
    for i in 0..8 {
        mem::w128(a.wrapping_add(160 + 16 * i as u32), st.xmm[i]);
    }
    Ok(Step::Next)
}

fn fxrstor(st: &mut CpuState, a: u32) -> ExecResult {
    let mxcsr = mem::r32(a.wrapping_add(24));
    if a & 15 != 0 || mxcsr & 0xffff_0000 != 0 {
        st.fault_addr = a;
        return Err(Exit::Fault);
    }
    st.x87.cw = mem::r16(a);
    set_status_word(st, mem::r16(a.wrapping_add(2)));
    st.x87.empty = !(mem::r8(a.wrapping_add(4)) as u32) & 0xff;
    for i in 0..8 {
        let v = load_f80(a.wrapping_add(32 + 16 * i));
        let p = phys(st, i);
        st.x87.st[p] = v;
    }
    st.mxcsr = mxcsr;
    for i in 0..8 {
        st.xmm[i] = mem::r128(a.wrapping_add(160 + 16 * i as u32));
    }
    Ok(Step::Next)
}

// --- dispatch ---------------------------------------------------------------------------------

/// Execute an x87 instruction; None if `insn` is not handled here.
pub fn exec(st: &mut CpuState, insn: &Instruction) -> Option<ExecResult> {
    use Mnemonic::*;
    let m = insn.mnemonic();
    match m {
        // loads
        Fld => {
            let v = if has_mem(insn) { load_mem(st, insn) } else { get(st, sti(insn.op0_register())) };
            push(st, v);
        }
        Fild | Fbld => {
            let v = load_mem(st, insn);
            push(st, v);
        }
        Fld1 | Fldz | Fldpi | Fldl2e | Fldl2t | Fldlg2 | Fldln2 => {
            use std::f64::consts::*;
            let v = match m {
                Fld1 => 1.0,
                Fldz => 0.0,
                Fldpi => PI,
                Fldl2e => LOG2_E,
                Fldl2t => LOG2_10,
                Fldlg2 => LOG10_2,
                _ => LN_2,
            };
            push(st, v);
        }
        // stores
        Fst | Fstp | Fstpnce => {
            let v = get(st, 0);
            if has_mem(insn) {
                let a = ea(st, insn);
                match insn.memory_size() {
                    MemorySize::Float32 => {
                        let b = to_f32_bits(st, v);
                        mem::w32(a, b);
                    }
                    MemorySize::Float64 => {
                        if is_snan(v) {
                            st.x87.sw |= SW_IE;
                        }
                        let b = if v.is_nan() { quiet(v).to_bits() } else { v.to_bits() };
                        mem::w64(a, b);
                    }
                    _ => store_f80(a, v),
                }
            } else {
                set(st, sti(insn.op0_register()), v);
            }
            if m != Fst {
                pop(st);
            }
        }
        Fist | Fistp | Fisttp => {
            let v = get(st, 0);
            let r = if m == Fisttp { RC_CHOP } else { rc(st) };
            store_int(st, insn, v, r);
            if m != Fist {
                pop(st);
            }
        }
        Fbstp => {
            let v = get(st, 0);
            let a = ea(st, insn);
            store_bcd(st, a, v);
            pop(st);
        }
        Fxch => {
            let i = if insn.op_count() == 0 { 1 } else { sti(insn.op_register(insn.op_count() - 1)) };
            let a = get(st, 0);
            let b = get(st, i);
            set(st, 0, b);
            set(st, i, a);
        }
        Fcmovb | Fcmove | Fcmovbe | Fcmovu | Fcmovnb | Fcmovne | Fcmovnbe | Fcmovnu => {
            let f = st.eflags;
            let (cf, zf, pf) = (f & CF != 0, f & ZF != 0, f & PF != 0);
            let c = match m {
                Fcmovb => cf,
                Fcmove => zf,
                Fcmovbe => cf || zf,
                Fcmovu => pf,
                Fcmovnb => !cf,
                Fcmovne => !zf,
                Fcmovnbe => !cf && !zf,
                _ => !pf,
            };
            let v = get(st, sti(insn.op1_register()));
            if c {
                set(st, 0, v);
            }
        }
        // arithmetic
        Fadd | Faddp | Fiadd | Fsub | Fsubp | Fisub | Fsubr | Fsubrp | Fisubr | Fmul | Fmulp | Fimul | Fdiv
        | Fdivp | Fidiv | Fdivr | Fdivrp | Fidivr => {
            let op = match m {
                Fadd | Faddp | Fiadd => Op::Add,
                Fsub | Fsubp | Fisub => Op::Sub,
                Fsubr | Fsubrp | Fisubr => Op::SubR,
                Fmul | Fmulp | Fimul => Op::Mul,
                Fdiv | Fdivp | Fidiv => Op::Div,
                _ => Op::DivR,
            };
            let (dst, b) = if has_mem(insn) {
                (0, load_mem(st, insn))
            } else if insn.op_count() == 2 {
                let d = sti(insn.op0_register());
                (d, get(st, sti(insn.op1_register())))
            } else {
                (1, get(st, 0))
            };
            let a = get(st, dst);
            let r = arith(st, op, a, b);
            set(st, dst, r);
            if matches!(m, Faddp | Fsubp | Fsubrp | Fmulp | Fdivp | Fdivrp) {
                pop(st);
            }
        }
        Fchs => {
            let v = get(st, 0);
            set(st, 0, -v);
        }
        Fabs => {
            let v = get(st, 0);
            set(st, 0, v.abs());
        }
        Fsqrt => {
            let x = get(st, 0);
            let r = sqrt_rc(x, rc(st));
            let r = if r.is_nan() { nan_result(st, x, x) } else { r };
            set(st, 0, r);
        }
        Frndint => {
            let x = get(st, 0);
            let r = if x.is_nan() { nan_result(st, x, x) } else { round_int(x, rc(st)) };
            set(st, 0, r);
        }
        Fscale => {
            let a = get(st, 0);
            let b = get(st, 1);
            let r = if a.is_nan() || b.is_nan() {
                nan_result(st, a, b)
            } else if b.is_infinite() {
                if (b > 0.0 && a == 0.0) || (b < 0.0 && a.is_infinite()) {
                    st.x87.sw |= SW_IE;
                    INDEFINITE
                } else if b > 0.0 {
                    a * f64::INFINITY
                } else {
                    a * 0.0
                }
            } else {
                scalbn(a, b.trunc().clamp(-100000.0, 100000.0) as i32)
            };
            set(st, 0, r);
        }
        Fxtract => {
            let x = get(st, 0);
            let (e, s) = if x.is_nan() {
                let n = nan_result(st, x, x);
                (n, n)
            } else if x.is_infinite() {
                (f64::INFINITY, x)
            } else if x == 0.0 {
                st.x87.sw |= SW_ZE;
                (f64::NEG_INFINITY, x)
            } else {
                let e = exponent(x);
                (e as f64, scalbn(x, -e))
            };
            set(st, 0, e);
            push(st, s);
        }
        Fprem => fprem(st, false),
        Fprem1 => fprem(st, true),
        Fsin => trig(st, Trig::Sin),
        Fcos => trig(st, Trig::Cos),
        Fsincos => trig(st, Trig::SinCos),
        Fptan => trig(st, Trig::Tan),
        Fpatan => {
            let x = get(st, 0);
            let y = get(st, 1);
            let r = if x.is_nan() || y.is_nan() { nan_result(st, y, x) } else { y.atan2(x) };
            set(st, 1, r);
            pop(st);
        }
        F2xm1 => {
            let x = get(st, 0);
            // architecturally defined for |x| <= 1 only; outside, behave like 2^x - 1
            let r = if x.is_nan() {
                nan_result(st, x, x)
            } else if x.abs() <= 1.0 {
                (x * std::f64::consts::LN_2).exp_m1()
            } else {
                x.exp2() - 1.0
            };
            set(st, 0, r);
        }
        Fyl2x | Fyl2xp1 => {
            let x = get(st, 0);
            let y = get(st, 1);
            let r = if x.is_nan() || y.is_nan() {
                nan_result(st, y, x)
            } else {
                let l = if m == Fyl2x { x.log2() } else { x.ln_1p() / std::f64::consts::LN_2 };
                let r = y * l;
                if r.is_nan() {
                    st.x87.sw |= SW_IE;
                    INDEFINITE
                } else {
                    if l.is_infinite() && x.is_finite() && y != 0.0 {
                        st.x87.sw |= SW_ZE;
                    }
                    r
                }
            };
            set(st, 1, r);
            pop(st);
        }
        // compares
        Fcom | Fcomp | Fcompp | Fucom | Fucomp | Fucompp | Ficom | Ficomp | Ftst => {
            let a = get(st, 0);
            let b = if m == Ftst {
                0.0
            } else if has_mem(insn) {
                load_mem(st, insn)
            } else if insn.op_count() == 0 {
                get(st, 1)
            } else {
                get(st, sti(insn.op_register(insn.op_count() - 1)))
            };
            let q = matches!(m, Fucom | Fucomp | Fucompp);
            let cc = compare(st, a, b, q);
            set_cc(st, C0 | C2 | C3, cc);
            match m {
                Fcomp | Fucomp | Ficomp => pop(st),
                Fcompp | Fucompp => {
                    pop(st);
                    pop(st);
                }
                _ => {}
            }
        }
        Fcomi | Fcomip | Fucomi | Fucomip => {
            let a = get(st, 0);
            let b = get(st, sti(insn.op1_register()));
            let cc = compare(st, a, b, matches!(m, Fucomi | Fucomip));
            let mut f = 0;
            if cc & C0 != 0 {
                f |= CF;
            }
            if cc & C2 != 0 {
                f |= PF;
            }
            if cc & C3 != 0 {
                f |= ZF;
            }
            st.eflags = (st.eflags & !(CF | PF | ZF | OF | SF | AF)) | f;
            if matches!(m, Fcomip | Fucomip) {
                pop(st);
            }
        }
        Fxam => {
            let p = phys(st, 0);
            let v = st.x87.st[p];
            let mut cc = if v.is_sign_negative() { C1 } else { 0 };
            cc |= if st.x87.empty & (1 << p) != 0 {
                C3 | C0
            } else if v.is_nan() {
                C0
            } else if v.is_infinite() {
                C2 | C0
            } else if v == 0.0 {
                C3
            } else {
                C2 // f64 denormals are normal numbers in the 80-bit format
            };
            set_cc(st, CC_MASK, cc);
        }
        // control
        Fnstsw | Fstsw => {
            let v = status_word(st);
            if insn.op0_kind() == OpKind::Register {
                st.gpr[EAX] = (st.gpr[EAX] & 0xffff_0000) | v as u32;
            } else {
                mem::w16(ea(st, insn), v);
            }
        }
        Fnstcw | Fstcw => mem::w16(ea(st, insn), st.x87.cw),
        Fldcw => st.x87.cw = mem::r16(ea(st, insn)),
        Fnstenv | Fstenv => {
            let a = ea(st, insn);
            store_env(st, a, insn.memory_size() == MemorySize::FpuEnv28);
            st.x87.cw |= 0x3f; // fnstenv masks all exceptions
        }
        Fldenv => {
            let a = ea(st, insn);
            load_env(st, a, insn.memory_size() == MemorySize::FpuEnv28);
        }
        Fnsave | Fsave => {
            let a = ea(st, insn);
            let data32 = insn.memory_size() == MemorySize::FpuState108;
            store_env(st, a, data32);
            let base = a.wrapping_add(if data32 { 28 } else { 14 });
            for i in 0..8 {
                let v = st.x87.st[phys(st, i)];
                store_f80(base.wrapping_add(10 * i), v);
            }
            fninit(st);
        }
        Frstor => {
            let a = ea(st, insn);
            let data32 = insn.memory_size() == MemorySize::FpuState108;
            load_env(st, a, data32);
            let base = a.wrapping_add(if data32 { 28 } else { 14 });
            for i in 0..8 {
                let v = load_f80(base.wrapping_add(10 * i));
                let p = phys(st, i);
                st.x87.st[p] = v;
            }
        }
        Fxsave => {
            let a = ea(st, insn);
            return Some(fxsave(st, a));
        }
        Fxrstor => {
            let a = ea(st, insn);
            return Some(fxrstor(st, a));
        }
        Fninit | Finit => fninit(st),
        Fnclex | Fclex => st.x87.sw &= 0x7f00,
        Ffree | Ffreep => {
            let p = phys(st, sti(insn.op0_register()));
            st.x87.empty |= 1 << p;
            if m == Ffreep {
                pop(st);
            }
        }
        Fincstp => {
            st.x87.top = (st.x87.top + 1) & 7;
            set_cc(st, CC_MASK, 0);
        }
        Fdecstp => {
            st.x87.top = (st.x87.top.wrapping_sub(1)) & 7;
            set_cc(st, CC_MASK, 0);
        }
        // 8087/287 leftovers are no-ops on 387+
        Fnop | Fneni | Feni | Fndisi | Fdisi | Fnsetpm | Fsetpm => {}
        _ => return None,
    }
    Some(Ok(Step::Next))
}
