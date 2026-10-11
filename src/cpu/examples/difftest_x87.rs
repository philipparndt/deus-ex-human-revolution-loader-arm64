//! Differential tester for the x87 FPU: random x87 instructions executed one at
//! a time in Unicorn (reference) and in dxcpu's interpreter, starting from an
//! identical random FPU state (stack values, TOP, tags, control word with
//! random rounding/precision control, condition codes). After each instruction
//! TOP, tags, control word, condition codes, all eight registers (converted to
//! f64), integer registers, eflags, xmm/mxcsr and memory are compared.
//!
//!   cargo run --release --example difftest_x87 -- [iterations] [seed]
//!
//! Tolerances (registers are f64 in dxcpu, 80-bit in Unicorn):
//! - loads/stores/moves/integer conversions/compares/fscale/fxtract/frndint:
//!   bit-exact (memory always bit-exact);
//! - fadd/fsub/fmul/fdiv/fsqrt (+ integer/pop forms): bit-exact when precision
//!   control is 53-bit (results rounded once in both engines), <= 1 ulp when it
//!   is 64-bit (double rounding 80->64) or the result is subnormal;
//! - transcendentals (fsin/fcos/fsincos/fptan/fpatan/f2xm1/fyl2x/fyl2xp1):
//!   relative 1e-12 or absolute 1e-15, any NaN matches any NaN;
//! - fprem/fprem1: see `prem_ok` (Unicorn 2.1.4 = QEMU 5.0 computes them with
//!   lossy double arithmetic; dxcpu's result must then equal the exact remainder).
//!
//! Inputs on which Unicorn (QEMU 5.0 fpu_helper.c) knowingly deviates from real
//! hardware are skipped and counted, see `quirk`.

use dxcpu::fpu;
use dxcpu::interp::Engine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::{
    Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter, MemorySize, Mnemonic, OpKind, Register,
};
use std::collections::{BTreeMap, HashMap};
use std::ffi::c_void;

#[link(name = "unicorn")]
unsafe extern "C" {
    fn uc_open(arch: i32, mode: i32, uc: *mut *mut c_void) -> i32;
    fn uc_close(uc: *mut c_void) -> i32;
    fn uc_mem_map_ptr(uc: *mut c_void, addr: u64, size: usize, perms: u32, ptr: *mut c_void) -> i32;
    fn uc_reg_write(uc: *mut c_void, regid: i32, value: *const c_void) -> i32;
    fn uc_reg_read(uc: *mut c_void, regid: i32, value: *mut c_void) -> i32;
    fn uc_emu_start(uc: *mut c_void, begin: u64, until: u64, timeout: u64, count: usize) -> i32;
    fn uc_ctl(uc: *mut c_void, control: u32, ...) -> i32;
}

const UC_ARCH_X86: i32 = 4;
const UC_CTL_TB_REMOVE_CACHE_W: u32 = 1207959561; // UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2)
const UC_MODE_32: i32 = 4;
const UC_REGS: [i32; 8] = [19, 22, 24, 21, 30, 20, 29, 23]; // eax ecx edx ebx esp ebp esi edi
const UC_EIP: i32 = 26;
const UC_EFLAGS: i32 = 25;
const UC_XMM0: i32 = 122;
const UC_MXCSR: i32 = 249;
const UC_FPSW: i32 = 31;
const UC_FPCW: i32 = 246;
const UC_FPTAG: i32 = 247;
const UC_ST0: i32 = 114;
const UC_CR0: i32 = 50;
const UC_CR4: i32 = 54;

const MEM_SIZE: usize = 1 << 20;
const CODE: u32 = 0x1000;
const DATA: u32 = 0x10000;
const DATA_SIZE: u32 = 0x1000;
const STACK: u32 = 0x80000;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn u32(&mut self) -> u32 {
        self.next() as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.u32() % n
    }
}

/// A random "interesting" double: mostly moderate normal numbers, plus zeros,
/// small integers, halves, big integers, powers of two, infinities, NaNs,
/// denormals/extremes and raw random bit patterns.
fn nice(rng: &mut Rng) -> f64 {
    let s = if rng.u32() & 1 == 0 { 1.0 } else { -1.0 };
    match rng.below(40) {
        0 | 1 => s * 0.0,
        2 => s * f64::INFINITY,
        3 => {
            // NaN: random sign, quiet or signalling, random payload
            let mut m = rng.next() & ((1 << 51) - 1);
            if m == 0 {
                m = 1;
            }
            let q = if rng.u32() & 1 == 0 { 1u64 << 51 } else { 0 };
            let sign = (rng.u32() as u64 & 1) << 63;
            f64::from_bits(sign | 0x7ff0_0000_0000_0000 | q | m)
        }
        4..=7 => (rng.below(201) as i32 - 100) as f64,
        8 | 9 => (rng.below(201) as i32 - 100) as f64 + 0.5,
        10 | 11 => {
            // around integer range limits
            let k = [15, 16, 31, 32, 53, 63, 64][rng.below(7) as usize];
            let d = [0.0, 0.5, 1.0, -0.5, -1.0, 1.5, -1.5][rng.below(7) as usize];
            s * (2f64.powi(k) + d)
        }
        12 => s * 2f64.powi(rng.below(200) as i32 - 100),
        13 => f64::from_bits(rng.next()),
        14 => {
            // extremes: near overflow/underflow and denormals
            let e = [1023, 1022, -1022, -1021, -1040, -1074][rng.below(6) as usize];
            s * (1.0 + (rng.next() >> 12) as f64 / 2f64.powi(52)) * 2f64.powi(e)
        }
        15..=18 => s * (1.0 + (rng.next() >> 12) as f64 / 2f64.powi(52)) * 2f64.powi(rng.below(8) as i32 - 4),
        _ => s * (1.0 + (rng.next() >> 12) as f64 / 2f64.powi(52)) * 2f64.powi(rng.below(81) as i32 - 40),
    }
}

/// Generate candidate x87 instruction bytes.
fn gen_bytes(rng: &mut Rng) -> [u8; 16] {
    let mut b = [0u8; 16];
    for x in b.iter_mut() {
        *x = rng.u32() as u8;
    }
    let mut p = 0;
    match rng.below(100) {
        0..=3 => {
            b[0] = 0x9b; // wait-prefixed forms (fstsw/fstcw/finit/fclex/fstenv/fsave)
            p = 1;
        }
        4..=6 => {
            b[0] = 0x66; // 16-bit operand size (fnstenv/fldenv/fnsave/frstor 14/94 byte forms)
            p = 1;
        }
        7..=10 => {
            b[0] = 0x0f; // fxsave / fxrstor
            b[1] = 0xae;
            let reg = rng.below(2) as u8;
            let disp = if rng.u32() & 1 == 0 { (rng.below(16) as u8).wrapping_sub(8) << 4 } else { rng.u32() as u8 };
            b[2] = 0x40 | (reg << 3) | 3;
            b[3] = disp;
            return b;
        }
        _ => {}
    }
    b[p] = 0xd8 + rng.below(8) as u8;
    if rng.below(5) == 0 {
        // D9 E0..FF: the operand-less group (fchs..fcos), under-represented otherwise
        b[p] = 0xd9;
        b[p + 1] = 0xe0 | rng.below(32) as u8;
        return b;
    }
    let reg = rng.below(8) as u8;
    if rng.u32() & 1 == 0 {
        b[p + 1] = 0xc0 | (reg << 3) | rng.below(8) as u8;
    } else {
        b[p + 1] = 0x40 | (reg << 3) | 3; // [ebx+disp8]
    }
    b
}

fn acceptable(insn: &Instruction) -> bool {
    if insn.is_invalid() {
        return false;
    }
    let s = format!("{:?}", insn.mnemonic());
    if !s.starts_with('F') || insn.mnemonic() == Mnemonic::Femms {
        return false;
    }
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => {
                if insn.memory_base() != Register::EBX
                    || insn.memory_index() != Register::None
                    || (insn.memory_displacement32() as i32).abs() > 1024
                    || !matches!(insn.memory_segment(), Register::DS)
                {
                    return false;
                }
            }
            OpKind::Register => {}
            _ => return false,
        }
    }
    true
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Tol {
    Exact,
    Arith,
    Trans,
    Prem,
}

fn tolerance(m: Mnemonic) -> Tol {
    use Mnemonic::*;
    match m {
        Fadd | Faddp | Fiadd | Fsub | Fsubp | Fisub | Fsubr | Fsubrp | Fisubr | Fmul | Fmulp | Fimul | Fdiv | Fdivp
        | Fidiv | Fdivr | Fdivrp | Fidivr | Fsqrt => Tol::Arith,
        Fsin | Fcos | Fsincos | Fptan | Fpatan | F2xm1 | Fyl2x | Fyl2xp1 => Tol::Trans,
        Fprem | Fprem1 => Tol::Prem,
        _ => Tol::Exact,
    }
}

fn ordered(x: f64) -> i64 {
    let b = x.to_bits() as i64;
    if b < 0 { i64::MIN - b } else { b }
}

fn ulps(a: f64, b: f64) -> u64 {
    (ordered(a) as i128 - ordered(b) as i128).unsigned_abs() as u64
}

fn value_ok(u: f64, d: f64, tol: Tol, pc53: bool) -> bool {
    if u.to_bits() == d.to_bits() {
        return true;
    }
    match tol {
        Tol::Exact | Tol::Prem => false,
        Tol::Arith => {
            if u.is_nan() || d.is_nan() {
                return false;
            }
            // subnormal results are rounded twice by Unicorn (53-bit, then to the
            // f64 denormal); overflowing results with directed rounding are
            // finite in 80-bit but become inf in the round-to-nearest conversion.
            let special = u.abs() < f64::MIN_POSITIVE || d.abs() < f64::MIN_POSITIVE || u.is_infinite() || d.is_infinite();
            ulps(u, d) <= 1 && (!pc53 || special)
        }
        Tol::Trans => {
            if u.is_nan() && d.is_nan() {
                return true;
            }
            if u.is_nan() || d.is_nan() || u.is_infinite() || d.is_infinite() {
                return false;
            }
            let diff = (u - d).abs();
            diff <= 1e-15 || diff <= 1e-12 * u.abs().max(d.abs())
        }
    }
}

fn exponent(x: f64) -> i32 {
    let b = x.to_bits();
    let e = ((b >> 52) & 0x7ff) as i32;
    if e == 0 { (63 - (b & ((1 << 52) - 1)).leading_zeros() as i32) - 1074 } else { e - 1023 }
}

/// Inputs where Unicorn 2.1.4 (QEMU 5.0 x87 helpers) is known to deviate from
/// real hardware; dxcpu implements the hardware behaviour. Returns a reason.
fn quirk(insn: &Instruction, before: &CpuState, ip_after: &CpuState, mem_snan: bool) -> Option<&'static str> {
    use Mnemonic::*;
    // QEMU 5.0 does not quiet signalling NaNs when converting m32/m64 <-> 80-bit
    // (hardware does, and so does dxcpu).
    if mem_snan {
        return Some("m32/m64 SNaN operand");
    }
    let st = |i: u32| before.x87.st[((before.x87.top + i) & 7) as usize];
    let (s0, s1) = (st(0), st(1));
    match insn.mnemonic() {
        Fst | Fstp
            if matches!(insn.memory_size(), MemorySize::Float32 | MemorySize::Float64)
                && before.x87.st[(before.x87.top & 7) as usize].is_nan()
                && before.x87.st[(before.x87.top & 7) as usize].to_bits() & (1 << 51) == 0 =>
        {
            Some("SNaN store to m32/m64")
        }
        // QEMU: x <= 0 (or NaN) sets C2 and does not pop; hardware: NaN/-inf result + pop
        Fyl2x if !(s0 > 0.0) => Some("fyl2x x<=0/NaN"),
        Fyl2xp1 if !(s0 + 1.0 > 0.0) => Some("fyl2xp1 x<=-1/NaN"),
        // QEMU: log(x + 1.0) loses tiny x entirely (y * 0 instead of y * tiny)
        Fyl2xp1 if s1.is_infinite() => Some("fyl2xp1 y=inf"),
        // QEMU: +-inf treated as out of range (C2=1); hardware: invalid -> NaN
        Fsin | Fcos | Fsincos | Fptan if s0.is_infinite() => Some("trig(inf)"),
        // QEMU: |x| == 2^63 counted as in range (hardware: |x| < 2^63)
        Fsin | Fcos | Fsincos | Fptan if s0.abs() == 2f64.powi(63) => Some("trig(+-2^63)"),
        // QEMU: 80-bit exponent field arithmetic on inf/NaN
        Fxtract if !s0.is_finite() => Some("fxtract inf/NaN"),
        // QEMU: 0*2^inf = 0, inf*2^-inf = inf; NaN in st1 returned unquieted
        Fscale if (s1.is_infinite() && (s0 == 0.0 || s0.is_infinite())) || s1.is_nan() => Some("fscale inf/NaN"),
        // QEMU: fisttp saturates to INT_MAX instead of the integer indefinite
        Fisttp if !(s0.trunc().abs() < 2f64.powi(insn.memory_size().size() as i32 * 8 - 1)) => {
            Some("fisttp out of range")
        }
        // QEMU: no BCD indefinite; -0 stored as +0
        Fbstp if !(s0.abs() < 1e18) || (s0 == 0.0 && s0.is_sign_negative()) || s0.round().abs() >= 1e18 => {
            Some("fbstp out of range")
        }
        // QEMU: fxrstor does not #GP on reserved MXCSR bits
        Fxrstor if ip_after.exit_reason == Exit::Fault as u32 && insn.memory_displacement32() & 15 == 0 => {
            Some("fxrstor reserved mxcsr")
        }
        // QEMU: fprem1 with exponent difference < 0 never reduces
        Fprem1
            if s0.is_finite() && s1.is_finite() && s0 != 0.0 && s1 != 0.0 && exponent(s0) < exponent(s1)
                && 2.0 * s0.abs() >= s1.abs() =>
        {
            Some("fprem1 |st0|<|st1|")
        }
        // QEMU: fprem(1) with a NaN or inf/0 operand returns the host NaN
        Fprem | Fprem1 if s0.is_nan() || s1.is_nan() || s0.is_infinite() || s1 == 0.0 => Some("fprem NaN/inf/0"),
        // QEMU: fprem(1) with st1=inf: double exponent arithmetic
        Fprem | Fprem1 if s1.is_infinite() => Some("fprem st1=inf"),
        _ => None,
    }
}

struct Uc(*mut c_void);
impl Drop for Uc {
    fn drop(&mut self) {
        unsafe { uc_close(self.0) };
    }
}

fn wreg(uc: &Uc, id: i32, v: u64) {
    unsafe { uc_reg_write(uc.0, id, &v as *const u64 as *const c_void) };
}
fn rreg(uc: &Uc, id: i32) -> u64 {
    let mut v = 0u64;
    unsafe { uc_reg_read(uc.0, id, &mut v as *mut u64 as *mut c_void) };
    v
}

/// Write a plausible value of the operand's type at the memory operand.
fn seed_operand(rng: &mut Rng, insn: &Instruction, a: usize, m1: &mut [u8], m2: &mut [u8]) {
    let mut bytes: Vec<u8> = Vec::new();
    match insn.memory_size() {
        MemorySize::Float32 => bytes.extend_from_slice(&(nice(rng) as f32).to_bits().to_le_bytes()),
        MemorySize::Float64 => bytes.extend_from_slice(&nice(rng).to_bits().to_le_bytes()),
        MemorySize::Float80 => {
            let (m, se) = fpu::f64_to_f80(nice(rng));
            bytes.extend_from_slice(&m.to_le_bytes());
            bytes.extend_from_slice(&se.to_le_bytes());
        }
        MemorySize::Int16 => bytes.extend_from_slice(&((nice(rng) as i64) as i16).to_le_bytes()),
        MemorySize::Int32 => bytes.extend_from_slice(&((nice(rng) as i64) as i32).to_le_bytes()),
        MemorySize::Int64 => bytes.extend_from_slice(&(nice(rng) as i64).to_le_bytes()),
        MemorySize::Bcd => {
            for _ in 0..9 {
                bytes.push(((rng.below(10) << 4) | rng.below(10)) as u8);
            }
            bytes.push(if rng.u32() & 1 == 0 { 0 } else { 0x80 });
        }
        MemorySize::FpuEnv28 | MemorySize::FpuState108 | MemorySize::FpuEnv14 | MemorySize::FpuState94 => {
            let data32 = matches!(insn.memory_size(), MemorySize::FpuEnv28 | MemorySize::FpuState108);
            let w = if data32 { 4 } else { 2 };
            let cw = 0x7fu32 | ((2 + rng.below(2)) << 8) | (rng.below(4) << 10);
            let sw = rng.u32() & 0x7f47;
            let tw = rng.u32() & 0xffff;
            for (i, v) in [cw, sw, tw, 0, 0, 0, 0].iter().enumerate() {
                let _ = i;
                bytes.extend_from_slice(&v.to_le_bytes()[..w]);
            }
            if matches!(insn.memory_size(), MemorySize::FpuState108 | MemorySize::FpuState94) {
                for _ in 0..8 {
                    let (m, se) = fpu::f64_to_f80(nice(rng));
                    bytes.extend_from_slice(&m.to_le_bytes());
                    bytes.extend_from_slice(&se.to_le_bytes());
                }
            }
        }
        MemorySize::Fxsave_512Byte => {
            if a % 16 != 0 {
                return;
            }
            bytes.resize(512, 0);
            for b in bytes.iter_mut() {
                *b = rng.u32() as u8;
            }
            let cw = 0x7fu16 | ((2 + rng.below(2) as u16) << 8) | ((rng.below(4) as u16) << 10);
            bytes[0..2].copy_from_slice(&cw.to_le_bytes());
            bytes[2..4].copy_from_slice(&((rng.u32() & 0x7f47) as u16).to_le_bytes());
            let mx = if rng.below(8) == 0 { rng.u32() } else { 0x1f80 | (rng.u32() & 0x7f) | (rng.below(4) << 13) };
            bytes[24..28].copy_from_slice(&mx.to_le_bytes());
            for i in 0..8 {
                let (m, se) = fpu::f64_to_f80(nice(rng));
                bytes[32 + 16 * i..40 + 16 * i].copy_from_slice(&m.to_le_bytes());
                bytes[40 + 16 * i..42 + 16 * i].copy_from_slice(&se.to_le_bytes());
            }
        }
        _ => return,
    }
    m1[a..a + bytes.len()].copy_from_slice(&bytes);
    m2[a..a + bytes.len()].copy_from_slice(&bytes);
}

/// Memory bytes whose content is implementation specific (last instruction /
/// operand pointers and opcode, not tracked by dxcpu): (offset, len) relative to ea.
fn ignored_mem(insn: &Instruction) -> Vec<(u32, u32)> {
    match insn.memory_size() {
        MemorySize::FpuEnv28 | MemorySize::FpuState108 => vec![(12, 16)],
        MemorySize::FpuEnv14 | MemorySize::FpuState94 => vec![(6, 8)],
        MemorySize::Fxsave_512Byte => vec![(6, 18)],
        _ => vec![],
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let iterations: usize = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(20000);
    let seed: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0x1234_5678);
    let max_fail: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(40);
    let mut rng = Rng(seed | 1);
    let mut mem_uc = vec![0u8; MEM_SIZE];
    let mut mem_ip = vec![0u8; MEM_SIZE];
    mem::set_base(mem_ip.as_mut_ptr());

    let mut uc_raw = std::ptr::null_mut();
    unsafe {
        assert_eq!(uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc_raw), 0);
        assert_eq!(uc_mem_map_ptr(uc_raw, 0, MEM_SIZE, 7, mem_uc.as_mut_ptr() as *mut c_void), 0);
    }
    let uc = Uc(uc_raw);
    wreg(&uc, UC_CR0, 0x33);
    wreg(&uc, UC_CR4, 0x600);
    let mut eng = Engine::new(0xFFFF_0000, 0xFFFF_0010);
    let mut fmt = IntelFormatter::new();
    let (mut tested, mut failures) = (0usize, 0usize);
    let mut per_mnemonic: HashMap<String, (usize, usize)> = Default::default();
    let mut unimpl: BTreeMap<String, usize> = Default::default();
    let mut quirks: BTreeMap<&'static str, usize> = Default::default();
    let mut faults = 0usize;
    let mut prem_exact_ref = 0usize;

    while tested < iterations && failures < max_fail {
        let bytes = gen_bytes(&mut rng);
        let mut dec = Decoder::with_ip(32, &bytes, CODE as u64, DecoderOptions::NONE);
        let insn = dec.decode();
        if !acceptable(&insn) {
            continue;
        }
        let len = insn.len();
        for m in [&mut mem_uc, &mut mem_ip] {
            m[CODE as usize..CODE as usize + len].copy_from_slice(&bytes[..len]);
            m[CODE as usize + len] = 0xF4;
            m[(STACK - 256) as usize..(STACK + 256) as usize].fill(0x5a);
        }
        for i in 0..(DATA_SIZE as usize) {
            let v = rng.u32() as u8;
            mem_uc[DATA as usize + i] = v;
            mem_ip[DATA as usize + i] = v;
        }
        let mut st = CpuState::new();
        for i in 0..8 {
            st.gpr[i] = rng.u32();
        }
        st.gpr[EBX] = DATA + 0x800;
        st.gpr[ESP] = STACK;
        st.eflags = 0x202 | (rng.u32() & flags::ARITH);
        st.eip = CODE;
        for i in 0..8 {
            for b in st.xmm[i].iter_mut() {
                *b = rng.u32() as u8;
            }
        }
        if insn.op_count() > 0 && (0..insn.op_count()).any(|i| insn.op_kind(i) == OpKind::Memory) && rng.below(4) != 0 {
            let a = st.gpr[EBX].wrapping_add(insn.memory_displacement32()) as usize;
            seed_operand(&mut rng, &insn, a, &mut mem_uc, &mut mem_ip);
        }
        // FPU state: precision 53 or 64 bit, random rounding, all exceptions masked
        let pc53 = rng.u32() & 1 == 0;
        st.x87.cw = 0x7f | if pc53 { 0x200 } else { 0x300 } | ((rng.below(4) as u16) << 10);
        if rng.below(3) != 0 {
            st.x87.cw &= !0xc00; // favour round-to-nearest
        }
        st.x87.top = rng.below(8);
        st.x87.sw = (rng.u32() as u16) & (fpu::CC_MASK | 0x3f | fpu::SW_SF);
        st.x87.empty = 0;
        for i in 0..8 {
            if rng.below(8) == 0 {
                st.x87.empty |= 1 << i;
            }
            st.x87.st[i] = nice(&mut rng);
        }
        for i in 0..8 {
            wreg(&uc, UC_REGS[i], st.gpr[i] as u64);
        }
        wreg(&uc, UC_EFLAGS, st.eflags as u64);
        for i in 0..8 {
            unsafe { uc_reg_write(uc.0, UC_XMM0 + i as i32, st.xmm[i].as_ptr() as *const c_void) };
        }
        wreg(&uc, UC_MXCSR, st.mxcsr as u64);
        wreg(&uc, UC_FPCW, st.x87.cw as u64);
        wreg(&uc, UC_FPSW, fpu::status_word(&st) as u64);
        let mut tag = 0u64;
        for i in 0..8 {
            if st.x87.empty & (1 << i) != 0 {
                tag |= 3 << (2 * i);
            }
        }
        wreg(&uc, UC_FPTAG, tag);
        for i in 0..8u32 {
            let (m, se) = fpu::f64_to_f80(st.x87.st[((st.x87.top + i) & 7) as usize]);
            let mut buf = [0u8; 16];
            buf[..8].copy_from_slice(&m.to_le_bytes());
            buf[8..10].copy_from_slice(&se.to_le_bytes());
            unsafe { uc_reg_write(uc.0, UC_ST0 + i as i32, buf.as_ptr() as *const c_void) };
        }
        let before = st;
        let mem_snan = has_mem(&insn) && {
            let a = (st.gpr[EBX].wrapping_add(insn.memory_displacement32())) as usize;
            match insn.memory_size() {
                MemorySize::Float32 => {
                    let v = u32::from_le_bytes(mem_ip[a..a + 4].try_into().unwrap());
                    v & 0x7fc0_0000 == 0x7f80_0000 && v & 0x3f_ffff != 0
                }
                MemorySize::Float64 => {
                    let v = u64::from_le_bytes(mem_ip[a..a + 8].try_into().unwrap());
                    f64::from_bits(v).is_nan() && v & (1 << 51) == 0
                }
                _ => false,
            }
        };

        unsafe { uc_ctl(uc.0, UC_CTL_TB_REMOVE_CACHE_W, CODE as u64, CODE as u64 + 32) };
        // Run to the end of the instruction (a 9B prefix is a separate fwait in QEMU).
        let mut uc_err = unsafe { uc_emu_start(uc.0, CODE as u64, (CODE + len as u32) as u64, 0, 0) };
        if uc_err == 0 && rreg(&uc, UC_EIP) as u32 != CODE + len as u32 {
            uc_err = -1; // exception (e.g. #GP) stopped emulation without an error code
        }
        eng.flush();
        let exit = eng.run(&mut st, CODE + len as u32);
        st.exit_reason = exit as u32;
        if exit == Exit::Unimplemented {
            *unimpl.entry(format!("{:?}", insn.mnemonic())).or_default() += 1;
            continue;
        }
        if let Some(q) = quirk(&insn, &before, &st, mem_snan) {
            *quirks.entry(q).or_default() += 1;
            continue;
        }
        tested += 1;
        let mut text = String::new();
        fmt.format(&insn, &mut text);
        let e = per_mnemonic.entry(format!("{:?}", insn.mnemonic())).or_default();
        e.0 += 1;
        let m = insn.mnemonic();

        let mut diffs = Vec::new();
        if uc_err != 0 || exit != Exit::Stop {
            if !(uc_err != 0 && exit == Exit::Fault) {
                diffs.push(format!("exit: unicorn err {} vs dxcpu {:?} (unicorn eip {:#x})", uc_err, exit, rreg(&uc, UC_EIP)));
            } else {
                faults += 1;
            }
        } else {
            for i in 0..8 {
                let u = rreg(&uc, UC_REGS[i]) as u32;
                if u != st.gpr[i] {
                    diffs.push(format!("gpr{}: unicorn {:#010x} dxcpu {:#010x}", i, u, st.gpr[i]));
                }
            }
            let ueip = rreg(&uc, UC_EIP) as u32;
            if ueip != st.eip {
                diffs.push(format!("eip: unicorn {:#x} dxcpu {:#x}", ueip, st.eip));
            }
            let mut fmask = flags::ARITH | flags::DF;
            if matches!(m, Mnemonic::Fcomi | Mnemonic::Fcomip | Mnemonic::Fucomi | Mnemonic::Fucomip) {
                // QEMU leaves OF/SF/AF unchanged, hardware clears them (dxcpu clears)
                fmask &= !(flags::OF | flags::SF | flags::AF);
                if st.eflags & (flags::OF | flags::SF | flags::AF) != 0 {
                    diffs.push("fcomi did not clear OF/SF/AF".into());
                }
            }
            let uf = rreg(&uc, UC_EFLAGS) as u32;
            if (uf ^ st.eflags) & fmask != 0 {
                diffs.push(format!("eflags: unicorn {:#x} dxcpu {:#x} (mask {:#x})", uf & fmask, st.eflags & fmask, fmask));
            }
            for i in 0..8 {
                let mut x = [0u8; 16];
                unsafe { uc_reg_read(uc.0, UC_XMM0 + i as i32, x.as_mut_ptr() as *mut c_void) };
                if x != st.xmm[i] {
                    diffs.push(format!("xmm{}: unicorn {:02x?} dxcpu {:02x?}", i, x, st.xmm[i]));
                }
            }
            let umx = rreg(&uc, UC_MXCSR) as u32;
            if umx != st.mxcsr {
                diffs.push(format!("mxcsr: unicorn {:#x} dxcpu {:#x}", umx, st.mxcsr));
            }

            // --- x87 state
            let usw = rreg(&uc, UC_FPSW) as u16;
            let ucw = rreg(&uc, UC_FPCW) as u16;
            let utag = rreg(&uc, UC_FPTAG) as u16;
            let utop = ((usw >> 11) & 7) as u32;
            if utop != st.x87.top {
                diffs.push(format!("top: unicorn {} dxcpu {}", utop, st.x87.top));
            }
            if ucw != st.x87.cw {
                diffs.push(format!("cw: unicorn {:#x} dxcpu {:#x}", ucw, st.x87.cw));
            }
            let mut uempty = 0u32;
            for i in 0..8 {
                if (utag >> (2 * i)) & 3 == 3 {
                    uempty |= 1 << i;
                }
            }
            if uempty != st.x87.empty {
                diffs.push(format!("empty: unicorn {:#04x} dxcpu {:#04x}", uempty, st.x87.empty));
            }
            let tol = tolerance(m);
            let mut ccmask = fpu::CC_MASK;
            if m == Mnemonic::Fsqrt {
                ccmask = 0; // QEMU sets C2 for negative operands; architecturally undefined
            }
            let dsw = fpu::status_word(&st);
            let mut prem_relaxed = false;
            if tol == Tol::Prem {
                let (a, b) = (
                    before.x87.st[(before.x87.top & 7) as usize],
                    before.x87.st[((before.x87.top + 1) & 7) as usize],
                );
                let d = exponent(a) - exponent(b);
                if usw & fpu::C2 != 0 || dsw & fpu::C2 != 0 {
                    // partial remainder: QEMU switches at exponent difference 53
                    // (dxcpu at 64 like hardware), q bits undefined
                    ccmask = if d >= 53 && d < 64 { 0 } else { fpu::C2 };
                    prem_relaxed = true;
                }
                // QEMU computes q via a double division that may round across an
                // integer; dxcpu is exact. Verify dxcpu against the exact
                // remainder instead when they disagree.
                let r = st.x87.st[(st.x87.top & 7) as usize];
                let exact = if m == Mnemonic::Fprem {
                    a % b
                } else {
                    let t = a % b;
                    let q = (a / b).trunc();
                    let ab = b.abs();
                    if 2.0 * t.abs() > ab || (2.0 * t.abs() == ab && (q % 2.0).abs() == 1.0) {
                        if t > 0.0 { t - ab } else { t + ab }
                    } else {
                        t
                    }
                };
                if d < 53 {
                    let mut ust = [0u8; 16];
                    unsafe { uc_reg_read(uc.0, UC_ST0, ust.as_mut_ptr() as *mut c_void) };
                    let u = fpu::f80_to_f64(
                        u64::from_le_bytes(ust[..8].try_into().unwrap()),
                        u16::from_le_bytes([ust[8], ust[9]]),
                    );
                    if u.to_bits() != r.to_bits() || (usw ^ dsw) & ccmask != 0 {
                        if r.to_bits() == exact.to_bits() {
                            prem_exact_ref += 1;
                            prem_relaxed = true;
                            ccmask = 0;
                        }
                    }
                } else if d >= 53 && d < 64 {
                    if r.to_bits() != exact.to_bits() {
                        diffs.push(format!("fprem: dxcpu {:e} exact {:e}", r, exact));
                    }
                }
            }
            if (usw ^ dsw) & ccmask != 0 {
                diffs.push(format!("cc: unicorn {:#06x} dxcpu {:#06x} (mask {:#06x})", usw & ccmask, dsw & ccmask, ccmask));
            }
            for i in 0..8u32 {
                let mut buf = [0u8; 16];
                unsafe { uc_reg_read(uc.0, UC_ST0 + i as i32, buf.as_mut_ptr() as *mut c_void) };
                let u = fpu::f80_to_f64(
                    u64::from_le_bytes(buf[..8].try_into().unwrap()),
                    u16::from_le_bytes([buf[8], buf[9]]),
                );
                let p = ((utop + i) & 7) as usize;
                let d = st.x87.st[p];
                if prem_relaxed && i == 0 {
                    continue;
                }
                // QEMU computes fyl2xp1 as y*log(x+1.0)/log(2): absolute error
                // ~|y|*2^-52 from rounding x+1.0
                let lossy = m == Mnemonic::Fyl2xp1 && {
                    let y = before.x87.st[((before.x87.top + 1) & 7) as usize];
                    (u - d).abs() <= 4e-16 * y.abs() + 1e-12 * u.abs()
                };
                if !lossy && !value_ok(u, d, tol, pc53) {
                    diffs.push(format!(
                        "st(phys {}): unicorn {:e} ({:#018x}) dxcpu {:e} ({:#018x}) ulps {}",
                        p,
                        u,
                        u.to_bits(),
                        d,
                        d.to_bits(),
                        ulps(u, d)
                    ));
                }
            }
            // --- memory
            if has_mem(&insn) {
                let a = before.gpr[EBX].wrapping_add(insn.memory_displacement32());
                for (off, len) in ignored_mem(&insn) {
                    let s = (a + off) as usize;
                    let (src, dst) = (&mem_uc[s..s + len as usize].to_vec(), &mut mem_ip[s..s + len as usize]);
                    dst.copy_from_slice(src);
                }
            }
            if mem_uc[DATA as usize..(DATA + DATA_SIZE) as usize] != mem_ip[DATA as usize..(DATA + DATA_SIZE) as usize] {
                let mut first = None;
                for i in DATA as usize..(DATA + DATA_SIZE) as usize {
                    if mem_uc[i] != mem_ip[i] {
                        first = Some(i);
                        break;
                    }
                }
                let i = first.unwrap();
                let lo = i & !7;
                diffs.push(format!(
                    "data memory differs at {:#x}: unicorn {:02x?} dxcpu {:02x?}",
                    i,
                    &mem_uc[lo..lo + 16],
                    &mem_ip[lo..lo + 16]
                ));
            }
            if mem_uc[(STACK - 64) as usize..(STACK + 64) as usize] != mem_ip[(STACK - 64) as usize..(STACK + 64) as usize] {
                diffs.push("stack memory differs".into());
            }
        }
        if !diffs.is_empty() {
            failures += 1;
            e.1 += 1;
            println!("MISMATCH  {:<40} bytes {:02x?}", text, &bytes[..len]);
            let regs: Vec<String> = (0..8u32)
                .map(|i| {
                    let p = ((before.x87.top + i) & 7) as usize;
                    format!("{}{:e}", if before.x87.empty & (1 << p) != 0 { "E:" } else { "" }, before.x87.st[p])
                })
                .collect();
            println!(
                "   in: cw {:#x} sw {:#06x} top {} st(0..7) [{}] eflags {:#x}",
                before.x87.cw,
                fpu::status_word(&before),
                before.x87.top,
                regs.join(", "),
                before.eflags
            );
            for d in diffs {
                println!("   {}", d);
            }
        }
    }
    println!("\n{} x87 instructions tested, {} mismatches ({} faulted in both)", tested, failures, faults);
    let mut all: Vec<_> = per_mnemonic.iter().collect();
    all.sort();
    let summary: Vec<String> = all.iter().map(|(m, (n, f))| format!("{}:{}{}", m, n, if *f > 0 { format!("/{}F", f) } else { String::new() })).collect();
    println!("per mnemonic: {}", summary.join(" "));
    println!("fprem/fprem1 verified against the exact remainder instead of Unicorn: {}", prem_exact_ref);
    if !quirks.is_empty() {
        println!("skipped (known Unicorn/QEMU 5.0 deviations): {:?}", quirks);
    }
    if !unimpl.is_empty() {
        println!("unimplemented (skipped): {:?}", unimpl);
    }
}

fn has_mem(insn: &Instruction) -> bool {
    (0..insn.op_count()).any(|i| insn.op_kind(i) == OpKind::Memory)
}
