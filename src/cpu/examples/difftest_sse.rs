//! Differential tester for SSE/SSE2/SSE3/SSSE3/MMX: random instructions that
//! touch xmm/mm registers are executed one at a time in Unicorn (reference,
//! CPU model Core2Duo = up to SSSE3) and in dxcpu's interpreter. GPRs, eflags,
//! XMM and MM registers (bit-exact), MXCSR control bits and memory are compared.
//! Register and memory contents mix random bits with special float values
//! (zeros, denormals, infinities, NaNs, small integers); MXCSR rounding mode,
//! FTZ and DAZ are randomised occasionally.
//!
//!   cargo run --release --example difftest_sse -- [iterations] [seed]
//!
//! Known reference deviations that are tolerated (and counted):
//! * both operands NaN in a binary float op: x86 returns the first operand's
//!   NaN, Unicorn (QEMU softfloat) picks by x87 rules; any NaN accepted there.

use dxcpu::interp::Engine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::{
    Decoder, DecoderOptions, Formatter, Instruction, InstructionInfoFactory, IntelFormatter, Mnemonic, OpAccess,
    OpKind, Register,
};
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
const UC_CTL_CPU_MODEL_W: u32 = 1140850695; // UC_CTL_WRITE(UC_CTL_CPU_MODEL, 1)
const UC_CPU_X86_CORE2DUO: i32 = 2;
const UC_MODE_32: i32 = 4;
const UC_REGS: [i32; 8] = [19, 22, 24, 21, 30, 20, 29, 23]; // eax ecx edx ebx esp ebp esi edi
const UC_EFLAGS: i32 = 25;
const UC_XMM0: i32 = 122;
const UC_ST0: i32 = 114; // MMn = mantissa of physical x87 register n (top is 0)
const UC_MXCSR: i32 = 249;
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
        // high half: xorshift's low bits are correlated between consecutive outputs,
        // which made some opcode/ModRM combinations unreachable
        (self.next().wrapping_mul(0x2545_f491_4f6c_dd1d) >> 32) as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.u32() % n
    }
}

fn special32(rng: &mut Rng) -> u32 {
    let sign = if rng.below(2) == 0 { 0 } else { 0x8000_0000 };
    let v = match rng.below(14) {
        0 => 0,
        1 => 0x7f80_0000,                              // inf
        2 => 0x7fc0_0000 | (rng.u32() & 0x003f_ffff), // qnan
        3 => 0x7f80_0001 | (rng.u32() & 0x003f_ffff), // snan
        4 => rng.u32() & 0x007f_ffff,                  // denormal
        5 => 0x0080_0000 | (rng.u32() & 0xf),          // tiny normal
        6 => 0x7f7f_ffff - (rng.u32() & 0xf),          // huge
        7 => (rng.below(2000) as f32).to_bits(),       // small integer
        8 => (rng.below(200) as f32 + 0.5).to_bits(),  // halves
        9 => 0x4f00_0000 + rng.below(4) - 2,           // around 2^31
        10 => 0x3f80_0000,                             // 1.0
        11 => 0x0000_0001 + rng.below(4),              // smallest denormals
        12 => ((rng.u32() % 64 + 100) << 23) | (rng.u32() & 0x7f_ffff), // moderate exponents
        _ => rng.u32() & 0x7fff_ffff,
    };
    (v & 0x7fff_ffff) | sign
}

fn special64(rng: &mut Rng) -> u64 {
    let sign = if rng.below(2) == 0 { 0 } else { 1u64 << 63 };
    let r = rng.next();
    let v = match rng.below(14) {
        0 => 0,
        1 => 0x7ff0_0000_0000_0000,
        2 => 0x7ff8_0000_0000_0000 | (r & 0x0007_ffff_ffff_ffff),
        3 => 0x7ff0_0000_0000_0001 | (r & 0x0007_ffff_ffff_ffff),
        4 => r & 0x000f_ffff_ffff_ffff,
        5 => 0x0010_0000_0000_0000 | (r & 0xf),
        6 => 0x7fef_ffff_ffff_ffff - (r & 0xf),
        7 => (rng.below(2000) as f64).to_bits(),
        8 => (rng.below(200) as f64 + 0.5).to_bits(),
        9 => (2147483648.0f64 + (rng.below(5) as f64 - 2.0) * 0.5).to_bits(),
        10 => 0x3ff0_0000_0000_0000,
        11 => (special32(rng) as f32 as f64).to_bits(), // f32-representable
        12 => ((r % 128 + 960) << 52) | (r & 0x000f_ffff_ffff_ffff),
        _ => r & 0x7fff_ffff_ffff_ffff,
    };
    (v & 0x7fff_ffff_ffff_ffff) | sign
}

/// 16 random bytes: random bits, f32 lanes or f64 lanes.
fn rand_vec(rng: &mut Rng) -> [u8; 16] {
    let mut v = [0u8; 16];
    match rng.below(4) {
        0 => {
            for b in v.iter_mut() {
                *b = rng.u32() as u8;
            }
        }
        1 | 2 => {
            for i in 0..4 {
                let x = if rng.below(4) == 0 { rng.u32() } else { special32(rng) };
                v[4 * i..4 * i + 4].copy_from_slice(&x.to_le_bytes());
            }
        }
        _ => {
            for i in 0..2 {
                let x = if rng.below(4) == 0 { rng.next() } else { special64(rng) };
                v[8 * i..8 * i + 8].copy_from_slice(&x.to_le_bytes());
            }
        }
    }
    v
}

fn acceptable(insn: &Instruction, info: &mut InstructionInfoFactory) -> bool {
    if insn.is_invalid() || insn.has_rep_prefix() || insn.has_repne_prefix() || insn.has_repe_prefix() {
        return false;
    }
    let m = insn.mnemonic();
    let name = format!("{:?}", m);
    if name.starts_with('F') {
        return false; // x87 (fxsave etc.)
    }
    for i in 0..insn.op_count() {
        if insn.op_kind(i) == OpKind::Register
            && (insn.op_register(i).is_dr() || insn.op_register(i).is_cr() || insn.op_register(i).is_tr())
        {
            return false;
        }
    }
    // must touch xmm or mm registers, and nothing exotic
    let s = format!("{:?}", insn.code());
    let touches = (0..insn.op_count())
        .any(|i| insn.op_kind(i) == OpKind::Register && (insn.op_register(i).is_xmm() || insn.op_register(i).is_mm()))
        || matches!(m, Mnemonic::Stmxcsr | Mnemonic::Ldmxcsr);
    if !touches || s.starts_with("EVEX") || s.starts_with("VEX") || s.starts_with("XOP") || s.starts_with("D3NOW") {
        return false;
    }
    // legacy cmpps & co: predicates >= 8 are reserved
    if matches!(m, Mnemonic::Cmpps | Mnemonic::Cmpss | Mnemonic::Cmppd | Mnemonic::Cmpsd) && insn.immediate8() >= 8 {
        return false;
    }
    // memory operands: only [ebx+disp] (or [edi] for maskmov*)
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => {
                if insn.memory_base() != Register::EBX
                    || insn.memory_index() != Register::None
                    || (insn.memory_displacement32() as i32).abs() > 1024
                    || !matches!(insn.memory_segment(), Register::DS | Register::SS | Register::ES)
                {
                    return false;
                }
            }
            OpKind::MemorySegEDI => {
                if insn.memory_segment() != Register::DS {
                    return false;
                }
            }
            OpKind::Register if insn.op_register(i).is_segment_register() => return false,
            OpKind::Register | OpKind::Immediate8 | OpKind::Immediate16 | OpKind::Immediate32
            | OpKind::Immediate8to16 | OpKind::Immediate8to32 | OpKind::Immediate8_2nd => {}
            _ => return false,
        }
    }
    let ii = info.info(insn);
    for r in ii.used_registers() {
        let reg = r.register().full_register32();
        let writes = !matches!(r.access(), OpAccess::Read | OpAccess::CondRead | OpAccess::None);
        if r.register().is_segment_register() {
            if writes {
                return false;
            }
            continue;
        }
        if writes && matches!(reg, Register::EBX | Register::ESP | Register::ESI | Register::EDI) {
            return false;
        }
    }
    true
}

fn is_nan32(x: u32) -> bool {
    x & 0x7f80_0000 == 0x7f80_0000 && x & 0x007f_ffff != 0
}
fn is_nan64(x: u64) -> bool {
    x & 0x7ff0_0000_0000_0000 == 0x7ff0_0000_0000_0000 && x & 0x000f_ffff_ffff_ffff != 0
}

/// Binary float op where Unicorn's NaN selection differs from hardware when both inputs are NaN.
fn nan_tolerant(m: Mnemonic) -> Option<bool /* f64 lanes */> {
    let s = format!("{:?}", m);
    let float_bin = ["Add", "Sub", "Mul", "Div", "Hadd", "Hsub", "Addsub"].iter().any(|p| s.starts_with(p))
        && ["ps", "pd", "ss", "sd"].iter().any(|p| s.ends_with(p));
    if !float_bin {
        return None;
    }
    Some(s.ends_with("pd") || s.ends_with("sd"))
}

/// All differing lanes are NaN on both sides?
fn nan_lanes_only(u: &[u8; 16], d: &[u8; 16], f64lanes: bool) -> bool {
    if f64lanes {
        (0..2).all(|i| {
            let a = u64::from_le_bytes(u[8 * i..8 * i + 8].try_into().unwrap());
            let b = u64::from_le_bytes(d[8 * i..8 * i + 8].try_into().unwrap());
            a == b || (is_nan64(a) && is_nan64(b))
        })
    } else {
        (0..4).all(|i| {
            let a = u32::from_le_bytes(u[4 * i..4 * i + 4].try_into().unwrap());
            let b = u32::from_le_bytes(d[4 * i..4 * i + 4].try_into().unwrap());
            a == b || (is_nan32(a) && is_nan32(b))
        })
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

fn write_mm(uc: &Uc, i: usize, v: u64) {
    let mut b = [0xffu8; 16];
    b[..8].copy_from_slice(&v.to_le_bytes());
    unsafe { uc_reg_write(uc.0, UC_ST0 + i as i32, b.as_ptr() as *const c_void) };
}
fn read_mm(uc: &Uc, i: usize) -> u64 {
    let mut b = [0u8; 16];
    unsafe { uc_reg_read(uc.0, UC_ST0 + i as i32, b.as_mut_ptr() as *mut c_void) };
    u64::from_le_bytes(b[..8].try_into().unwrap())
}

// --- second oracle: the same instruction executed as x86_64 code under Rosetta 2 -------------
//
// Unicorn (QEMU 5 softfloat) ignores MXCSR.FTZ, returns raw denormals from
// min/max under DAZ, selects NaNs by x87 rules and has a few SSSE3 bugs.
// Rosetta implements x86 SSE semantics on Apple's FP hardware (FEAT_AFP),
// so when it can run the instruction it is the primary reference. The
// register/memory encodings used here are identical in 32- and 64-bit mode.

const ORACLE_C: &str = r#"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
struct St { uint32_t gpr[8]; uint32_t eflags; uint32_t mxcsr; uint64_t pad; uint8_t xmm[8][16];
            uint64_t mm[8]; uint64_t rbx; uint64_t rdi; };
struct Req { uint8_t code[16]; struct St st; uint32_t memoff; uint8_t mem[16]; uint32_t pad2[3]; };
struct Resp { struct St st; uint8_t mem[16]; uint32_t status; uint32_t pad[3]; };
struct St *g_state; void *g_code; uint32_t g_default_mxcsr = 0x1f80;
extern void run_insn(void);
__asm__(
".globl _run_insn\n_run_insn:\n"
" push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
" mov _g_state(%rip), %rax\n ldmxcsr 36(%rax)\n"
" movdqu 48(%rax), %xmm0\n movdqu 64(%rax), %xmm1\n movdqu 80(%rax), %xmm2\n movdqu 96(%rax), %xmm3\n"
" movdqu 112(%rax), %xmm4\n movdqu 128(%rax), %xmm5\n movdqu 144(%rax), %xmm6\n movdqu 160(%rax), %xmm7\n"
" movq 176(%rax), %mm0\n movq 184(%rax), %mm1\n movq 192(%rax), %mm2\n movq 200(%rax), %mm3\n"
" movq 208(%rax), %mm4\n movq 216(%rax), %mm5\n movq 224(%rax), %mm6\n movq 232(%rax), %mm7\n"
" movl 32(%rax), %ecx\n push %rcx\n popfq\n"
" movl 4(%rax), %ecx\n movl 8(%rax), %edx\n movl 20(%rax), %ebp\n movl 24(%rax), %esi\n"
" mov 240(%rax), %rbx\n mov 248(%rax), %rdi\n movl 0(%rax), %eax\n"
" call *_g_code(%rip)\n"
" pushfq\n push %rax\n mov _g_state(%rip), %rax\n"
" pop %r8\n movl %r8d, 0(%rax)\n pop %r8\n movl %r8d, 32(%rax)\n"
" movl %ecx, 4(%rax)\n movl %edx, 8(%rax)\n movl %ebp, 20(%rax)\n movl %esi, 24(%rax)\n"
" stmxcsr 36(%rax)\n"
" movdqu %xmm0, 48(%rax)\n movdqu %xmm1, 64(%rax)\n movdqu %xmm2, 80(%rax)\n movdqu %xmm3, 96(%rax)\n"
" movdqu %xmm4, 112(%rax)\n movdqu %xmm5, 128(%rax)\n movdqu %xmm6, 144(%rax)\n movdqu %xmm7, 160(%rax)\n"
" movq %mm0, 176(%rax)\n movq %mm1, 184(%rax)\n movq %mm2, 192(%rax)\n movq %mm3, 200(%rax)\n"
" movq %mm4, 208(%rax)\n movq %mm5, 216(%rax)\n movq %mm6, 224(%rax)\n movq %mm7, 232(%rax)\n"
" emms\n ldmxcsr _g_default_mxcsr(%rip)\n"
" pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n");
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) { sig = s; siglongjmp(jb, 1); }
static uint8_t buf[0x2000] __attribute__((aligned(16)));
int main(void) {
    uint8_t *code = mmap(0, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) return 1;
    signal(SIGSEGV, handler); signal(SIGBUS, handler); signal(SIGILL, handler); signal(SIGFPE, handler);
    struct Req rq; struct Resp rs; struct St st;
    int slot = 0;
    while (fread(&rq, sizeof rq, 1, stdin) == 1) {
        uint8_t *c = code + (slot++ % 512) * 32;
        memcpy(c, rq.code, 16);
        c[rq.code[15]] = 0xC3;
        g_code = c;
        st = rq.st;
        memset(buf, 0, sizeof buf);
        memcpy(buf + rq.memoff, rq.mem, 16);
        st.rbx = (uint64_t)(buf + st.rbx);
        st.rdi = (uint64_t)(buf + st.rdi);
        g_state = &st;
        sig = 0;
        if (sigsetjmp(jb, 1) == 0) run_insn();
        else __asm__ volatile("emms; ldmxcsr %0" :: "m"(g_default_mxcsr));
        rs.st = st;
        memcpy(rs.mem, buf + rq.memoff, 16);
        rs.status = sig;
        fwrite(&rs, sizeof rs, 1, stdout);
        fflush(stdout);
    }
    return 0;
}
"#;

#[repr(C)]
#[derive(Clone, Copy)]
struct OSt {
    gpr: [u32; 8],
    eflags: u32,
    mxcsr: u32,
    pad: u64,
    xmm: [[u8; 16]; 8],
    mm: [u64; 8],
    rbx: u64,
    rdi: u64,
}
#[repr(C)]
struct OReq {
    code: [u8; 16],
    st: OSt,
    memoff: u32,
    mem: [u8; 16],
    pad: [u32; 3],
}
#[repr(C)]
struct OResp {
    st: OSt,
    mem: [u8; 16],
    status: u32,
    pad: [u32; 3],
}

struct Oracle {
    child: std::process::Child,
    stdin: std::process::ChildStdin,
    stdout: std::process::ChildStdout,
}

impl Oracle {
    fn start() -> Option<Oracle> {
        use std::process::{Command, Stdio};
        let dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
        let src = dir.join("sse_oracle.c");
        let bin = dir.join("sse_oracle_x86_64");
        std::fs::write(&src, ORACLE_C).ok()?;
        let ok = Command::new("clang")
            .args(["-O1", "-arch", "x86_64", "-o"])
            .arg(&bin)
            .arg(&src)
            .status()
            .ok()?
            .success();
        if !ok {
            return None;
        }
        let mut child = Command::new(&bin).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().ok()?;
        let stdin = child.stdin.take()?;
        let stdout = child.stdout.take()?;
        Some(Oracle { child, stdin, stdout })
    }

    fn run(&mut self, req: &OReq) -> OResp {
        use std::io::{Read, Write};
        let b = unsafe { std::slice::from_raw_parts(req as *const OReq as *const u8, std::mem::size_of::<OReq>()) };
        self.stdin.write_all(b).unwrap();
        self.stdin.flush().unwrap();
        let mut resp: OResp = unsafe { std::mem::zeroed() };
        let rb = unsafe {
            std::slice::from_raw_parts_mut(&mut resp as *mut OResp as *mut u8, std::mem::size_of::<OResp>())
        };
        self.stdout.read_exact(rb).unwrap();
        resp
    }
}

impl Drop for Oracle {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

/// Can the instruction run unchanged as x86_64 code in the oracle?
fn oracle_eligible(insn: &Instruction, bytes: &[u8]) -> bool {
    // Rosetta forces all MXCSR exception masks on (no unmasked SIMD exceptions)
    if insn.mnemonic() == Mnemonic::Ldmxcsr {
        return false;
    }
    let mut dec = Decoder::with_ip(64, bytes, CODE as u64, DecoderOptions::NONE);
    let i64 = dec.decode();
    if i64.is_invalid() || i64.mnemonic() != insn.mnemonic() || i64.len() != insn.len() || i64.op_count() != insn.op_count() {
        return false;
    }
    for i in 0..insn.op_count() {
        if insn.op_kind(i) != i64.op_kind(i) {
            return false;
        }
        if insn.op_kind(i) == OpKind::Register {
            let r = insn.op_register(i);
            if matches!(r.full_register32(), Register::EBX | Register::ESP | Register::EDI) {
                return false;
            }
        }
    }
    true
}

/// Memory address an instruction may access (16-byte window), relative to DATA.
fn mem_window(insn: &Instruction, st: &CpuState) -> u32 {
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => return st.gpr[EBX].wrapping_add(insn.memory_displacement32()) - DATA,
            OpKind::MemorySegEDI => return st.gpr[EDI] - DATA,
            _ => {}
        }
    }
    0x800
}

/// rcp/rsqrt lanes: equal, or both normal and within 2^-11 relative (Intel: |err| <= 1.5*2^-12).
fn approx_equal(a: &[u8; 16], b: &[u8; 16]) -> bool {
    (0..4).all(|i| {
        let x = u32::from_le_bytes(a[4 * i..4 * i + 4].try_into().unwrap());
        let y = u32::from_le_bytes(b[4 * i..4 * i + 4].try_into().unwrap());
        let (fx, fy) = (f32::from_bits(x) as f64, f32::from_bits(y) as f64);
        x == y || (fx.is_normal() && fy.is_normal() && ((fx - fy) / fx).abs() <= 1.0 / 2048.0)
    })
}

fn is_approx(m: Mnemonic) -> bool {
    matches!(m, Mnemonic::Rcpps | Mnemonic::Rcpss | Mnemonic::Rsqrtps | Mnemonic::Rsqrtss)
}

struct Outcome {
    fault: bool,
    gpr: [u32; 8],
    eflags: u32,
    mxcsr: u32,
    xmm: [[u8; 16]; 8],
    mm: [u64; 8],
    mem: [u8; 16],
}

/// Compare dxcpu's outcome with a reference; `gpr_mask` selects compared GPRs.
fn compare(r: &Outcome, d: &Outcome, gpr_mask: u8, mxcsr_mask: u32, insn: &Instruction, tolerate_nan: bool, tolerated: &mut bool) -> Vec<String> {
    let mut diffs = Vec::new();
    if r.fault || d.fault {
        if r.fault != d.fault {
            diffs.push(format!("fault: reference {} dxcpu {}", r.fault, d.fault));
        }
        return diffs;
    }
    for i in 0..8 {
        if gpr_mask & (1 << i) != 0 && r.gpr[i] != d.gpr[i] {
            diffs.push(format!(
                "{}: reference {:#010x} dxcpu {:#010x}",
                ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"][i],
                r.gpr[i],
                d.gpr[i]
            ));
        }
    }
    let fmask = flags::ARITH | flags::DF;
    if (r.eflags ^ d.eflags) & fmask != 0 {
        diffs.push(format!("eflags: reference {:#x} dxcpu {:#x}", r.eflags & fmask, d.eflags & fmask));
    }
    for i in 0..8 {
        if r.xmm[i] != d.xmm[i] {
            if is_approx(insn.mnemonic()) && approx_equal(&r.xmm[i], &d.xmm[i]) {
                continue;
            }
            if tolerate_nan {
                if let Some(f64l) = nan_tolerant(insn.mnemonic()) {
                    if nan_lanes_only(&r.xmm[i], &d.xmm[i], f64l) {
                        *tolerated = true;
                        continue;
                    }
                }
            }
            diffs.push(format!("xmm{}: reference {:02x?}\n         dxcpu     {:02x?}", i, r.xmm[i], d.xmm[i]));
        }
        if r.mm[i] != d.mm[i] {
            diffs.push(format!("mm{}: reference {:#018x} dxcpu {:#018x}", i, r.mm[i], d.mm[i]));
        }
    }
    if (r.mxcsr ^ d.mxcsr) & mxcsr_mask != 0 {
        diffs.push(format!("mxcsr: reference {:#x} dxcpu {:#x}", r.mxcsr, d.mxcsr));
    }
    if r.mem != d.mem {
        diffs.push(format!("memory: reference {:02x?} dxcpu {:02x?}", r.mem, d.mem));
    }
    diffs
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(|s| s.as_str()) == Some("--probe") {
        // --probe <hex bytes>: show whether the fuzzer accepts an encoding
        let bytes: Vec<u8> = (0..args[2].len() / 2).map(|i| u8::from_str_radix(&args[2][2 * i..2 * i + 2], 16).unwrap()).collect();
        let insn = Decoder::with_ip(32, &bytes, CODE as u64, DecoderOptions::NONE).decode();
        let mut info = InstructionInfoFactory::new();
        println!("{:?} acceptable={} oracle={}", insn.code(), acceptable(&insn, &mut info), oracle_eligible(&insn, &bytes[..insn.len()]));
        return;
    }
    let iterations: usize = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(20000);
    let seed: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0x1234_5678);
    let use_oracle = !args.iter().any(|a| a == "--unicorn-only");
    // MXCSR exception flags are compared against Rosetta (except DE, which Rosetta does not report)
    let strict_flags = !args.iter().any(|a| a == "--no-flags");
    let mut rng = Rng(seed | 1);
    let mut mem_uc = vec![0u8; MEM_SIZE];
    let mut mem_ip = vec![0u8; MEM_SIZE];
    mem::set_base(mem_ip.as_mut_ptr());

    let mut oracle = if use_oracle { Oracle::start() } else { None };
    println!("Rosetta oracle: {}", if oracle.is_some() { "enabled" } else { "not available (Unicorn only)" });

    let mut uc_raw = std::ptr::null_mut();
    unsafe {
        assert_eq!(uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc_raw), 0);
        assert_eq!(uc_ctl(uc_raw, UC_CTL_CPU_MODEL_W, UC_CPU_X86_CORE2DUO), 0);
        assert_eq!(uc_mem_map_ptr(uc_raw, 0, MEM_SIZE, 7, mem_uc.as_mut_ptr() as *mut c_void), 0);
    }
    let uc = Uc(uc_raw);
    wreg(&uc, UC_CR0, 0x33);
    wreg(&uc, UC_CR4, 0x600);
    let mut eng = Engine::new(0xFFFF_0000, 0xFFFF_0010);
    let mut info = InstructionInfoFactory::new();
    let mut fmt = IntelFormatter::new();
    let (mut tested, mut failures, mut nan_tolerated, mut flag_diffs, mut by_oracle) = (0usize, 0, 0, 0, 0);
    let mut ros_nan_tolerated = 0usize;
    let mut per_mnemonic: std::collections::BTreeMap<String, (usize, usize)> = Default::default();
    let mut unimpl: std::collections::BTreeMap<String, usize> = Default::default();
    let mut both_fault: std::collections::BTreeMap<String, usize> = Default::default();
    let mut uc_wrong: std::collections::BTreeMap<String, usize> = Default::default();
    let mut ros_wrong: std::collections::BTreeMap<String, usize> = Default::default();

    while tested < iterations && failures < 40 {
        let mut bytes = [0u8; 16];
        for b in bytes.iter_mut() {
            *b = rng.u32() as u8;
        }
        // bias towards the SSE/MMX opcode space: [66|F2|F3] 0F [38|3A] xx
        if rng.below(4) != 0 {
            let pre = [0x66u8, 0xF2, 0xF3, 0x90][rng.below(4) as usize];
            let mut k = 0;
            if pre != 0x90 {
                bytes[0] = pre;
                k = 1;
            }
            bytes[k] = 0x0F;
            if rng.below(4) == 0 {
                bytes[k + 1] = [0x38, 0x3A][rng.below(2) as usize];
            }
        }
        let mut dec = Decoder::with_ip(32, &bytes, CODE as u64, DecoderOptions::NONE);
        let insn = dec.decode();
        if !acceptable(&insn, &mut info) {
            continue;
        }
        let len = insn.len();
        for m in [&mut mem_uc, &mut mem_ip] {
            m[CODE as usize..CODE as usize + len].copy_from_slice(&bytes[..len]);
            m[CODE as usize + len] = 0xF4;
        }
        for m in [&mut mem_uc, &mut mem_ip] {
            m[(STACK - 256) as usize..(STACK + 256) as usize].fill(0x5a);
        }
        for i in 0..(DATA_SIZE as usize / 16) {
            let v = rand_vec(&mut rng);
            let a = DATA as usize + 16 * i;
            mem_uc[a..a + 16].copy_from_slice(&v);
            mem_ip[a..a + 16].copy_from_slice(&v);
        }
        let mut st = CpuState::new();
        for i in 0..8 {
            st.gpr[i] = match rng.below(4) {
                0 => rng.u32() % 64,
                1 => (rng.u32() % 4000) ^ if rng.below(2) == 0 { 0 } else { !0 },
                _ => rng.u32(),
            };
        }
        st.gpr[EBX] = DATA + 0x800;
        st.gpr[ESI] = DATA + 0x400;
        st.gpr[EDI] = DATA + 0xC00 + rng.below(32);
        st.gpr[ESP] = STACK;
        st.eflags = 0x202 | (rng.u32() & flags::ARITH);
        st.eip = CODE;
        for i in 0..8 {
            st.xmm[i] = rand_vec(&mut rng);
            let v = rand_vec(&mut rng);
            st.mm[i] = u64::from_le_bytes(v[..8].try_into().unwrap());
        }
        st.mxcsr = 0x1f80;
        if rng.below(3) == 0 {
            st.mxcsr |= rng.below(4) << 13;
        }
        if rng.below(6) == 0 {
            st.mxcsr |= 0x8000; // FTZ
        }
        if rng.below(6) == 0 {
            st.mxcsr |= 0x40; // DAZ
        }
        for i in 0..8 {
            wreg(&uc, UC_REGS[i], st.gpr[i] as u64);
        }
        wreg(&uc, UC_EFLAGS, st.eflags as u64);
        for i in 0..8 {
            unsafe { uc_reg_write(uc.0, UC_XMM0 + i as i32, st.xmm[i].as_ptr() as *const c_void) };
            write_mm(&uc, i, st.mm[i]);
        }
        wreg(&uc, UC_MXCSR, st.mxcsr as u64);
        let before = st;
        let memoff = mem_window(&insn, &st);
        let mut mem_before = [0u8; 16];
        mem_before.copy_from_slice(&mem_ip[(DATA + memoff) as usize..(DATA + memoff) as usize + 16]);

        unsafe { uc_ctl(uc.0, UC_CTL_TB_REMOVE_CACHE_W, CODE as u64, CODE as u64 + 32) };
        let uc_err = unsafe { uc_emu_start(uc.0, CODE as u64, 0, 0, 1) };
        eng.flush();
        let exit = eng.run(&mut st, CODE + len as u32);
        let mname = format!("{:?}", insn.mnemonic());
        if exit == Exit::Unimplemented {
            *unimpl.entry(mname).or_default() += 1;
            continue;
        }
        tested += 1;
        let mut text = String::new();
        fmt.format(&insn, &mut text);
        let e = per_mnemonic.entry(mname.clone()).or_default();
        e.0 += 1;

        let win = |m: &Vec<u8>| -> [u8; 16] {
            m[(DATA + memoff) as usize..(DATA + memoff) as usize + 16].try_into().unwrap()
        };
        let mine = Outcome {
            fault: exit != Exit::Stop,
            gpr: st.gpr,
            eflags: st.eflags,
            mxcsr: st.mxcsr,
            xmm: st.xmm,
            mm: st.mm,
            mem: win(&mem_ip),
        };
        let mut uc_out = Outcome {
            fault: uc_err != 0,
            gpr: [0; 8],
            eflags: rreg(&uc, UC_EFLAGS) as u32,
            mxcsr: rreg(&uc, UC_MXCSR) as u32,
            xmm: [[0; 16]; 8],
            mm: [0; 8],
            mem: win(&mem_uc),
        };
        for i in 0..8 {
            uc_out.gpr[i] = rreg(&uc, UC_REGS[i]) as u32;
            unsafe { uc_reg_read(uc.0, UC_XMM0 + i as i32, uc_out.xmm[i].as_mut_ptr() as *mut c_void) };
            uc_out.mm[i] = read_mm(&uc, i);
        }
        // ldmxcsr with reserved bits: #GP on hardware, Unicorn accepts it
        if insn.mnemonic() == Mnemonic::Ldmxcsr && mine.fault && !uc_out.fault {
            let v = u32::from_le_bytes(mem_before[..4].try_into().unwrap());
            if v & 0xffff_0000 != 0 {
                uc_out.fault = true;
            }
        }
        let mut tolerated = false;
        let mut uc_diffs = compare(&uc_out, &mine, 0xff, 0xffc0, &insn, true, &mut tolerated);
        if !uc_out.fault && !mine.fault {
            if mem_uc[DATA as usize..(DATA + DATA_SIZE) as usize] != mem_ip[DATA as usize..(DATA + DATA_SIZE) as usize] {
                uc_diffs.push("data memory differs".into());
            }
            if mem_uc[(STACK - 64) as usize..(STACK + 64) as usize] != mem_ip[(STACK - 64) as usize..(STACK + 64) as usize]
            {
                uc_diffs.push("stack memory differs".into());
            }
            if (uc_out.mxcsr ^ mine.mxcsr) & 0x3f != 0 {
                flag_diffs += 1;
            }
        } else if uc_out.fault && mine.fault {
            *both_fault.entry(mname.clone()).or_default() += 1;
        }

        let mut ros_diffs = None;
        if let Some(o) = oracle.as_mut() {
            if oracle_eligible(&insn, &bytes[..len]) {
                let mut code = [0u8; 16];
                code[..len].copy_from_slice(&bytes[..len]);
                code[15] = len as u8;
                let req = OReq {
                    code,
                    st: OSt {
                        gpr: before.gpr,
                        eflags: before.eflags,
                        mxcsr: before.mxcsr,
                        pad: 0,
                        xmm: before.xmm,
                        mm: before.mm,
                        rbx: (before.gpr[EBX] - DATA) as u64,
                        rdi: (before.gpr[EDI] - DATA) as u64,
                    },
                    memoff,
                    mem: mem_before,
                    pad: [0; 3],
                };
                let r = o.run(&req);
                let ros = Outcome {
                    fault: r.status != 0,
                    gpr: r.st.gpr,
                    eflags: r.st.eflags,
                    mxcsr: r.st.mxcsr,
                    xmm: r.st.xmm,
                    mm: r.st.mm,
                    mem: r.mem,
                };
                let mut ros = ros;
                if insn.mnemonic() == Mnemonic::Ldmxcsr && mine.fault && !ros.fault {
                    let v = u32::from_le_bytes(mem_before[..4].try_into().unwrap());
                    if v & 0xffff_0000 != 0 {
                        ros.fault = true; // #GP on hardware; Rosetta accepts reserved bits
                    }
                }
                // Rosetta flips the sign of a NaN subtrahend in hsub*/addsub* (subps etc. are fine)
                let ros_nan_quirk = matches!(
                    insn.mnemonic(),
                    Mnemonic::Hsubps | Mnemonic::Hsubpd | Mnemonic::Addsubps | Mnemonic::Addsubpd
                );
                let mut t = false;
                // eax ecx edx ebp esi (ebx/edi are host pointers in the oracle, esp untouched)
                let rd = compare(&ros, &mine, 0b0110_0111, if strict_flags { 0xfffd } else { 0xffc0 }, &insn, ros_nan_quirk, &mut t);
                if t && rd.is_empty() {
                    ros_nan_tolerated += 1;
                }
                if rd.is_empty() && !ros.fault && (ros.mxcsr ^ mine.mxcsr) & 0x3f != 0 {
                    *ros_wrong.entry(format!("{} {:#x}->{:#x}", mname, ros.mxcsr & 0x3f, mine.mxcsr & 0x3f)).or_default() += 1;
                }
                ros_diffs = Some(rd);
            }
        }

        let diffs = match ros_diffs {
            Some(rd) => {
                by_oracle += 1;
                if rd.is_empty() {
                    if !uc_diffs.is_empty() {
                        *uc_wrong.entry(mname.clone()).or_default() += 1;
                    }
                    Vec::new()
                } else {
                    let mut v: Vec<String> = rd.into_iter().map(|s| format!("[rosetta] {}", s)).collect();
                    v.extend(uc_diffs.into_iter().map(|s| format!("[unicorn] {}", s)));
                    v
                }
            }
            None => {
                if uc_diffs.is_empty() && tolerated {
                    nan_tolerated += 1;
                }
                uc_diffs.into_iter().map(|s| format!("[unicorn] {}", s)).collect()
            }
        };
        if !diffs.is_empty() {
            failures += 1;
            e.1 += 1;
            println!("MISMATCH  {:<40} bytes {:02x?}", text, &bytes[..len]);
            println!("   in: gpr {:08x?} eflags {:#x} mxcsr {:#x}", before.gpr, before.eflags, before.mxcsr);
            for i in 0..8 {
                if insn_uses_xmm(&insn, i) {
                    println!("   in: xmm{} {:02x?}", i, before.xmm[i]);
                }
            }
            println!("   in: mem@{:#x} {:02x?}", DATA + memoff, mem_before);
            for d in diffs {
                println!("   {}", d);
            }
        }
    }
    println!("\n{} instructions tested, {} mismatches", tested, failures);
    println!(
        "{} distinct mnemonics; {} checked against Rosetta, rest against Unicorn; {} Unicorn-only both-NaN diffs tolerated; {} MXCSR exception-flag diffs vs Unicorn (not compared)",
        per_mnemonic.len(),
        by_oracle,
        nan_tolerated,
        flag_diffs
    );
    let mut bad: Vec<_> = per_mnemonic.iter().filter(|(_, v)| v.1 > 0).collect();
    bad.sort();
    for (m, (n, f)) in bad {
        println!("  {:<12} {}/{} failed", m, f, n);
    }
    if args.iter().any(|a| a == "-v") {
        println!("tested: {:?}", per_mnemonic.iter().map(|(k, v)| (k.as_str(), v.0)).collect::<Vec<_>>());
    }
    println!("{} hsub/addsub results with Rosetta's flipped-NaN-sign quirk tolerated", ros_nan_tolerated);
    if !uc_wrong.is_empty() {
        println!("Unicorn deviated from Rosetta (dxcpu matched Rosetta): {:?}", uc_wrong);
    }
    if !ros_wrong.is_empty() {
        println!("MXCSR exception flag differences vs Rosetta (rosetta->dxcpu, not counted as mismatches): {:?}", ros_wrong);
    }
    if !both_fault.is_empty() {
        println!("both faulted: {:?}", both_fault);
    }
    if !unimpl.is_empty() {
        println!("unimplemented (skipped): {:?}", unimpl);
    }
}

fn insn_uses_xmm(insn: &Instruction, i: usize) -> bool {
    (0..insn.op_count()).any(|k| {
        insn.op_kind(k) == OpKind::Register
            && insn.op_register(k).is_xmm()
            && insn.op_register(k) as usize - Register::XMM0 as usize == i
    })
}
