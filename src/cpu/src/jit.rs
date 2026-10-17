//! i386 → AArch64 block translator (see docs/JIT.md).
//!
//! Hot integer instructions are translated natively; everything else runs
//! through the interpreter one instruction at a time (`helper_interp`).
//!
//! SSE/SSE2/SSE3 (see `Translator::sse`): guest xmm0-7 are cached in v16-v23
//! within a block (loaded on first use from CpuState.xmm through x13, written
//! back when dirty at exits and before hot-path helper calls; blocks that
//! branch to their own start keep the cache across the back edge). Results are
//! bit-exact with the interpreter:
//! * Float arithmetic (add/sub/mul/div/sqrt) differs from x86 only when the
//!   result is NaN (operand NaN selection, default NaN sign). Results are
//!   checked once per segment of xmm-only instructions (`Seg`); if any lane is
//!   NaN, a cold path restores the xmm state from before the segment and
//!   re-executes it in the interpreter.
//! * min/max use fcmgt + bit select (x86 returns the second operand on NaN or
//!   equality), cmpps & co map to fcmeq/fcmge/fcmgt (+not), float->int
//!   conversions fix up NaN/out-of-range lanes to 0x80000000, comiss/ucomiss
//!   leave the FCMP result in NZCV (lazy kind K_FCMP, mapped to ZF/PF/CF).
//! * Float translation assumes MXCSR control bits 0x1f80 (round to nearest, no
//!   FTZ/DAZ, exceptions masked) and the host's default FPCR. JitEngine keeps a
//!   second code cache for other MXCSR values in which SSE float instructions
//!   go to the interpreter (data movement, logic, shuffles and integer SIMD
//!   stay native); ldmxcsr/fxrstor end their block so that the mode is
//!   re-evaluated. MXCSR exception flags (bits 0-5) are only accumulated by
//!   interpreted instructions (translated code does not set them).

use crate::a64::*;
use crate::codebuf::CodeBuf;
use crate::interp::{self, Engine, Step};
use crate::mem;
use crate::state::flags::*;
use crate::state::*;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Instruction, MemorySize, Mnemonic, OpKind, Register};
use std::collections::HashMap;

// Host register assignment (see docs/JIT.md).
const G: [u32; 8] = [19, 20, 21, 22, 23, 24, 25, 26]; // eax..edi
const BASE: u32 = 27;
const CTX: u32 = 28;
const T0: u32 = 0;
const T1: u32 = 1;
const T2: u32 = 2;
const T3: u32 = 3;
const T4: u32 = 4; // effective address
const T5: u32 = 5;
const TA: u32 = 17; // host address for ldapr/stlr (DXCPU_TSO)
const TD: u32 = 16; // vector data for ldapr/stlr (only a call target elsewhere)
// Lazy flag operands within a block.
const FA: u32 = 9;
const FB: u32 = 10;
const FR: u32 = 11;
const FC: u32 = 12; // carry in for inc/dec
// SSE translation (see `sse` below).
const XB: u32 = 13; // &state.xmm[0] (valid while `xbase` is set)
const NZ: u32 = 15; // saved NZCV around NZCV-clobbering SSE sequences
const VX: u32 = 16; // guest xmm i is cached in v(16 + i)
const V0: u32 = 0; // vector temporaries
const V1: u32 = 1;
const V2: u32 = 2;
const ACC: u32 = 3; // segment NaN-check accumulator
const V4: u32 = 4;
const V5: u32 = 5;
const V6: u32 = 6;
const SAVE: u32 = 24; // v24 + i: pre-segment value of guest xmm i
const XMM_OFF: u32 = std::mem::offset_of!(CpuState, xmm) as u32;
const MXCSR_OFF: u32 = std::mem::offset_of!(CpuState, mxcsr) as u32;
// x87 state, relative to XB (&state.xmm[0]).
const X87_OFF: u32 = std::mem::offset_of!(CpuState, x87) as u32 - XMM_OFF;
const X87_TOP: u32 = X87_OFF + std::mem::offset_of!(X87, top) as u32;
const X87_EMPTY: u32 = X87_OFF + std::mem::offset_of!(X87, empty) as u32;
const X87_CW: u32 = X87_OFF + std::mem::offset_of!(X87, cw) as u32;
const X87_SW: u32 = X87_OFF + std::mem::offset_of!(X87, sw) as u32;
const SW_IE: u32 = 0x0001;
const SW_SF: u32 = 0x0040;

pub const K_MAT: u32 = 0;
pub const K_ADD: u32 = 1;
pub const K_SUB: u32 = 2;
pub const K_LOGIC: u32 = 3;
pub const K_INC: u32 = 4;
pub const K_DEC: u32 = 5;
/// Shifts: fres = result, fcf = CF, fa = OF; SF/ZF/PF from the result, AF = 0 (undefined).
/// In registers the host NZCV holds N/Z of the result, C = !CF, V = OF (the K_SUB
/// convention, so conditions map the same way).
pub const K_SHIFT: u32 = 7;
/// adc / sbb: fa, fb = operands, fres = result, fcf = CF out (carry in folded in).
/// Registers: K_ADD / K_SUB NZCV conventions.
pub const K_ADC: u32 = 8;
pub const K_SBB: u32 = 9;
/// Translator-internal: host NZCV holds the result of an FCMP (ucomiss & co).
/// Never stored in JitCtx (materialised to EFLAGS on flush).
const K_FCMP: u32 = 6;

/// Context shared with translated code; offsets are hard-coded below.
#[repr(C)]
pub struct JitCtx {
    pub gpr: [u32; 8],        // 0
    pub eip: u32,             // 32
    pub eflags: u32,          // 36
    pub fkind: u32,           // 40
    pub fsize: u32,           // 44
    pub fa: u32,              // 48
    pub fb: u32,              // 52
    pub fres: u32,            // 56
    pub fcf: u32,             // 60
    pub base: *mut u8,        // 64
    pub helper_interp: usize, // 72
    pub helper_flags: usize,  // 80
    pub helper_cond: usize,   // 88
    pub state: *mut CpuState, // 96
    pub engine: *mut Engine,  // 104
    pub exit_reason: u32,     // 112
    pub pad: u32,             // 116
    pub ibtc: *mut IbEntry,   // 120: indirect branch target cache of the running Jit
}

/// Indirect branch target cache entry: guest eip -> translated block (see exit_indirect).
/// Empty entries hold eip 0xffffffff, which is never a block start (a guest jump to
/// 0 must fault, not hit an empty entry).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IbEntry {
    eip: u32,
    pad: u32,
    code: usize,
}
const IBTC_BITS: u32 = 12;
const IBTC_SIZE: usize = 1 << IBTC_BITS;
#[inline]
fn ibtc_hash(eip: u32) -> usize {
    ((eip ^ (eip >> 11)) & (IBTC_SIZE as u32 - 1)) as usize
}

const OFF_EIP: u32 = 32;
const OFF_EFLAGS: u32 = 36;
const OFF_FKIND: u32 = 40;
const OFF_FSIZE: u32 = 44;
const OFF_FA: u32 = 48;
const OFF_FRES: u32 = 56;
const OFF_FCF: u32 = 60;
const OFF_BASE: u32 = 64;
const OFF_HELPER_INTERP: u32 = 72;
const OFF_HELPER_FLAGS: u32 = 80;
const OFF_HELPER_COND: u32 = 88;
const OFF_STATE: u32 = 96;
const OFF_IBTC: u32 = 120;

impl JitCtx {
    pub fn new(state: *mut CpuState, engine: *mut Engine) -> Self {
        JitCtx {
            gpr: [0; 8],
            eip: 0,
            eflags: 0x202,
            fkind: K_MAT,
            fsize: 4,
            fa: 0,
            fb: 0,
            fres: 0,
            fcf: 0,
            base: mem::base(),
            helper_interp: helper_interp as usize,
            helper_flags: helper_flags as usize,
            helper_cond: helper_cond as usize,
            state,
            engine,
            exit_reason: 0,
            pad: 0,
            ibtc: std::ptr::null_mut(),
        }
    }

    /// Compute full EFLAGS from the lazy descriptor.
    pub fn materialize(&mut self) {
        if self.fkind == K_MAT {
            return;
        }
        let size = self.fsize;
        let f = match self.fkind {
            K_ADD => interp::add_flags(size, self.fa, self.fb, 0).1,
            K_SUB => interp::sub_flags(size, self.fa, self.fb, 0).1,
            K_LOGIC => interp::logic_flags(size, self.fres),
            K_INC => (interp::add_flags(size, self.fa, 1, 0).1 & !CF) | (self.fcf & CF),
            K_DEC => (interp::sub_flags(size, self.fa, 1, 0).1 & !CF) | (self.fcf & CF),
            K_ADC | K_SBB => {
                let (a, b, r) = (self.fa, self.fb, self.fres);
                let sign = 1u32 << (size * 8 - 1);
                let of = if self.fkind == K_ADC { (a ^ r) & (b ^ r) } else { (a ^ b) & (a ^ r) } & sign;
                (interp::logic_flags(size, r) & !(CF | OF))
                    | (self.fcf & CF)
                    | ((a ^ b ^ r) & AF)
                    | if of != 0 { OF } else { 0 }
            }
            K_SHIFT => {
                (interp::logic_flags(size, self.fres) & !(CF | OF))
                    | (self.fcf & CF)
                    | if self.fa & 1 != 0 { OF } else { 0 }
            }
            _ => 0,
        };
        self.eflags = (self.eflags & !ARITH) | f;
        self.fkind = K_MAT;
    }
}

extern "C" fn helper_flags(ctx: *mut JitCtx) {
    unsafe { (*ctx).materialize() }
}

/// Evaluate an x86 condition (index = Mnemonic of a jcc) from materialised flags.
extern "C" fn helper_cond(ctx: *mut JitCtx, cc: u32) -> u32 {
    let ctx = unsafe { &mut *ctx };
    ctx.materialize();
    let mut st = CpuState::new();
    st.eflags = ctx.eflags;
    let m: Mnemonic = unsafe { std::mem::transmute(cc as u16) };
    interp::cond(&st, m) as u32
}

/// Execute one instruction with the interpreter. Returns 0 to continue the
/// block, 1 if control left the block (ctx.eip set), 2 on a fault.
pub static STATS_ON: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub static FALLBACK_COUNT: [std::sync::atomic::AtomicU64; 2048] = [const { std::sync::atomic::AtomicU64::new(0) }; 2048];
pub static DISPATCH_COUNT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

// Debugging aid: DXCPU_JIT_OFF=sse,fast,int,all sends those instruction groups
// to the interpreter (bisecting translation bugs against the interpreter).
const OFF_SSE: u32 = 1;
const OFF_FAST: u32 = 2;
const OFF_INT: u32 = 4;
const OFF_ALL: u32 = 8;
const OFF_X87: u32 = 16;
/// DXCPU_VERIFY=1: run every block in the interpreter first (memory writes
/// undone), then translated, and report blocks whose results differ. Blocks are
/// neither chained nor looped internally in this mode, so one run_once = one block.
pub fn verify_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| {
        let on = std::env::var_os("DXCPU_VERIFY").is_some();
        if on {
            mem::UNDO_ON.store(true, std::sync::atomic::Ordering::Relaxed);
        }
        on
    })
}

/// DXCPU_PERFMAP=<file>: append "host_start size g_<eip>" per translated block
/// (the perf map format), so host profiles of JIT code can be mapped back to
/// guest addresses.
fn perfmap_add(host: usize, size: usize, eip: u32) {
    use std::io::Write;
    static F: std::sync::OnceLock<Option<std::sync::Mutex<std::fs::File>>> = std::sync::OnceLock::new();
    let f = F.get_or_init(|| {
        let path = std::env::var_os("DXCPU_PERFMAP")?;
        std::fs::OpenOptions::new().create(true).append(true).open(path).ok().map(std::sync::Mutex::new)
    });
    if let Some(f) = f {
        // one write call per line (writeln! on a File writes every piece separately)
        let line = format!("{:x} {:x} g_{:08x}\n", host, size, eip);
        if let Ok(mut f) = f.lock() {
            let _ = f.write_all(line.as_bytes());
        }
    }
}

// Translated-code tracking for invalidation: one bit per 4 KB guest page that
// some block was translated from. The host reports code changes (mprotect to
// executable, munmap, mmap MAP_FIXED); if they touch such a page, CODE_GEN
// moves on and every engine drops its translations at its next dispatch.
pub static CODE_GEN: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
static CODE_PAGES: [std::sync::atomic::AtomicU64; 16384] = [const { std::sync::atomic::AtomicU64::new(0) }; 16384];

pub fn mark_code(lo: u32, hi: u32) {
    use std::sync::atomic::Ordering;
    let (a, b) = (lo >> 12, hi.max(lo + 1).wrapping_sub(1) >> 12);
    for p in a..=b.max(a) {
        CODE_PAGES[(p >> 6) as usize].fetch_or(1 << (p & 63), Ordering::Relaxed);
    }
}

/// Code at [addr, addr+len) may have changed: returns true (and invalidates all
/// translations) if any of it had been translated.
pub fn code_changed(addr: u32, len: u32) -> bool {
    use std::sync::atomic::Ordering;
    if len == 0 {
        return false;
    }
    let end = (addr as u64 + len as u64 - 1).min(0xffff_ffff) as u32;
    let mut hit = false;
    for p in (addr >> 12)..=(end >> 12) {
        let w = &CODE_PAGES[(p >> 6) as usize];
        let bit = 1u64 << (p & 63);
        if w.load(Ordering::Relaxed) & bit != 0 {
            w.fetch_and(!bit, Ordering::Relaxed);
            hit = true;
        }
    }
    if hit {
        CODE_GEN.fetch_add(1, Ordering::Release);
    }
    hit
}

/// x86 store ordering for guest memory (on by default, DXCPU_TSO=0 turns it
/// off). Without it HR crashes in its job system about every other level load
/// (lock-free lists published with plain stores; measured with the autoplay
/// bot: 4 crashes in 8 loads without, 0 in 8 with, at ~11% lower frame rate).
/// ARM may reorder plain loads and stores; x86 only lets a later load pass an
/// earlier store. Loads get dmb ishld after them (unless the next instruction
/// loads from an address computed from the loaded value: load_feeds_next_load),
/// stores dmb ishst before them, stack accesses excepted (see Translator::ea);
/// the interpreter fences likewise (mem.rs). litmus.c shows the difference
/// (message passing).
/// DXCPU_TSO=rcpc: loads as ldapr, stores as stlr instead (RCpc acquire /
/// release is exactly TSO; vector data goes through x16). Measured on M-series:
/// no faster than the barriers (bench.c float part slower), kept for comparison.
pub fn tso_on() -> bool {
    tso_mode() != 0
}
/// DXCPU_TSO_EBP=0: [ebp - x] accesses (and [ebp + x] after `mov ebp, esp`)
/// keep their barriers too.
fn tso_ebp_local() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("DXCPU_TSO_EBP").map(|v| v != "0").unwrap_or(true))
}
/// 0 = off, 1 = ldapr/stlr (DXCPU_TSO=rcpc), 2 = dmb barriers
pub fn tso_mode() -> u8 {
    static MODE: std::sync::OnceLock<u8> = std::sync::OnceLock::new();
    *MODE.get_or_init(|| {
        let mode = match std::env::var("DXCPU_TSO").as_deref() {
            Ok("0") => 0,
            Ok("rcpc") => 1,
            _ => 2,
        };
        mem::TSO.store(mode != 0, std::sync::atomic::Ordering::Relaxed);
        mode
    })
}

// DXCPU_JIT_OFF=m:Add+Sub+Shl interprets just those mnemonics.
fn jit_off_mnemonic(m: Mnemonic) -> bool {
    static SET: std::sync::OnceLock<Vec<String>> = std::sync::OnceLock::new();
    let set = SET.get_or_init(|| {
        let v = std::env::var("DXCPU_JIT_OFF").unwrap_or_default();
        v.split(',')
            .filter_map(|w| w.trim().strip_prefix("m:"))
            .flat_map(|l| l.split('+').map(|x| x.to_string()).collect::<Vec<_>>())
            .collect()
    });
    !set.is_empty() && set.iter().any(|n| *n == format!("{:?}", m))
}
fn jit_off() -> u32 {
    static OFF: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *OFF.get_or_init(|| {
        let v = std::env::var("DXCPU_JIT_OFF").unwrap_or_default();
        v.split(',')
            .map(|w| match w.trim() {
                "sse" => OFF_SSE,
                "fast" => OFF_FAST,
                "int" => OFF_INT,
                "all" => OFF_ALL,
                "x87" => OFF_X87,
                _ => 0,
            })
            .fold(0, |a, b| a | b)
    })
}

/// DXCPU_STATS_ADDR=<Mnemonic>: also count the guest addresses where that
/// mnemonic is interpreted (printed with the stats).
pub static ADDR_COUNTS: std::sync::Mutex<Option<HashMap<u32, u64>>> = std::sync::Mutex::new(None);
fn stats_addr_mnemonic() -> Option<&'static str> {
    static M: std::sync::OnceLock<Option<String>> = std::sync::OnceLock::new();
    M.get_or_init(|| std::env::var("DXCPU_STATS_ADDR").ok()).as_deref()
}

extern "C" fn helper_interp(ctx: *mut JitCtx, insn: *const Instruction) -> u32 {
    let ctx = unsafe { &mut *ctx };
    let insn = unsafe { &*insn };
    if STATS_ON.load(std::sync::atomic::Ordering::Relaxed) {
        FALLBACK_COUNT[(insn.mnemonic() as usize) & 2047].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if let Some(want) = stats_addr_mnemonic() {
            if format!("{:?}", insn.mnemonic()) == want {
                let mut g = ADDR_COUNTS.lock().unwrap();
                *g.get_or_insert_with(HashMap::new).entry(insn.ip32()).or_insert(0) += 1;
            }
        }
    }
    ctx.materialize();
    let st = unsafe { &mut *ctx.state };
    st.gpr = ctx.gpr;
    st.eflags = ctx.eflags;
    st.eip = insn.ip32();
    let eng = unsafe { &mut *ctx.engine };
    let r = interp::exec(eng, st, insn);
    ctx.gpr = st.gpr;
    ctx.eflags = st.eflags;
    match r {
        Ok(Step::Next) => {
            ctx.eip = insn.next_ip32();
            0
        }
        Ok(Step::Jump) => {
            ctx.eip = st.eip;
            1
        }
        Err(e) => {
            ctx.eip = insn.ip32();
            ctx.exit_reason = e as u32;
            st.fault_eip = insn.ip32();
            2
        }
    }
}

/// Exit codes returned by translated code.
pub const EXIT_DISPATCH: u32 = 0; // ctx.eip holds the next guest eip
pub const EXIT_FAULT: u32 = 2; // ctx.exit_reason holds the Exit

pub struct Jit {
    buf: CodeBuf,
    blocks: HashMap<u32, *const u8>,
    enter: extern "C" fn(*mut JitCtx, *const u8) -> u32,
    exit_addr: usize,
    unlinked: HashMap<u32, Vec<*mut u32>>,
    keep: Vec<Box<Instruction>>,
    pub stub_lo: u32,
    pub stub_hi: u32,
    pub translated: u64,
    /// Translate SSE floating point natively (only valid while MXCSR has the
    /// default control bits 0x1f80; see JitEngine::run).
    fp_ok: bool,
    /// Indirect branch target cache (filled by run_once, read by translated code).
    ibtc: Box<[IbEntry]>,
    /// Body length (words) of the last translated block (see translate_words).
    last_body: usize,
}

const BUF_SIZE: usize = 64 << 20;

impl Jit {
    pub fn new(stub_lo: u32, stub_hi: u32) -> Self {
        Self::with_mode(stub_lo, stub_hi, true)
    }

    pub fn with_mode(stub_lo: u32, stub_hi: u32, fp_ok: bool) -> Self {
        let mut buf = CodeBuf::new(BUF_SIZE);
        let mut a = Asm::new();
        // enter(ctx, code)
        a.stp_x_pre(29, 30, SP, -16);
        a.mov_sp(29, SP);
        a.stp_x_pre(19, 20, SP, -16);
        a.stp_x_pre(21, 22, SP, -16);
        a.stp_x_pre(23, 24, SP, -16);
        a.stp_x_pre(25, 26, SP, -16);
        a.stp_x_pre(27, 28, SP, -16);
        a.mov_x(CTX, 0);
        a.ldr_imm(3, BASE, CTX, OFF_BASE);
        for i in 0..4u32 {
            a.ldp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
        }
        a.br(1);
        // exit: guest registers -> ctx, restore host registers, return w0
        let exit_pos = a.pos();
        for i in 0..4u32 {
            a.stp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
        }
        a.ldp_x_post(27, 28, SP, 16);
        a.ldp_x_post(25, 26, SP, 16);
        a.ldp_x_post(23, 24, SP, 16);
        a.ldp_x_post(21, 22, SP, 16);
        a.ldp_x_post(19, 20, SP, 16);
        a.ldp_x_post(29, 30, SP, 16);
        a.ret();
        let p = buf.add(&a.code).expect("jit buffer");
        Jit {
            buf,
            blocks: HashMap::new(),
            enter: unsafe { std::mem::transmute::<*const u8, extern "C" fn(*mut JitCtx, *const u8) -> u32>(p) },
            exit_addr: p as usize + exit_pos * 4,
            unlinked: HashMap::new(),
            keep: Vec::new(),
            stub_lo,
            stub_hi,
            translated: 0,
            fp_ok,
            ibtc: vec![IbEntry { eip: u32::MAX, pad: 0, code: 0 }; IBTC_SIZE].into_boxed_slice(),
            last_body: 0,
        }
    }

    pub(crate) fn decode(&self, eip: u32) -> Vec<Instruction> {
        let mut out = Vec::new();
        let mut ip = eip;
        while out.len() < 96 {
            let insn = decode_at(ip);
            out.push(insn);
            ip = insn.next_ip32();
            if insn.is_invalid() || insn.flow_control() != FlowControl::Next {
                break;
            }
            if ip >= self.stub_lo && ip < self.stub_hi {
                break;
            }
        }
        out
    }

    /// Run translated code from ctx.eip until a dispatcher exit.
    pub fn run_once(&mut self, ctx: &mut JitCtx) -> u32 {
        // The indirect branch cache first: returns from thunks re-enter here at
        // a high rate and mostly hit it (HashMap lookups hash with SipHash).
        let e = self.ibtc[ibtc_hash(ctx.eip)];
        let code = if e.eip == ctx.eip {
            e.code as *const u8
        } else {
            let code = match self.blocks.get(&ctx.eip) {
                Some(&c) => c,
                None => self.translate(ctx.eip),
            };
            if !verify_on() {
                self.ibtc[ibtc_hash(ctx.eip)] = IbEntry { eip: ctx.eip, pad: 0, code: code as usize };
            }
            code
        };
        ctx.ibtc = self.ibtc.as_mut_ptr();
        (self.enter)(ctx as *mut JitCtx, code)
    }

    /// Translate the block at `eip` without running it and return its code
    /// words and the length of the block body without the cold paths and
    /// literals (offline analysis: instruction counts of given guest code).
    pub fn translate_words(&mut self, eip: u32) -> (Vec<u32>, usize) {
        let p = self.translate(eip);
        let n = (self.buf.next_addr() - p as usize) / 4;
        (unsafe { std::slice::from_raw_parts(p as *const u32, n) }.to_vec(), self.last_body)
    }

    fn translate(&mut self, eip: u32) -> *const u8 {
        if self.buf.remaining() < 1 << 20 {
            // Out of code space: start over (all chains are dropped with the blocks).
            let (lo, hi, fp) = (self.stub_lo, self.stub_hi, self.fp_ok);
            *self = Jit::with_mode(lo, hi, fp);
        }
        let insns = self.decode(eip);
        if let Some(last) = insns.last() {
            mark_code(eip, last.next_ip32());
        }
        let start = self.buf.next_addr();
        let mut t = Translator::new(start, self.exit_addr, self.fp_ok);
        let self_loop = insns.last().is_some_and(|l| {
            matches!(l.flow_control(), FlowControl::ConditionalBranch | FlowControl::UnconditionalBranch)
                && l.op0_kind() == OpKind::NearBranch32
                && l.near_branch32() == eip
        });
        let mut loop_regs = 0u8;
        if self_loop {
            for insn in &insns {
                for i in 0..insn.op_count() {
                    if insn.op_kind(i) == OpKind::Register && insn.op_register(i).is_xmm() {
                        loop_regs |= 1 << xidx(insn.op_register(i));
                    }
                }
            }
        }
        let self_loop = self_loop && !verify_on();
        if !self_loop {
            loop_regs = 0;
        }
        let loop_cf = self_loop && insns.iter().any(|i| matches!(i.mnemonic(), Mnemonic::Inc | Mnemonic::Dec));
        t.begin(eip, loop_regs, loop_cf);
        for (i, insn) in insns.iter().enumerate() {
            let live = flags_live_after(&insns, i);
            t.ld_dep_next = t.tso && load_feeds_next_load(insn, insns.get(i + 1));
            let cont = t.insn(insn, live);
            t.ld_dep_next = false;
            if !cont {
                break;
            }
            if i + 1 == insns.len() {
                t.seg_end();
                t.exit_direct_flags(insn.next_ip32());
            }
        }
        self.last_body = t.a.pos();
        t.finish();
        let code = std::mem::take(&mut t.a.code);
        if let Some(path) = std::env::var_os("DXCPU_JITDUMP") {
            // Debugging aid: append the block as assembler source (.long words).
            use std::io::Write;
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
                let _ = writeln!(f, "_b{:08x}: // {} guest insns", eip, insns.len());
                for w in &code {
                    let _ = writeln!(f, "  .long {:#010x}", w);
                }
            }
        }
        let p = self.buf.add(&code).expect("jit buffer");
        debug_assert_eq!(p as usize, start);
        perfmap_add(p as usize, code.len() * 4, eip);
        self.keep.extend(t.keep.drain(..));
        for (pos, target) in t.exits {
            let at = unsafe { (p as *mut u32).add(pos) };
            if target >= self.stub_lo && target < self.stub_hi {
                continue; // stubs always go through the dispatcher
            }
            if verify_on() {
                continue; // every block returns to the dispatcher
            }
            if let Some(&tp) = self.blocks.get(&target) {
                self.link(at, tp);
            } else {
                self.unlinked.entry(target).or_default().push(at);
            }
        }
        self.blocks.insert(eip, p);
        self.translated += 1;
        if let Some(waiters) = self.unlinked.remove(&eip).filter(|_| !verify_on()) {
            for at in waiters {
                self.link(at, p);
            }
        }
        p
    }

    fn link(&mut self, at: *mut u32, target: *const u8) {
        let off = (target as isize - at as isize) / 4;
        self.buf.patch(at, 0x1400_0000 | ((off as u32) & 0x03ff_ffff));
    }
}

fn is_shift(m: Mnemonic) -> bool {
    use Mnemonic::*;
    matches!(m, Shl | Sal | Shr | Sar | Rol | Ror | Rcl | Rcr | Shld | Shrd)
}

/// Decode one instruction without reading past the end of a page unless the
/// instruction really continues there.
fn decode_at(ip: u32) -> Instruction {
    let room = (0x4000 - (ip % 0x4000)).min(16) as usize;
    let mut len = room;
    loop {
        let bytes = unsafe { std::slice::from_raw_parts(mem::ptr(ip), len) };
        let mut dec = Decoder::with_ip(32, bytes, ip as u64, DecoderOptions::NONE);
        let insn = dec.decode();
        if !insn.is_invalid() || len >= 16 {
            return insn;
        }
        len = 16;
    }
}

/// True if the code at `eip` overwrites all arithmetic flags before reading
/// any of them (looking at a few straight-line instructions).
fn flags_dead_at(eip: u32) -> bool {
    flags_dead_from(eip, 0x3f)
}

/// Are all `pending` flags overwritten at `eip` before any of them is read
/// (straight-line code only, at most 16 instructions)?
fn flags_dead_from(eip: u32, mut pending: u32) -> bool {
    let mut ip = eip;
    for _ in 0..16 {
        // Speculative: the target may be unmapped (unused import, data); treat as live.
        if !mem::readable(ip, 16) {
            return false;
        }
        let insn = decode_at(ip);
        if insn.is_invalid() || insn.rflags_read() & pending != 0 {
            return false;
        }
        let mut written = insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set() | insn.rflags_undefined();
        if is_shift(insn.mnemonic()) {
            written = 0;
        }
        pending &= !written;
        if pending == 0 {
            return true;
        }
        if insn.flow_control() != FlowControl::Next {
            return false;
        }
        ip = insn.next_ip32();
    }
    false
}

/// Are the arithmetic flags produced by insns[i] possibly read later?
fn flags_live_after(insns: &[Instruction], i: usize) -> bool {
    let mut pending: u32 = 0x3f; // OF SF ZF AF PF CF (iced RflagsBits low bits)
    for insn in &insns[i + 1..] {
        if insn.rflags_read() & pending != 0 {
            return true;
        }
        let mut written = insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set() | insn.rflags_undefined();
        // Shifts/rotates by a count that may be zero leave all flags unchanged.
        if is_shift(insn.mnemonic()) {
            let count_op = insn.op_count() - 1;
            let zero_possible = insn.op_kind(count_op) == OpKind::Register
                || (insn.op_kind(count_op) == OpKind::Immediate8 && insn.immediate8() & 0x1f == 0);
            if zero_possible {
                written = 0;
            }
        }
        pending &= !written;
        if pending == 0 {
            return false;
        }
        if insn.flow_control() != FlowControl::Next {
            // Direct jump or call: look at the target (the callee or jump target
            // usually sets the flags before reading any). Anything else: live.
            let direct = matches!(insn.flow_control(), FlowControl::UnconditionalBranch | FlowControl::Call)
                && insn.op0_kind() == OpKind::NearBranch32;
            return !(direct && flags_dead_from(insn.near_branch32(), pending));
        }
    }
    // The block was cut (size limit, stub boundary): continue at the next instruction.
    match insns.last() {
        Some(last) => !flags_dead_from(last.next_ip32(), pending),
        None => true,
    }
}

/// TSO: may the barrier after the load of `a` be left out because `b`, the
/// next instruction, loads from an address computed from the register `a`
/// loaded? An address dependency orders two loads on ARM (no barrier needed
/// between them), and the barrier after b's load then orders both before
/// everything later; nothing accesses memory in between. Only plain moves
/// whose translation is exactly "load, barrier" qualify (no prefixes, which
/// could send them to the interpreter).
fn load_feeds_next_load(a: &Instruction, b: Option<&Instruction>) -> bool {
    use Mnemonic::*;
    let Some(b) = b else { return false };
    let plain = |i: &Instruction| {
        i.op_count() == 2
            && i.op0_kind() == OpKind::Register
            && i.op1_kind() == OpKind::Memory
            && i.segment_prefix() == Register::None
            && !i.has_lock_prefix()
            && !i.has_rep_prefix()
            && !i.has_repne_prefix()
            && !jit_off_mnemonic(i.mnemonic())
    };
    if jit_off() != 0 || !plain(a) || !plain(b) || !matches!(a.mnemonic(), Mov | Movzx | Movsx) {
        return false;
    }
    let r = a.op0_register();
    if r.size() != 4 || !is_gpr(r) || r == Register::ESP {
        return false;
    }
    let b_ok = match b.mnemonic() {
        Mov | Movzx | Movsx => is_gpr(b.op0_register()),
        Movaps | Movups | Movapd | Movupd | Movdqa | Movdqu | Movss | Movsd => b.op0_register().is_xmm(),
        _ => false,
    };
    let (base, index) = (b.memory_base(), b.memory_index());
    // b's load must get its own barrier: not a stack access (see Translator::ea)
    let b_stack = matches!(base, Register::ESP | Register::EBP);
    b_ok && !b_stack && (base == r || index == r)
}

// --- per-block translator ----------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
struct FlagSrc {
    kind: u32,
    size: u32,
}

/// Out-of-line slow path, emitted after the block body (see `finish`).
#[derive(Clone)]
enum ColdKind {
    /// Re-run a segment of instructions (boxed, pointers) with the interpreter.
    Interp(Vec<u64>),
    /// Evaluate the x86 condition (Mnemonic of a jcc) with helper_cond; result -> NZCV (Ne = true).
    Cond(u32),
    /// x86_nan_fixup(r, a, b) of a single-precision op whose result has a NaN lane.
    NanFix(u32, Option<u32>, u32),
}

#[derive(Clone)]
struct Cold {
    fix: Fixup,
    kind: ColdKind,
    /// Cached xmm registers to write back before the helper call.
    dirty: u8,
    /// xmm registers to write back from their segment save slot (v24 + i).
    saved: u8,
    /// Cached xmm registers to reload before resuming.
    valid: u8,
    resume: usize,
}

/// A run of native SSE instructions that only write xmm registers. Float
/// results are checked for NaN once at the end (ACC accumulates the per-lane
/// "not NaN" masks); on failure the cold path restores the xmm state from
/// before the segment and re-executes it with the interpreter (x86 NaN
/// propagation and default NaN differ from ARM; non-NaN results are identical).
struct Seg {
    insns: Vec<u64>,
    /// xmm registers modified in the segment.
    modified: u8,
    /// Modified registers that were dirty before: their old value is in v24 + i.
    saved: u8,
    /// ACC holds at least one mask.
    checked: bool,
}

struct Translator {
    /// x86 TSO memory ordering (DXCPU_TSO): barriers around guest memory accesses.
    tso: bool,
    /// The last effective address computed by ea() is ESP-based (thread-local stack).
    ea_stack: bool,
    /// The current instruction's load needs no barrier (load_feeds_next_load).
    ld_dep_next: bool,
    /// EBP holds a stack address: `mov ebp, esp` earlier in the block and EBP
    /// not written since (TSO: [ebp + x] is a stack access).
    ebp_frame: bool,
    a: Asm,
    start: usize,
    exit_addr: usize,
    /// (position of patchable branch word, target eip)
    exits: Vec<(usize, u32)>,
    /// Dispatcher stubs of the direct exits (emitted by `finish`).
    exit_stubs: Vec<(Fixup, u32)>,
    keep: Vec<Box<Instruction>>,
    /// Host NZCV currently reflects this x86 flag producer (FA/FB/FR/FC hold its operands).
    nzcv: Option<FlagSrc>,
    /// The lazy descriptor in registers must be stored to ctx at exits.
    dirty: bool,
    /// SSE floating point may be translated (MXCSR control bits are the default).
    fp_ok: bool,
    /// Guest xmm registers cached in v16..v23 (bit i = xmm i), and those modified
    /// since loading (must be written back to CpuState.xmm before helper calls/exits).
    xvalid: u8,
    xdirty: u8,
    /// XB holds &state.xmm. Invariant: xvalid != 0 implies xbase.
    xbase: bool,
    cold: Vec<Cold>,
    seg: Option<Seg>,
    consts: Vec<(Fixup, u128)>,
    /// Register FC holds the current CF (valid while no flag-modifying instruction
    /// or hot-path helper call happened; only consulted when `nzcv` is None).
    fc_cf: bool,
    /// Guest address of the block.
    block_eip: u32,
    /// Self-looping block: (code position after the prologue, xmm registers kept
    /// cached across the back edge).
    loop_head: Option<(usize, u8)>,
}

/// Host register holding the 32-bit guest register that contains `r`
/// (AH/CH/DH/BH live in bits 8..15 of EAX/ECX/EDX/EBX).
fn gr(r: Register) -> u32 {
    let i = interp::gpr_index(r);
    if is_high8(r) { G[i - 4] } else { G[i] }
}

fn is_high8(r: Register) -> bool {
    matches!(r, Register::AH | Register::CH | Register::DH | Register::BH)
}

impl Translator {
    fn new(start: usize, exit_addr: usize, fp_ok: bool) -> Self {
        Translator {
            tso: tso_on(),
            ea_stack: false,
            ld_dep_next: false,
            ebp_frame: false,
            a: Asm::new(),
            start,
            exit_addr,
            exits: Vec::new(),
            exit_stubs: Vec::new(),
            keep: Vec::new(),
            nzcv: None,
            dirty: false,
            fp_ok,
            xvalid: 0,
            xdirty: 0,
            xbase: false,
            cold: Vec::new(),
            seg: None,
            consts: Vec::new(),
            fc_cf: false,
            block_eip: 0,
            loop_head: None,
        }
    }

    /// Start of the block. For a block that branches back to its own start, the
    /// xmm registers it uses (`loop_regs`) are loaded once in a prologue and stay
    /// cached across the back edge (treated as dirty so that exits write them back).
    fn begin(&mut self, eip: u32, loop_regs: u8, self_loop_cf: bool) {
        self.block_eip = eip;
        if loop_regs != 0 {
            for i in 0..8u32 {
                if loop_regs & (1 << i) != 0 {
                    self.xget(i);
                }
            }
            self.xdirty = loop_regs;
        }
        if loop_regs != 0 || self_loop_cf {
            // inc/dec in the loop preserve CF: keep it in FC across the back edge
            self.load_cf_from_ctx();
            self.fc_cf = true;
            self.loop_head = Some((self.a.pos(), loop_regs));
        }
    }

    /// FC = CF from the flags stored in ctx (materialised EFLAGS or lazy descriptor).
    fn load_cf_from_ctx(&mut self) {
        self.a.ldr_imm(2, T0, CTX, OFF_FKIND);
        self.a.ldr_imm(2, T1, CTX, OFF_EFLAGS);
        self.a.ldr_imm(2, FC, CTX, OFF_FCF);
        self.a.cmp_w(T0, XZR);
        self.a.csel_w(FC, T1, FC, Cond::Eq);
        self.a.ubfx_w(FC, FC, 0, 1);
    }

    // --- xmm register cache --------------------------------------------------------------------

    fn ensure_xbase(&mut self) {
        if !self.xbase {
            self.a.ldr_imm(3, XB, CTX, OFF_STATE);
            self.a.add_x_imm(XB, XB, XMM_OFF);
            self.xbase = true;
        }
    }

    /// Host vector register holding guest xmm i (loaded if needed).
    fn xget(&mut self, i: u32) -> u32 {
        if self.xvalid & (1 << i) == 0 {
            self.ensure_xbase();
            self.a.ldr_q_imm(VX + i, XB, 16 * i);
            self.xvalid |= 1 << i;
        }
        VX + i
    }

    /// Like xget, for an instruction that modifies the register.
    fn xmod(&mut self, i: u32) -> u32 {
        let r = self.xget(i);
        self.seg_premod(i);
        self.xdirty |= 1 << i;
        r
    }

    /// Host register for guest xmm i that is about to be completely overwritten.
    fn xdef(&mut self, i: u32) -> u32 {
        self.ensure_xbase();
        self.seg_premod(i);
        self.xvalid |= 1 << i;
        self.xdirty |= 1 << i;
        VX + i
    }

    fn xmm_store(&mut self, mask: u8) {
        for i in 0..8u32 {
            if mask & (1 << i) != 0 {
                self.a.str_q_imm(VX + i, XB, 16 * i);
            }
        }
    }

    /// Write modified xmm registers back to CpuState (state unchanged; for exits).
    fn xmm_writeback(&mut self) {
        let d = self.xdirty;
        self.xmm_store(d);
    }

    /// Write back and forget the cache (before a helper call in the hot path:
    /// v16-v31 and x13 are caller-saved).
    fn xmm_spill(&mut self) {
        self.xmm_writeback();
        self.xvalid = 0;
        self.xdirty = 0;
        self.xbase = false;
    }

    /// Load a 128-bit constant from the block's literal pool.
    fn ldr_const(&mut self, vt: u32, v: u128) {
        let f = self.a.ldr_q_lit_fixup(vt);
        self.consts.push((f, v));
    }

    /// Emit the cold paths and the literal pool after the block body.
    fn finish(&mut self) {
        debug_assert!(self.seg.is_none());
        for (f, target) in std::mem::take(&mut self.exit_stubs) {
            self.a.bind(f);
            self.a.mov_w_imm(T0, target);
            self.a.str_imm(2, T0, CTX, OFF_EIP);
            self.a.mov_w_imm(0, EXIT_DISPATCH);
            self.b_exit();
        }
        let colds = std::mem::take(&mut self.cold);
        for c in colds {
            self.a.bind(c.fix);
            if let ColdKind::NanFix(r, a, b) = c.kind {
                // native, no helper call: registers are as at the branch
                self.x86_nan_fixup(r, a, b);
                self.a.b_to(c.resume);
                continue;
            }
            self.xmm_store(c.dirty);
            for i in 0..8u32 {
                if c.saved & (1 << i) != 0 {
                    self.a.str_q_imm(SAVE + i, XB, 16 * i);
                }
            }
            self.a.stp_x_pre(9, 10, SP, -16);
            self.a.stp_x_pre(11, 12, SP, -16);
            self.a.mrs_nzcv(1);
            self.a.stp_x_pre(1, XB, SP, -16);
            match &c.kind {
                ColdKind::Interp(ptrs) => {
                    for i in 0..4u32 {
                        self.a.stp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
                    }
                    let mut fails = Vec::new();
                    for &ptr in ptrs {
                        self.a.mov_x(0, CTX);
                        self.a.mov_x_imm(1, ptr);
                        self.a.ldr_imm(3, 16, CTX, OFF_HELPER_INTERP);
                        self.a.blr(16);
                        fails.push(self.a.cbnz_w_fixup(0));
                    }
                    for f in fails {
                        self.a.bind(f);
                    }
                    for i in 0..4u32 {
                        self.a.ldp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
                    }
                }
                ColdKind::NanFix(..) => unreachable!(),
                ColdKind::Cond(m) => {
                    self.a.mov_x(0, CTX);
                    self.a.mov_w_imm(1, *m);
                    self.a.ldr_imm(3, 16, CTX, OFF_HELPER_COND);
                    self.a.blr(16);
                }
            }
            self.a.ldp_x_post(1, XB, SP, 16);
            self.a.msr_nzcv(1);
            self.a.ldp_x_post(11, 12, SP, 16);
            self.a.ldp_x_post(9, 10, SP, 16);
            for i in 0..8u32 {
                if c.valid & (1 << i) != 0 {
                    self.a.ldr_q_imm(VX + i, XB, 16 * i);
                }
            }
            match c.kind {
                ColdKind::Interp(_) => {
                    self.a.cbz_w_to(0, c.resume);
                    self.b_exit_keep_w0();
                }
                ColdKind::Cond(_) => {
                    self.a.cmp_w(0, XZR);
                    self.a.b_to(c.resume);
                }
                ColdKind::NanFix(..) => unreachable!(),
            }
        }
        let consts = std::mem::take(&mut self.consts);
        for (f, v) in consts {
            self.a.bind(f);
            for k in 0..4 {
                self.a.emit((v >> (32 * k)) as u32);
            }
        }
    }

    // --- segments (deferred NaN checks, see Seg) --------------------------------------------------

    /// Before guest xmm i is modified inside a segment: keep its pre-segment value.
    fn seg_premod(&mut self, i: u32) {
        let dirty = self.xdirty & (1 << i) != 0;
        if let Some(seg) = &mut self.seg {
            if seg.modified & (1 << i) == 0 {
                seg.modified |= 1 << i;
                if dirty {
                    // clean registers still have their old value in CpuState
                    seg.saved |= 1 << i;
                    self.a.vmov(SAVE + i, VX + i);
                }
            }
        }
    }

    /// Record the NaN mask of a float result (lanes of `r` that the instruction
    /// wrote; other lanes must not be NaN, e.g. zeroed by a scalar op).
    fn seg_check(&mut self, dbl: bool, r: u32) {
        let seg = self.seg.as_mut().expect("float op outside a segment");
        if seg.checked {
            self.a.fcmeq_v(dbl, V2, r, r);
            self.a.vand(ACC, ACC, V2);
        } else {
            self.a.fcmeq_v(dbl, ACC, r, r);
            seg.checked = true;
        }
    }

    /// Turn ARM NaN results of a single-precision op into the x86 ones (FPCR.DN=0):
    /// a NaN first operand is returned quieted (ARM may prefer an SNaN second
    /// operand), and invalid operations on non-NaN inputs give the x86 default
    /// NaN 0xffc00000 instead of 0x7fc00000. `a` = first operand (None for sqrt),
    /// `b` = second/only source; neither may alias `r`. Uses V2, V5, V6.
    /// x86_nan_fixup on a cold path, taken only if some lane of `r` is NaN (the
    /// fixup only changes NaN lanes). Keeps the fixup's dependency chain and
    /// instructions off the hot path; `r`, `a` and `b` must stay unchanged until
    /// the next instruction. Uses V2 and T1.
    fn x86_nan_check(&mut self, r: u32, a: Option<u32>, b: u32) {
        self.a.fcmeq_v(false, V2, r, r);
        self.a.uminv_4s(V2, V2);
        self.a.fmov_w_s(T1, V2);
        let fix = self.a.cbz_w_fixup(T1);
        let c = Cold { fix, kind: ColdKind::NanFix(r, a, b), dirty: 0, saved: 0, valid: 0, resume: self.a.pos() };
        self.cold.push(c);
    }

    fn x86_nan_fixup(&mut self, r: u32, a: Option<u32>, b: u32) {
        debug_assert!(a != Some(r) && b != r);
        self.a.fcmeq_v(false, V6, b, b); // b not NaN
        if let Some(a) = a {
            self.a.fcmeq_v(false, V5, a, a); // a not NaN
            self.a.vand(V6, V6, V5);
        }
        self.a.fcmeq_v(false, V2, r, r);
        self.a.vbic(V6, V6, V2); // inputs not NaN, result NaN: invalid operation
        self.a.movi_4s(V2, 0x80, 24);
        self.a.vand(V6, V6, V2);
        self.a.vorr(r, r, V6); // default NaN sign
        if let Some(a) = a {
            self.a.movi_4s(V2, 0x40, 16);
            self.a.vorr(V2, a, V2); // quiet(a)
            self.a.vbif(r, V2, V5); // where a is NaN
        }
    }

    /// Integer load of guest memory [BASE + wm] into rd (TSO aware).
    fn gld(&mut self, size: u32, rd: u32, wm: u32) {
        if self.tso && !self.ea_stack && tso_mode() == 1 {
            self.a.add_x_uxtw(TA, BASE, wm);
            self.a.ldapr(size, rd, TA);
        } else {
            self.a.ldr_uxtw(size, rd, BASE, wm);
            self.tso_after_load();
        }
    }
    /// Integer store of rs to guest memory [BASE + wm] (TSO aware).
    fn gst(&mut self, size: u32, rs: u32, wm: u32) {
        if self.tso && !self.ea_stack && tso_mode() == 1 {
            self.a.add_x_uxtw(TA, BASE, wm);
            self.a.stlr(size, rs, TA);
        } else {
            self.tso_before_store();
            self.a.str_uxtw(size, rs, BASE, wm);
        }
    }

    /// SIMD load of guest memory [BASE + wm] (size 2 = S, 3 = D, 4 = Q), TSO
    /// aware: ldapr has no vector form, so the data goes through TD.
    fn vld(&mut self, size: u32, vt: u32, wm: u32) {
        if !(self.tso && !self.ea_stack && tso_mode() == 1) {
            self.a.ldr_v_uxtw(size, vt, BASE, wm);
            self.tso_after_load();
            return;
        }
        self.a.add_x_uxtw(TA, BASE, wm);
        if size == 2 {
            self.a.ldapr(2, TD, TA);
            self.a.fmov_s_w(vt, TD);
            return;
        }
        self.a.ldapr(3, TD, TA);
        self.a.fmov_d_x(vt, TD);
        if size == 4 {
            self.a.add_x_imm(TA, TA, 8);
            self.a.ldapr(3, TD, TA);
            self.a.ins_gen(3, vt, 1, TD);
        }
    }
    /// SIMD store to guest memory [BASE + wm], TSO aware (see vld).
    fn vst(&mut self, size: u32, vs: u32, wm: u32) {
        if !(self.tso && !self.ea_stack && tso_mode() == 1) {
            self.tso_before_store();
            self.a.str_v_uxtw(size, vs, BASE, wm);
            return;
        }
        self.a.add_x_uxtw(TA, BASE, wm);
        if size == 2 {
            self.a.fmov_w_s(TD, vs);
            self.a.stlr(2, TD, TA);
            return;
        }
        self.a.fmov_x_d(TD, vs);
        self.a.stlr(3, TD, TA);
        if size == 4 {
            self.a.umov_x_d(TD, vs, 1);
            self.a.add_x_imm(TA, TA, 8);
            self.a.stlr(3, TD, TA);
        }
    }

    fn tso_after_load(&mut self) {
        if self.tso && !self.ea_stack && !self.ld_dep_next {
            self.a.dmb_ishld();
        }
    }
    fn tso_before_store(&mut self) {
        if self.tso && !self.ea_stack {
            self.a.dmb_ishst();
        }
    }

    // --- x87 ------------------------------------------------------------------------------------
    //
    // The common x87 data movement natively, with exactly the interpreter's
    // semantics (fpu.rs): values are f64, TOP/empty/sw live in CpuState.x87;
    // stack over/underflow and SNaN loads/stores set IE (and SF). Anything
    // unusual (rounding control other than nearest for fst m32, NaN or out of
    // range integer stores) runs the instruction in the interpreter via a cold
    // path, before any state was changed except idempotent status bits.

    fn x87_sw_or(&mut self, bits: u32) {
        self.a.ldr_imm(1, T5, XB, X87_SW);
        self.a.mov_w_imm(T3, bits);
        self.a.orr_w(T5, T5, T3);
        self.a.str_imm(1, T5, XB, X87_SW);
    }

    /// dst = byte offset of physical register `p` (relative to XB).
    fn x87_slot(&mut self, dst: u32, p: u32) {
        self.a.lsl_w_imm(dst, p, 3);
        self.a.add_w_imm(dst, dst, X87_OFF);
    }

    /// V `vd` = ST(i), flagging underflow like fpu::get. Needs T1 = TOP; uses T2, T3, T5.
    fn x87_get(&mut self, vd: u32, i: u32) {
        self.a.add_w_imm(T2, T1, i);
        self.a.ubfx_w(T2, T2, 0, 3);
        self.a.ldr_imm(2, T3, XB, X87_EMPTY);
        self.a.lsrv_w(T3, T3, T2);
        self.a.ubfx_w(T3, T3, 0, 1);
        let ok = self.a.cbz_w_fixup(T3);
        self.x87_sw_or(SW_IE | SW_SF);
        self.a.bind(ok);
        self.x87_slot(T2, T2);
        self.a.ldr_v_uxtw(3, vd, XB, T2);
    }

    /// ST(i) = V `vs` (no tag change, like fpu::set). Needs T1 = TOP; uses T2.
    fn x87_set(&mut self, vs: u32, i: u32) {
        self.a.add_w_imm(T2, T1, i);
        self.a.ubfx_w(T2, T2, 0, 3);
        self.x87_slot(T2, T2);
        self.a.str_v_uxtw(3, vs, XB, T2);
    }

    /// Push V `vs` like fpu::push. Needs T1 = TOP, leaves the new TOP in T1 (and stored).
    fn x87_push(&mut self, vs: u32) {
        self.a.sub_w_imm(T1, T1, 1);
        self.a.ubfx_w(T1, T1, 0, 3);
        self.a.ldr_imm(2, T3, XB, X87_EMPTY);
        self.a.lsrv_w(T3, T3, T1);
        self.a.ubfx_w(T3, T3, 0, 1);
        let ok = self.a.cbnz_w_fixup(T3); // target slot empty: fine
        self.x87_sw_or(SW_IE | SW_SF);
        self.a.bind(ok);
        self.x87_slot(T2, T1);
        self.a.str_v_uxtw(3, vs, XB, T2);
        self.a.mov_w_imm(T3, 1);
        self.a.lslv_w(T3, T3, T1);
        self.a.ldr_imm(2, T5, XB, X87_EMPTY);
        self.a.bic_w(T5, T5, T3);
        self.a.str_imm(2, T5, XB, X87_EMPTY);
        self.a.str_imm(2, T1, XB, X87_TOP);
    }

    /// Pop like fpu::pop. Needs T1 = TOP.
    fn x87_pop(&mut self) {
        self.a.mov_w_imm(T3, 1);
        self.a.lslv_w(T3, T3, T1);
        self.a.ldr_imm(2, T5, XB, X87_EMPTY);
        self.a.orr_w(T5, T5, T3);
        self.a.str_imm(2, T5, XB, X87_EMPTY);
        self.a.add_w_imm(T1, T1, 1);
        self.a.ubfx_w(T1, T1, 0, 3);
        self.a.str_imm(2, T1, XB, X87_TOP);
    }

    /// f64 bits in T0 (x): if a NaN, set IE for a signalling one and quiet it
    /// (fpu::f64_load / fst m64). Result back in V `vd`. Uses T2, T3, T5.
    fn x87_quiet_f64(&mut self, vd: u32) {
        self.a.fcmp_s(true, vd, vd);
        let not_nan = self.a.bcond_fixup(Cond::Vc);
        self.a.lsr_x_imm(T2, T0, 51);
        self.a.ubfx_w(T2, T2, 0, 1);
        let quiet = self.a.cbnz_w_fixup(T2);
        self.x87_sw_or(SW_IE);
        self.a.mov_x_imm(T3, 1 << 51);
        self.a.orr_x_lsl(T0, T0, T3, 0);
        self.a.fmov_d_x(vd, T0);
        self.a.bind(quiet);
        self.a.bind(not_nan);
    }

    /// Run `insn` in the interpreter from a cold path when `fix` is taken.
    fn x87_cold(&mut self, insn: &Instruction, fix: Fixup, resume: usize) {
        let boxed = Box::new(*insn);
        let ptr = &*boxed as *const Instruction as u64;
        self.keep.push(boxed);
        let c = Cold {
            fix,
            kind: ColdKind::Interp(vec![ptr]),
            dirty: self.xdirty,
            saved: 0,
            valid: self.xvalid,
            resume,
        };
        self.cold.push(c);
    }

    fn x87(&mut self, insn: &Instruction) -> Option<bool> {
        use Mnemonic::*;
        if jit_off() & OFF_X87 != 0 || !self.fp_ok {
            return None;
        }
        let m = insn.mnemonic();
        if !matches!(m, Fld | Fst | Fstp | Fxch | Fild | Fist | Fistp | Fisttp | Fldz | Fld1 | Emms) {
            return None;
        }
        if insn.has_lock_prefix() || matches!(insn.segment_prefix(), Register::FS | Register::GS) {
            return None;
        }
        let mem = (0..insn.op_count()).any(|i| insn.op_kind(i) == OpKind::Memory);
        let msize = if mem { insn.memory_size() } else { MemorySize::Unknown };
        let sti = |r: Register| (r as u32).wrapping_sub(Register::ST0 as u32);
        // Supported operand forms only.
        match m {
            Fld | Fst | Fstp if mem && !matches!(msize, MemorySize::Float32 | MemorySize::Float64) => return None,
            Fild if !matches!(msize, MemorySize::Int16 | MemorySize::Int32 | MemorySize::Int64) => return None,
            Fist | Fistp | Fisttp if !matches!(msize, MemorySize::Int16 | MemorySize::Int32) => return None,
            _ => {}
        }
        if mem {
            self.ea(insn);
        }
        self.ensure_xbase();
        if m == Emms {
            self.a.mov_w_imm(T0, 0xff);
            self.a.str_imm(2, T0, XB, X87_EMPTY);
            return Some(true);
        }
        self.a.ldr_imm(2, T1, XB, X87_TOP);
        let mut cold: Vec<Fixup> = Vec::new();
        match m {
            Fld if mem && msize == MemorySize::Float32 => {
                self.gld(2, T0, T4);
                // SNaN (quiet bit clear) sets IE; fcvt quiets and keeps sign and payload.
                self.a.lsl_w_imm(T2, T0, 1);
                self.a.mov_w_imm(T3, 0xff00_0000);
                self.a.cmp_w(T2, T3);
                let not_nan = self.a.bcond_fixup(Cond::Ls);
                self.a.ubfx_w(T2, T0, 22, 1);
                let quiet = self.a.cbnz_w_fixup(T2);
                self.x87_sw_or(SW_IE);
                self.a.bind(quiet);
                self.a.bind(not_nan);
                self.a.fmov_s_w(V0, T0);
                self.a.fcvt_d_s(V0, V0);
                self.x87_push(V0);
            }
            Fld if mem => {
                self.gld(3, T0, T4);
                self.a.fmov_d_x(V0, T0);
                self.x87_quiet_f64(V0);
                self.x87_push(V0);
            }
            Fld => {
                self.x87_get(V0, sti(insn.op0_register()));
                self.x87_push(V0);
            }
            Fldz | Fld1 => {
                self.a.mov_x_imm(T0, if m == Fldz { 0 } else { 1.0f64.to_bits() });
                self.a.fmov_d_x(V0, T0);
                self.x87_push(V0);
            }
            Fild => {
                match msize {
                    MemorySize::Int16 => {
                        self.a.ldrs_w_uxtw(1, T0, BASE, T4);
                        self.tso_after_load();
                        self.a.scvtf_s_w(true, V0, T0);
                    }
                    MemorySize::Int32 => {
                        self.gld(2, T0, T4);
                        self.a.scvtf_s_w(true, V0, T0);
                    }
                    _ => {
                        self.gld(3, T0, T4);
                        self.a.scvtf_d_x(V0, T0);
                    }
                }
                self.x87_push(V0);
            }
            Fst | Fstp if mem && msize == MemorySize::Float32 => {
                self.x87_get(V0, 0);
                // rounding control other than nearest: interpreter
                self.a.ldr_imm(1, T0, XB, X87_CW);
                self.a.ubfx_w(T0, T0, 10, 2);
                cold.push(self.a.cbnz_w_fixup(T0));
                self.a.fcmp_s(true, V0, V0);
                let not_nan = self.a.bcond_fixup(Cond::Vc);
                self.a.fmov_x_d(T0, V0);
                self.a.lsr_x_imm(T2, T0, 51);
                self.a.ubfx_w(T2, T2, 0, 1);
                let quiet = self.a.cbnz_w_fixup(T2);
                self.x87_sw_or(SW_IE);
                self.a.bind(quiet);
                self.a.bind(not_nan);
                self.a.fcvt_s_d(V1, V0);
                self.vst(2, V1, T4);
                if m == Fstp {
                    self.x87_pop();
                }
            }
            Fst | Fstp if mem => {
                self.x87_get(V0, 0);
                self.a.fmov_x_d(T0, V0);
                self.x87_quiet_f64(V0);
                self.vst(3, V0, T4);
                if m == Fstp {
                    self.x87_pop();
                }
            }
            Fst | Fstp => {
                self.x87_get(V0, 0);
                self.x87_set(V0, sti(insn.op0_register()));
                if m == Fstp {
                    self.x87_pop();
                }
            }
            Fxch => {
                let i = if insn.op_count() == 0 { 1 } else { sti(insn.op_register(insn.op_count() - 1)) };
                self.x87_get(V0, 0);
                self.x87_get(V1, i);
                self.x87_set(V1, 0);
                self.x87_set(V0, i);
            }
            Fist | Fistp | Fisttp => {
                self.x87_get(V0, 0);
                // NaN: interpreter (integer indefinite + IE)
                self.a.fcmp_s(true, V0, V0);
                cold.push(self.a.bcond_fixup(Cond::Vs));
                if m == Fisttp {
                    self.a.fcvtzs_x(true, T0, V0);
                } else {
                    // nearest (fcvtns, ties to even) or chop; up/down: interpreter
                    self.a.ldr_imm(1, T2, XB, X87_CW);
                    self.a.ubfx_w(T2, T2, 10, 2);
                    let near = self.a.cbz_w_fixup(T2);
                    self.a.subs_w_imm(XZR, T2, 3);
                    cold.push(self.a.bcond_fixup(Cond::Ne));
                    self.a.fcvtzs_x(true, T0, V0);
                    let done = self.a.b_fixup();
                    self.a.bind(near);
                    self.a.fcvtns_x(true, T0, V0);
                    self.a.bind(done);
                }
                // out of range: interpreter (integer indefinite + IE)
                self.a.cmp_x_sxtw(T0, T0);
                cold.push(self.a.bcond_fixup(Cond::Ne));
                if msize == MemorySize::Int16 {
                    self.a.sxth_w(T3, T0);
                    self.a.cmp_w(T3, T0);
                    cold.push(self.a.bcond_fixup(Cond::Ne));
                    self.gst(1, T0, T4);
                } else {
                    self.gst(2, T0, T4);
                }
                if m != Fist {
                    self.x87_pop();
                }
            }
            _ => unreachable!(),
        }
        // Each special case re-runs the instruction in the interpreter (cold stub)
        // and resumes after it.
        let resume = self.a.pos();
        for f in cold {
            self.x87_cold(insn, f, resume);
        }
        Some(true)
    }

    /// Close the current segment: one NaN check for all its float results.
    fn seg_end(&mut self) {
        let Some(seg) = self.seg.take() else { return };
        if !seg.checked {
            return;
        }
        self.a.uminv_4s(V2, ACC);
        self.a.fmov_w_s(T1, V2);
        let fix = self.a.cbz_w_fixup(T1);
        let c = Cold {
            fix,
            kind: ColdKind::Interp(seg.insns),
            dirty: self.xdirty & !seg.modified,
            saved: seg.saved,
            valid: self.xvalid,
            resume: self.a.pos(),
        };
        self.cold.push(c);
    }

    /// Instructions that may be part of a segment: SSE with an xmm destination.
    fn seg_candidate(insn: &Instruction) -> bool {
        use Mnemonic::*;
        insn.op_count() >= 1
            && insn.op0_kind() == OpKind::Register
            && insn.op0_register().is_xmm()
            && !matches!(insn.mnemonic(), Comiss | Ucomiss | Comisd | Ucomisd)
            && !insn.has_lock_prefix()
    }

    fn b_exit(&mut self) {
        let here = self.start + self.a.pos() * 4;
        let off = (self.exit_addr as isize - here as isize) / 4;
        self.a.emit(0x1400_0000 | ((off as u32) & 0x03ff_ffff));
    }

    /// 32-bit shift of T0 by the constant c (1..31): result in T2 and FR, CF in FC,
    /// OF in FA (as the interpreter computes it: also for counts other than 1).
    fn shift_imm_flags(&mut self, m: Mnemonic, c: u32) {
        match m {
            Mnemonic::Shl => {
                self.a.lsl_w_imm(T2, T0, c);
                self.a.ubfx_w(FC, T0, 32 - c, 1);
            }
            Mnemonic::Shr => {
                self.a.lsr_w_imm(T2, T0, c);
                self.a.ubfx_w(FC, T0, c - 1, 1);
            }
            _ => {
                self.a.asr_w_imm(T2, T0, c);
                self.a.ubfx_w(FC, T0, c - 1, 1);
            }
        }
        self.shift_of(m);
        self.a.mov_w(FR, T2);
    }

    /// Lazy add flags for a = old value (register), b = addend (register).
    fn set_add_flags(&mut self, flags_live: bool, a: u32, b: u32) {
        if flags_live {
            self.a.mov_w(FA, a);
            self.a.mov_w(FB, b);
            self.a.adds_w(FR, FA, FB);
            self.nzcv = Some(FlagSrc { kind: K_ADD, size: 4 });
            self.dirty = true;
        } else {
            self.nzcv = None;
            self.dirty = false;
        }
    }

    /// x86 CF (0/1) into FC, from wherever the current flags live.
    fn cf_to_fc(&mut self) {
        match self.nzcv {
            Some(f) if matches!(f.kind, K_SUB | K_SBB | K_SHIFT) => self.a.cset_w(FC, Cond::Lo),
            Some(f) if matches!(f.kind, K_ADD | K_ADC) => self.a.cset_w(FC, Cond::Hs),
            Some(f) if f.kind == K_LOGIC => self.a.mov_w_imm(FC, 0),
            Some(f) if f.kind == K_FCMP => self.a.cset_w(FC, Cond::Lt),
            Some(_) => {} // inc/dec: FC holds CF
            None if self.fc_cf => {}
            None => self.load_cf_from_ctx(),
        }
    }

    /// OF of a shift into FA: shl = sign(result) ^ CF, shr = sign(source), sar = 0.
    fn shift_of(&mut self, m: Mnemonic) {
        match m {
            Mnemonic::Shl => {
                self.a.lsr_w_imm(FA, T2, 31);
                self.a.eor_w(FA, FA, FC);
            }
            Mnemonic::Shr => self.a.lsr_w_imm(FA, T0, 31),
            _ => self.a.mov_w_imm(FA, 0),
        }
    }

    /// Host NZCV for a shift result in T2 (K_SHIFT convention) and record the
    /// lazy descriptor (FR, FC, FA set by shift_imm_flags).
    fn shift_nzcv(&mut self) {
        self.a.tst_w(T2, T2); // N, Z; C = V = 0
        self.a.mrs_nzcv(T1);
        self.a.mov_w_imm(T3, 1);
        self.a.eor_w(T3, T3, FC);
        self.a.orr_w_lsl(T1, T1, T3, 29); // C = !CF
        self.a.orr_w_lsl(T1, T1, FA, 28); // V = OF
        self.a.msr_nzcv(T1);
        self.nzcv = Some(FlagSrc { kind: K_SHIFT, size: 4 });
        self.dirty = true;
    }

    /// Store the lazy flag descriptor (if flags were produced in this block).
    fn flush_flags(&mut self) {
        if let (true, Some(f)) = (self.dirty, self.nzcv) {
            if f.kind == K_FCMP {
                // ucomiss & co: ZF = Z|V (equal or unordered), PF = V, CF = N|V (Lt); OF=SF=AF=0.
                self.a.ldr_imm(2, T3, CTX, OFF_EFLAGS);
                self.a.mov_w_imm(T0, ARITH);
                self.a.bic_w(T3, T3, T0);
                self.a.cset_w(T0, Cond::Lt);
                self.a.orr_w(T3, T3, T0);
                self.a.cset_w(T0, Cond::Vs);
                self.a.orr_w_lsl(T3, T3, T0, 2);
                self.a.cset_w(T1, Cond::Eq);
                self.a.orr_w(T0, T0, T1);
                self.a.orr_w_lsl(T3, T3, T0, 6);
                self.a.str_imm(2, T3, CTX, OFF_EFLAGS);
                self.a.str_imm(2, XZR, CTX, OFF_FKIND);
                self.dirty = false;
                return;
            }
            // ctx.fcf always holds CF of a lazy descriptor (inc/dec read it inline).
            match f.kind {
                K_SUB => self.a.cset_w(FC, Cond::Lo),
                K_ADD => self.a.cset_w(FC, Cond::Hs),
                K_LOGIC => self.a.mov_w_imm(FC, 0),
                _ => {}
            }
            self.a.mov_w_imm(T0, f.kind);
            self.a.mov_w_imm(T1, f.size);
            self.a.stp_w(T0, T1, CTX, OFF_FKIND);
            self.a.stp_w(FA, FB, CTX, OFF_FA);
            self.a.stp_w(FR, FC, CTX, OFF_FRES);
            self.dirty = false;
        }
    }

    /// Exit to a known guest eip, storing the lazy flags only if the target
    /// may read them.
    fn exit_direct_flags(&mut self, target: u32) {
        let saved = (self.nzcv, self.dirty);
        if !flags_dead_at(target) {
            self.flush_flags();
        }
        self.exit_direct(target);
        self.nzcv = saved.0;
        self.dirty = saved.1;
    }

    /// Exit to a known guest eip: a patchable branch (initially to the slow
    /// path into the dispatcher, emitted after the block body).
    fn exit_direct(&mut self, target: u32) {
        if let (Some((head, regs)), true) = (self.loop_head, target == self.block_eip) {
            // back edge of a self loop: keep the xmm cache and CF in FC (state as after the prologue)
            let saved = (self.xvalid, self.xdirty, self.xbase);
            for i in 0..8u32 {
                if regs & (1 << i) != 0 {
                    self.xget(i);
                }
            }
            match self.nzcv {
                Some(f) if f.kind == K_SUB => self.a.cset_w(FC, Cond::Lo),
                Some(f) if f.kind == K_ADD => self.a.cset_w(FC, Cond::Hs),
                Some(f) if f.kind == K_LOGIC => self.a.mov_w_imm(FC, 0),
                Some(f) if f.kind == K_FCMP => self.a.cset_w(FC, Cond::Lt),
                Some(_) => {} // inc/dec: FC holds CF
                None if self.fc_cf => {}
                None => self.load_cf_from_ctx(),
            }
            self.a.b_to(head);
            (self.xvalid, self.xdirty, self.xbase) = saved;
            return;
        }
        self.xmm_writeback();
        // A branch to the dispatcher stub at the end of the block, patched to the
        // target block once that is translated (the stubs stay out of the hot code).
        let pos = self.a.pos();
        let f = self.a.b_fixup();
        self.exits.push((pos, target));
        self.exit_stubs.push((f, target));
    }

    /// Exit with the guest eip in register `r`.
    fn exit_indirect(&mut self, r: u32) {
        self.xmm_writeback();
        debug_assert!(r != T1 && r != T2 && r != T3);
        // Inline lookup in the indirect branch target cache; a hit enters the
        // block directly (same entry state as a linked direct exit).
        self.a.eor_w_lsr(T1, r, r, 11);
        self.a.ubfx_w(T1, T1, 0, IBTC_BITS);
        self.a.lsl_w_imm(T1, T1, 4);
        self.a.ldr_imm(3, T2, CTX, OFF_IBTC);
        self.a.add_x(T2, T2, T1);
        self.a.ldr_imm(2, T3, T2, 0);
        self.a.cmp_w(T3, r);
        let miss = self.a.bcond_fixup(Cond::Ne);
        self.a.ldr_imm(3, T3, T2, 8);
        self.a.br(T3);
        self.a.bind(miss);
        self.a.str_imm(2, r, CTX, OFF_EIP);
        self.a.mov_w_imm(0, EXIT_DISPATCH);
        self.b_exit();
    }

    // --- operands ------------------------------------------------------------------------------

    /// Compute the effective address of the memory operand into T4.
    fn ea(&mut self, insn: &Instruction) {
        let base = insn.memory_base();
        let index = insn.memory_index();
        let disp = insn.memory_displacement32();
        // Thread-local for TSO purposes: ESP-based accesses and locals below the
        // frame pointer ([ebp - x]; EBP may be a general register with
        // -fomit-frame-pointer, but negative offsets from an object pointer are
        // rare). Skipping their barriers keeps the ordering of everything else:
        // the dmb after a load orders it before all later accesses, the dmb
        // before a store all earlier stores before it.
        // Also any [ebp + ...] after `mov ebp, esp` in this block: EBP then is the
        // frame pointer, these are arguments and locals like [esp + x].
        self.ea_stack = base == Register::ESP
            || (base == Register::EBP
                && tso_ebp_local()
                && (self.ebp_frame || (index == Register::None && (disp as i32) < 0)));
        let mut have = false;
        if base != Register::None && index == Register::None {
            // [base + disp]: one instruction when the displacement fits an add/sub immediate
            let d = disp as i32;
            if (0..4096).contains(&d) {
                self.a.add_w_imm(T4, gr(base), d as u32);
                return;
            } else if (-4095..0).contains(&d) {
                self.a.sub_w_imm(T4, gr(base), (-d) as u32);
                return;
            }
        }
        if base != Register::None {
            self.a.mov_w(T4, gr(base));
            have = true;
        }
        if index != Register::None {
            let sh = insn.memory_index_scale().trailing_zeros();
            if have {
                self.a.add_w_lsl(T4, T4, gr(index), sh);
            } else {
                self.a.lsl_w_imm(T4, gr(index), sh);
                have = true;
            }
        }
        if !have {
            self.a.mov_w_imm(T4, disp);
        } else if disp != 0 {
            let d = disp as i32;
            if (0..4096).contains(&d) {
                self.a.add_w_imm(T4, T4, d as u32);
            } else if (-4095..0).contains(&d) {
                self.a.sub_w_imm(T4, T4, (-d) as u32);
            } else {
                self.a.mov_w_imm(T5, disp);
                self.a.add_w(T4, T4, T5);
            }
        }
    }

    /// Load operand i (zero-extended) into host register rd.
    fn load(&mut self, insn: &Instruction, i: u32, rd: u32) {
        match insn.op_kind(i) {
            OpKind::Register => {
                let r = insn.op_register(i);
                let h = gr(r);
                match r.size() {
                    4 => self.a.mov_w(rd, h),
                    2 => self.a.uxth_w(rd, h),
                    _ => {
                        if matches!(r, Register::AH | Register::CH | Register::DH | Register::BH) {
                            self.a.ubfx_w(rd, h, 8, 8);
                        } else {
                            self.a.uxtb_w(rd, h);
                        }
                    }
                }
            }
            OpKind::Memory => {
                self.ea(insn);
                let sz = insn.memory_size().size();
                self.gld(log2(sz), rd, T4);
            }
            _ => self.a.mov_w_imm(rd, insn.immediate(i) as u32 & size_mask(interp::op_size(insn, i))),
        }
    }

    /// Store host register rs into operand 0. For memory, T4 must already
    /// hold the address if `ea_ready`.
    fn store(&mut self, insn: &Instruction, rs: u32, ea_ready: bool) {
        match insn.op0_kind() {
            OpKind::Register => {
                let r = insn.op0_register();
                let h = gr(r);
                match r.size() {
                    4 => self.a.mov_w(h, rs),
                    2 => self.a.bfi_w(h, rs, 0, 16),
                    _ => {
                        if matches!(r, Register::AH | Register::CH | Register::DH | Register::BH) {
                            self.a.bfi_w(h, rs, 8, 8);
                        } else {
                            self.a.bfi_w(h, rs, 0, 8);
                        }
                    }
                }
            }
            OpKind::Memory => {
                if !ea_ready {
                    self.ea(insn);
                }
                let sz = insn.memory_size().size();
                self.gst(log2(sz), rs, T4);
            }
            _ => unreachable!(),
        }
    }

    // --- flags / conditions --------------------------------------------------------------------

    /// Host condition equivalent to the x86 condition of `m`, if the current
    /// NZCV producer allows a direct mapping.
    fn host_cond(&self, m: Mnemonic) -> Option<Cond> {
        use Mnemonic::*;
        let f = self.nzcv?;
        let c = match f.kind {
            K_SUB | K_SHIFT | K_SBB => match m {
                Jo | Seto | Cmovo => Cond::Vs,
                Jno | Setno | Cmovno => Cond::Vc,
                Jb | Setb | Cmovb => Cond::Lo,
                Jae | Setae | Cmovae => Cond::Hs,
                Je | Sete | Cmove => Cond::Eq,
                Jne | Setne | Cmovne => Cond::Ne,
                Jbe | Setbe | Cmovbe => Cond::Ls,
                Ja | Seta | Cmova => Cond::Hi,
                Js | Sets | Cmovs => Cond::Mi,
                Jns | Setns | Cmovns => Cond::Pl,
                Jl | Setl | Cmovl => Cond::Lt,
                Jge | Setge | Cmovge => Cond::Ge,
                Jle | Setle | Cmovle => Cond::Le,
                Jg | Setg | Cmovg => Cond::Gt,
                _ => return None,
            },
            K_ADD | K_ADC => match m {
                Jo | Seto | Cmovo => Cond::Vs,
                Jno | Setno | Cmovno => Cond::Vc,
                Jb | Setb | Cmovb => Cond::Hs,
                Jae | Setae | Cmovae => Cond::Lo,
                Je | Sete | Cmove => Cond::Eq,
                Jne | Setne | Cmovne => Cond::Ne,
                Js | Sets | Cmovs => Cond::Mi,
                Jns | Setns | Cmovns => Cond::Pl,
                Jl | Setl | Cmovl => Cond::Lt,
                Jge | Setge | Cmovge => Cond::Ge,
                Jle | Setle | Cmovle => Cond::Le,
                Jg | Setg | Cmovg => Cond::Gt,
                _ => return None,
            },
            // AND/OR/XOR/TEST: C = V = 0 on both architectures
            K_LOGIC => match m {
                Jo | Seto | Cmovo => Cond::Vs,
                Jno | Setno | Cmovno => Cond::Vc,
                Je | Sete | Cmove | Jbe | Setbe | Cmovbe => Cond::Eq,
                Jne | Setne | Cmovne | Ja | Seta | Cmova => Cond::Ne,
                Js | Sets | Cmovs | Jl | Setl | Cmovl => Cond::Mi,
                Jns | Setns | Cmovns | Jge | Setge | Cmovge => Cond::Pl,
                Jle | Setle | Cmovle => Cond::Le,
                Jg | Setg | Cmovg => Cond::Gt,
                _ => return None,
            },
            // after FCMP: unordered = C,V; less = N; equal = Z,C; greater = C
            K_FCMP => match m {
                Jb | Setb | Cmovb => Cond::Lt,
                Jae | Setae | Cmovae => Cond::Ge,
                Ja | Seta | Cmova => Cond::Gt,
                Jbe | Setbe | Cmovbe => Cond::Le,
                Jp | Setp | Cmovp => Cond::Vs,
                Jnp | Setnp | Cmovnp => Cond::Vc,
                _ => return None,
            },
            K_INC | K_DEC => match m {
                Jo | Seto | Cmovo => Cond::Vs,
                Jno | Setno | Cmovno => Cond::Vc,
                Je | Sete | Cmove => Cond::Eq,
                Jne | Setne | Cmovne => Cond::Ne,
                Js | Sets | Cmovs => Cond::Mi,
                Jns | Setns | Cmovns => Cond::Pl,
                Jl | Setl | Cmovl => Cond::Lt,
                Jge | Setge | Cmovge => Cond::Ge,
                Jle | Setle | Cmovle => Cond::Le,
                Jg | Setg | Cmovg => Cond::Gt,
                _ => return None,
            },
            _ => return None,
        };
        Some(c)
    }

    /// Evaluate an x86 condition into host flags such that `Ne` means true
    /// (generic path through the helper).
    /// Materialised EFLAGS in ctx are tested inline; a lazy descriptor goes
    /// through helper_cond on a cold path.
    fn cond_generic(&mut self, m: Mnemonic) -> Cond {
        use Mnemonic::*;
        self.flush_flags();
        let (base, neg) = cc_base(m);
        self.a.ldr_imm(2, T0, CTX, OFF_FKIND);
        let fix = self.a.cbnz_w_fixup(T0);
        self.a.ldr_imm(2, T1, CTX, OFF_EFLAGS);
        match base {
            Jl | Jle => {
                self.a.eor_w_lsr(T0, T1, T1, 4); // bit 7 = SF ^ OF
                self.a.mov_w_imm(T2, SF);
                self.a.and_w(T0, T0, T2);
                if base == Jle {
                    self.a.mov_w_imm(T2, ZF);
                    self.a.and_w(T2, T1, T2);
                    self.a.orr_w(T0, T0, T2);
                }
                self.a.tst_w(T0, T0);
            }
            _ => {
                let mask = match base {
                    Jo => OF,
                    Jb => CF,
                    Je => ZF,
                    Jbe => CF | ZF,
                    Js => SF,
                    _ => PF,
                };
                self.a.mov_w_imm(T2, mask);
                self.a.tst_w(T1, T2);
            }
        }
        let c = Cold { fix, kind: ColdKind::Cond(base as u32), dirty: self.xdirty, saved: 0, valid: self.xvalid, resume: self.a.pos() };
        self.cold.push(c);
        // After the helper the descriptor in ctx is materialised; register flags are gone.
        self.nzcv = None;
        if neg { Cond::Eq } else { Cond::Ne }
    }

    /// Returns a host condition that is true iff the x86 condition holds.
    fn cond(&mut self, m: Mnemonic) -> Cond {
        if let Some(c) = self.host_cond(m) {
            return c;
        }
        if self.nzcv.map(|f| f.kind) == Some(K_FCMP) {
            let (base, neg) = cc_base(m);
            if base == Mnemonic::Je {
                // ZF = Z | V
                self.flush_flags();
                self.a.cset_w(T0, Cond::Vs);
                self.a.csinc_w(T0, T0, XZR, Cond::Ne);
                self.a.cmp_w(T0, XZR);
                self.nzcv = None;
                return if neg { Cond::Eq } else { Cond::Ne };
            }
        }
        self.cond_generic(m)
    }

    /// Current CF value into FC (for inc/dec, which preserve CF).
    fn carry_into_fc(&mut self) {
        match self.nzcv {
            Some(f) if f.kind == K_SUB => self.a.cset_w(FC, Cond::Lo),
            Some(f) if f.kind == K_ADD => self.a.cset_w(FC, Cond::Hs),
            Some(f) if f.kind == K_LOGIC => self.a.mov_w_imm(FC, 0),
            Some(f) if f.kind == K_FCMP => self.a.cset_w(FC, Cond::Lt),
            // inc/dec, adc/sbb and shifts leave CF in FC (as for flush_flags)
            Some(_) => {}
            _ if self.fc_cf => {}
            // flags in ctx: CF is eflags bit 0 if materialised, else ctx.fcf (see flush_flags)
            _ => self.load_cf_from_ctx(),
        }
    }

    // --- fallback ------------------------------------------------------------------------------

    fn fallback(&mut self, insn: &Instruction) -> bool {
        let boxed = Box::new(*insn);
        let ptr = &*boxed as *const Instruction as u64;
        self.keep.push(boxed);
        self.flush_flags();
        self.xmm_spill();
        self.fc_cf = false;
        for i in 0..4u32 {
            self.a.stp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
        }
        self.a.mov_x(0, CTX);
        self.a.mov_x_imm(1, ptr);
        self.a.ldr_imm(3, 16, CTX, OFF_HELPER_INTERP);
        self.a.blr(16);
        for i in 0..4u32 {
            self.a.ldp_w(G[2 * i as usize], G[2 * i as usize + 1], CTX, 8 * i);
        }
        self.nzcv = None;
        self.dirty = false;
        // MXCSR writes leave the block so that the engine can switch translation mode.
        if insn.flow_control() != FlowControl::Next
            || matches!(insn.mnemonic(), Mnemonic::Ldmxcsr | Mnemonic::Fxrstor | Mnemonic::Xrstor)
        {
            // control transfer: always back to the dispatcher (eip in ctx)
            self.b_exit_keep_w0();
            return false;
        }
        // w0 != 0 -> leave the block (fault)
        let cont = self.a.cbz_w_fixup(0);
        self.b_exit_keep_w0();
        self.a.bind(cont);
        true
    }

    /// Branch to the exit with w0 as returned by the helper (1 -> dispatch).
    fn b_exit_keep_w0(&mut self) {
        // map 1 (jump) to EXIT_DISPATCH, keep 2 (fault)
        self.a.subs_w_imm(XZR, 0, 1);
        self.a.mov_w_imm(1, EXIT_DISPATCH);
        self.a.csel_w(0, 1, 0, Cond::Eq);
        self.b_exit();
    }

    // --- instructions ----------------------------------------------------------------------------

    /// Translate one instruction; returns false if it ended the block.
    fn insn(&mut self, insn: &Instruction, flags_live: bool) -> bool {
        let r = self.insn_inner(insn, flags_live);
        if insn.rflags_modified() != 0 {
            self.fc_cf = false;
        }
        if may_write_ebp(insn) {
            self.ebp_frame = insn.mnemonic() == Mnemonic::Mov
                && insn.op0_kind() == OpKind::Register
                && insn.op0_register() == Register::EBP
                && insn.op1_kind() == OpKind::Register
                && insn.op1_register() == Register::ESP;
        }
        r
    }

    fn insn_inner(&mut self, insn: &Instruction, flags_live: bool) -> bool {
        use Mnemonic::*;
        let m = insn.mnemonic();
        let size = if insn.op_count() > 0 { interp::op_size(insn, 0) } else { 4 };
        let off = jit_off();
        if off & OFF_ALL != 0 {
            self.seg_end();
            return self.fallback(insn);
        }
        let cand = Self::seg_candidate(insn);
        if !cand {
            self.seg_end();
        } else if self.seg.is_none() {
            self.seg = Some(Seg { insns: Vec::new(), modified: 0, saved: 0, checked: false });
        }
        if let Some(r) = self.sse(insn, flags_live) {
            if let Some(seg) = &mut self.seg {
                let boxed = Box::new(*insn);
                seg.insns.push(&*boxed as *const Instruction as u64);
                self.keep.push(boxed);
            }
            return r;
        }
        self.seg_end();
        if let Some(r) = self.x87(insn) {
            return r;
        }
        // Instructions with prefixes we do not model natively go to the interpreter.
        let lock_ok = matches!(m, Xadd | Cmpxchg)
            && size == 4
            && insn.op0_kind() == OpKind::Memory
            && insn.op1_kind() == OpKind::Register;
        if (insn.has_lock_prefix() && !lock_ok) || insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix()
            || insn.segment_prefix() == Register::FS || insn.segment_prefix() == Register::GS
        {
            return self.fallback(insn);
        }
        for i in 0..insn.op_count() {
            match insn.op_kind(i) {
                OpKind::Register if !is_gpr(insn.op_register(i)) => return self.fallback(insn),
                OpKind::Register | OpKind::Memory | OpKind::Immediate8 | OpKind::Immediate16
                | OpKind::Immediate32 | OpKind::Immediate8to16 | OpKind::Immediate8to32 | OpKind::NearBranch32 => {}
                _ => return self.fallback(insn),
            }
        }
        if (off & OFF_INT != 0 || jit_off_mnemonic(m)) && insn.flow_control() == FlowControl::Next {
            return self.fallback(insn);
        }
        if off & OFF_FAST == 0 && self.fast_path(insn, m, size, flags_live) {
            return true;
        }
        match m {
            // prefetch is only a cache hint (never faults)
            Nop | Prefetcht0 | Prefetcht1 | Prefetcht2 | Prefetchnta | Prefetchw => {}
            Mov => {
                self.load(insn, 1, T0);
                self.store(insn, T0, false);
            }
            Movzx => {
                self.load(insn, 1, T0);
                self.store(insn, T0, false);
            }
            Movsx => {
                self.load(insn, 1, T0);
                if interp::op_size(insn, 1) == 1 { self.a.sxtb_w(T0, T0) } else { self.a.sxth_w(T0, T0) }
                self.store(insn, T0, false);
            }
            Lea => {
                self.ea(insn);
                self.store(insn, T4, false);
            }
            Add | Sub | Cmp | And | Or | Xor | Test => {
                if size != 4 && size != 2 && size != 1 {
                    return self.fallback(insn);
                }
                self.load(insn, 0, T0); // T4 = address if memory
                self.load_keep_ea(insn, 1, T1);
                let sh = 32 - size * 8;
                let (kind, store) = match m {
                    Add => (K_ADD, true),
                    Sub => (K_SUB, true),
                    Cmp => (K_SUB, false),
                    And => (K_LOGIC, true),
                    Test => (K_LOGIC, false),
                    _ => (K_LOGIC, true),
                };
                if sh != 0 {
                    self.a.lsl_w_imm(T2, T0, sh);
                    self.a.lsl_w_imm(T3, T1, sh);
                } else {
                    self.a.mov_w(T2, T0);
                    self.a.mov_w(T3, T1);
                }
                match m {
                    Add => self.a.adds_w(T2, T2, T3),
                    Sub | Cmp => self.a.subs_w(T2, T2, T3),
                    And | Test => self.a.ands_w(T2, T2, T3),
                    Or => {
                        self.a.orr_w(T2, T2, T3);
                        self.a.tst_w(T2, T2);
                    }
                    _ => {
                        self.a.eor_w(T2, T2, T3);
                        self.a.tst_w(T2, T2);
                    }
                }
                if sh != 0 {
                    self.a.lsr_w_imm(T2, T2, sh);
                }
                if flags_live {
                    self.a.mov_w(FA, T0);
                    self.a.mov_w(FB, T1);
                    self.a.mov_w(FR, T2);
                    self.nzcv = Some(FlagSrc { kind, size });
                    self.dirty = true;
                } else {
                    self.nzcv = Some(FlagSrc { kind, size });
                    self.dirty = false;
                }
                if store {
                    self.store(insn, T2, true);
                }
            }
            Inc | Dec => {
                if flags_live {
                    self.carry_into_fc();
                }
                self.load(insn, 0, T0);
                let sh = 32 - size * 8;
                self.a.lsl_w_imm(T2, T0, sh);
                self.a.mov_w_imm(T3, 1 << sh);
                if m == Inc { self.a.adds_w(T2, T2, T3) } else { self.a.subs_w(T2, T2, T3) }
                if sh != 0 {
                    self.a.lsr_w_imm(T2, T2, sh);
                }
                if flags_live {
                    self.a.mov_w(FA, T0);
                    self.a.mov_w_imm(FB, 1);
                    self.a.mov_w(FR, T2);
                    self.dirty = true;
                } else {
                    self.dirty = false;
                }
                self.nzcv = Some(FlagSrc { kind: if m == Inc { K_INC } else { K_DEC }, size });
                self.store(insn, T2, true);
            }
            Neg => {
                self.load(insn, 0, T0);
                let sh = 32 - size * 8;
                self.a.lsl_w_imm(T3, T0, sh);
                self.a.subs_w(T2, XZR, T3);
                if sh != 0 {
                    self.a.lsr_w_imm(T2, T2, sh);
                }
                if flags_live {
                    self.a.mov_w_imm(FA, 0);
                    self.a.mov_w(FB, T0);
                    self.a.mov_w(FR, T2);
                    self.dirty = true;
                } else {
                    self.dirty = false;
                }
                self.nzcv = Some(FlagSrc { kind: K_SUB, size });
                self.store(insn, T2, true);
            }
            Not => {
                self.load(insn, 0, T0);
                self.a.mvn_w(T0, T0);
                self.store(insn, T0, true);
            }
            Shl | Shr | Sar if !flags_live && size == 4 && insn.op1_kind() != OpKind::Register => {
                let count = insn.immediate(1) as u32 & 31;
                self.load(insn, 0, T0);
                if count != 0 {
                    match m {
                        Shl => self.a.lsl_w_imm(T0, T0, count),
                        Shr => self.a.lsr_w_imm(T0, T0, count),
                        _ => self.a.asr_w_imm(T0, T0, count),
                    }
                    self.store(insn, T0, true);
                    self.nzcv = None;
                }
            }
            Shl | Shr | Sar if !flags_live && size == 4 && insn.op1_register() == Register::CL => {
                self.load(insn, 0, T0);
                self.a.ubfx_w(T1, G[1], 0, 5);
                let skip = self.a.cbz_w_fixup(T1);
                match m {
                    Shl => self.a.lslv_w(T0, T0, T1),
                    Shr => self.a.lsrv_w(T0, T0, T1),
                    _ => self.a.asrv_w(T0, T0, T1),
                }
                self.store(insn, T0, true);
                self.a.bind(skip);
                self.nzcv = None;
            }
            Shl | Shr | Sar if flags_live && size == 4 && insn.op1_kind() != OpKind::Register => {
                let c = insn.immediate(1) as u32 & 31;
                if c != 0 {
                    self.load(insn, 0, T0);
                    self.shift_imm_flags(m, c);
                    self.store(insn, T2, true);
                    self.shift_nzcv();
                }
            }
            Shl | Shr | Sar if flags_live && size == 4 && insn.op1_register() == Register::CL => {
                // count 0 leaves the flags alone: keep them in ctx on both paths
                self.flush_flags();
                self.load(insn, 0, T0);
                self.a.ubfx_w(T1, G[1], 0, 5);
                let skip = self.a.cbz_w_fixup(T1);
                match m {
                    Shl => {
                        self.a.lslv_w(T2, T0, T1);
                        self.a.mov_w_imm(T3, 32);
                        self.a.sub_w(T3, T3, T1);
                        self.a.lsrv_w(T3, T0, T3);
                    }
                    Shr => {
                        self.a.lsrv_w(T2, T0, T1);
                        self.a.sub_w_imm(T3, T1, 1);
                        self.a.lsrv_w(T3, T0, T3);
                    }
                    _ => {
                        self.a.asrv_w(T2, T0, T1);
                        self.a.sub_w_imm(T3, T1, 1);
                        self.a.lsrv_w(T3, T0, T3);
                    }
                }
                self.a.ubfx_w(FC, T3, 0, 1);
                self.shift_of(m);
                self.store(insn, T2, true);
                self.a.mov_w(FR, T2);
                self.a.mov_w_imm(T3, K_SHIFT);
                self.a.mov_w_imm(T1, 4);
                self.a.stp_w(T3, T1, CTX, OFF_FKIND);
                self.a.stp_w(FA, FB, CTX, OFF_FA);
                self.a.stp_w(FR, FC, CTX, OFF_FRES);
                self.a.bind(skip);
                self.nzcv = None;
                self.dirty = false;
            }
            Rol | Ror if !flags_live && size == 4 && insn.op1_kind() != OpKind::Register => {
                let c = insn.immediate(1) as u32 & 31;
                if c != 0 {
                    self.load(insn, 0, T0);
                    self.a.mov_w_imm(T1, if m == Rol { 32 - c } else { c });
                    self.a.rorv_w(T0, T0, T1);
                    self.store(insn, T0, true);
                }
            }
            Rol | Ror if !flags_live && size == 4 && insn.op1_register() == Register::CL => {
                self.load(insn, 0, T0);
                self.a.ubfx_w(T1, G[1], 0, 5);
                if m == Rol {
                    self.a.mov_w_imm(T2, 32);
                    self.a.sub_w(T1, T2, T1); // rorv takes the amount mod 32
                }
                self.a.rorv_w(T0, T0, T1);
                self.store(insn, T0, true);
            }
            // atomic on ARM via LSE (also without lock: one location, same result).
            // LSE atomics fault on unaligned addresses (x86 allows them): those
            // go to the interpreter. Both paths leave the flags in ctx.
            Xadd | Cmpxchg if size == 4 && insn.op0_kind() == OpKind::Memory && insn.op1_kind() == OpKind::Register => {
                let src = gr(insn.op1_register());
                self.flush_flags();
                self.xmm_spill();
                self.ea(insn);
                self.a.mov_w_imm(T3, 3);
                self.a.tst_w(T4, T3);
                let unaligned = self.a.bcond_fixup(Cond::Ne);
                self.a.add_x(T5, BASE, T4);
                if m == Xadd {
                    self.a.ldaddal_w(src, T0, T5); // T0 = old
                    self.set_add_flags(flags_live, T0, src);
                    self.a.mov_w(src, T0);
                } else {
                    // EAX gets the old value either way (equal: it already holds it); flags = cmp eax, old
                    self.a.mov_w(T0, G[EAX]);
                    self.a.casal_w(T0, src, T5); // T0 = old
                    if flags_live {
                        self.a.mov_w(FA, G[EAX]);
                        self.a.mov_w(FB, T0);
                        self.a.subs_w(FR, FA, FB);
                        self.nzcv = Some(FlagSrc { kind: K_SUB, size: 4 });
                        self.dirty = true;
                    }
                    self.a.mov_w(G[EAX], T0);
                }
                self.flush_flags();
                let done = self.a.b_fixup();
                self.a.bind(unaligned);
                let cont = self.fallback(insn);
                debug_assert!(cont);
                self.a.bind(done);
                self.nzcv = None;
                self.dirty = false;
            }
            Adc | Sbb if size == 4 => {
                self.cf_to_fc();
                self.load(insn, 0, T0);
                self.load_keep_ea(insn, 1, T1);
                if m == Adc {
                    self.a.subs_w_imm(XZR, FC, 1); // C = CF
                    self.a.adcs_w(T2, T0, T1);
                } else {
                    self.a.mov_w_imm(T3, 1);
                    self.a.eor_w(T3, T3, FC);
                    self.a.subs_w_imm(XZR, T3, 1); // C = !CF (ARM borrow convention)
                    self.a.sbcs_w(T2, T0, T1);
                }
                self.a.cset_w(FC, if m == Adc { Cond::Hs } else { Cond::Lo });
                self.store(insn, T2, true);
                if flags_live {
                    self.a.mov_w(FA, T0);
                    self.a.mov_w(FB, T1);
                    self.a.mov_w(FR, T2);
                    self.nzcv = Some(FlagSrc { kind: if m == Adc { K_ADC } else { K_SBB }, size: 4 });
                    self.dirty = true;
                } else {
                    self.nzcv = None;
                    self.dirty = false;
                }
            }
            Shld | Shrd if size == 4 && insn.op1_kind() == OpKind::Register && insn.op2_kind() == OpKind::Immediate8 => {
                let c = insn.immediate8() as u32 & 31;
                if c != 0 {
                    let b = gr(insn.op1_register());
                    self.load(insn, 0, T0);
                    if m == Shld {
                        self.a.lsl_w_imm(T2, T0, c);
                        self.a.lsr_w_imm(T3, b, 32 - c);
                        self.a.ubfx_w(FC, T0, 32 - c, 1);
                    } else {
                        self.a.lsr_w_imm(T2, T0, c);
                        self.a.lsl_w_imm(T3, b, 32 - c);
                        self.a.ubfx_w(FC, T0, c - 1, 1);
                    }
                    self.a.orr_w(T2, T2, T3);
                    self.a.eor_w(T3, T2, T0);
                    self.a.lsr_w_imm(FA, T3, 31); // OF: sign changed
                    self.store(insn, T2, true);
                    if flags_live {
                        self.a.mov_w(FR, T2);
                        self.shift_nzcv();
                    } else {
                        self.nzcv = None;
                        self.dirty = false;
                    }
                }
            }
            Shld | Shrd if size == 4 && insn.op1_kind() == OpKind::Register && insn.op2_register() == Register::CL => {
                self.flush_flags();
                let b = gr(insn.op1_register());
                self.load(insn, 0, T0);
                self.a.ubfx_w(T1, G[1], 0, 5);
                let skip = self.a.cbz_w_fixup(T1);
                self.a.mov_w_imm(T3, 32);
                self.a.sub_w(T3, T3, T1); // 32 - c
                if m == Shld {
                    self.a.lslv_w(T2, T0, T1);
                    self.a.lsrv_w(FB, b, T3);
                    self.a.lsrv_w(FC, T0, T3);
                } else {
                    self.a.lsrv_w(T2, T0, T1);
                    self.a.lslv_w(FB, b, T3);
                    self.a.sub_w_imm(T3, T1, 1);
                    self.a.lsrv_w(FC, T0, T3);
                }
                self.a.orr_w(T2, T2, FB);
                self.a.ubfx_w(FC, FC, 0, 1);
                self.a.eor_w(T3, T2, T0);
                self.a.lsr_w_imm(FA, T3, 31);
                self.store(insn, T2, true);
                self.a.mov_w(FR, T2);
                self.a.mov_w_imm(T3, K_SHIFT);
                self.a.mov_w_imm(T1, 4);
                self.a.stp_w(T3, T1, CTX, OFF_FKIND);
                self.a.stp_w(FA, FB, CTX, OFF_FA);
                self.a.stp_w(FR, FC, CTX, OFF_FRES);
                self.a.bind(skip);
                self.nzcv = None;
                self.dirty = false;
            }
            Div | Idiv if size == 4 && insn.op_count() == 1 => {
                // #DE cases (zero divisor, quotient overflow) go to the interpreter.
                self.flush_flags();
                self.xmm_spill();
                self.load(insn, 0, T1);
                let zero = self.a.cbz_w_fixup(T1);
                self.a.mov_w(T0, G[EAX]);
                self.a.orr_x_lsl(T0, T0, G[EDX], 32); // edx:eax
                let overflow;
                if m == Div {
                    self.a.udiv_x(T2, T0, T1);
                    self.a.lsr_x_imm(T3, T2, 32);
                    let ok = self.a.cbz_x_fixup(T3);
                    overflow = self.a.b_fixup();
                    self.a.bind(ok);
                    self.a.msub_x(T3, T2, T1, T0);
                } else {
                    self.a.sxtw_x(T5, T1);
                    self.a.sdiv_x(T2, T0, T5);
                    self.a.cmp_x_sxtw(T2, T2);
                    overflow = self.a.bcond_fixup(Cond::Ne);
                    self.a.msub_x(T3, T2, T5, T0);
                }
                self.a.mov_w(G[EAX], T2);
                self.a.mov_w(G[EDX], T3);
                let done = self.a.b_fixup();
                self.a.bind(zero);
                self.a.bind(overflow);
                let cont = self.fallback(insn);
                debug_assert!(cont);
                self.a.bind(done);
                self.nzcv = None;
                self.dirty = false;
            }
            Bt if size == 4 && !(insn.op0_kind() == OpKind::Memory && insn.op1_kind() == OpKind::Register) => {
                if flags_live {
                    // CF = bit; ZF unchanged (others undefined): materialise, then set CF
                    self.flush_flags();
                    self.xmm_spill();
                    self.a.ldr_imm(2, T0, CTX, OFF_FKIND);
                    let mat = self.a.cbz_w_fixup(T0);
                    self.a.mov_x(0, CTX);
                    self.a.ldr_imm(3, 16, CTX, OFF_HELPER_FLAGS);
                    self.a.blr(16);
                    self.a.bind(mat);
                    self.load(insn, 0, T0);
                    if insn.op1_kind() == OpKind::Register {
                        self.a.ubfx_w(T2, gr(insn.op1_register()), 0, 5);
                        self.a.lsrv_w(T1, T0, T2);
                    } else {
                        self.a.lsr_w_imm(T1, T0, insn.immediate8() as u32 & 31);
                    }
                    self.a.ldr_imm(2, T2, CTX, OFF_EFLAGS);
                    self.a.bfi_w(T2, T1, 0, 1);
                    self.a.str_imm(2, T2, CTX, OFF_EFLAGS);
                    self.nzcv = None;
                    self.dirty = false;
                    self.fc_cf = false;
                }
            }
            // pushfd/popfd (HR's GL context wrapper saves ZF across CGLUnlockContext
            // ~580k times/s). Same masks as the interpreter.
            Pushfd => {
                self.flush_flags();
                self.xmm_spill();
                self.a.ldr_imm(2, T0, CTX, OFF_FKIND);
                let mat = self.a.cbz_w_fixup(T0);
                self.a.mov_x(0, CTX);
                self.a.ldr_imm(3, 16, CTX, OFF_HELPER_FLAGS);
                self.a.blr(16);
                self.a.bind(mat);
                self.a.ldr_imm(2, T0, CTX, OFF_EFLAGS);
                self.a.mov_w_imm(T1, 0x00fc_0fd5);
                self.a.and_w(T0, T0, T1);
                self.a.mov_w_imm(T1, 2);
                self.a.orr_w(T0, T0, T1);
                let esp = G[ESP];
                self.a.sub_w_imm(esp, esp, 4);
                self.a.str_uxtw(2, T0, BASE, esp);
                self.nzcv = None;
                self.dirty = false;
                self.fc_cf = false;
            }
            Popfd => {
                // all arithmetic flags come from the stack: pending lazy flags are dead
                let esp = G[ESP];
                self.a.ldr_uxtw(2, T0, BASE, esp);
                self.a.add_w_imm(esp, esp, 4);
                self.a.ldr_imm(2, T1, CTX, OFF_EFLAGS);
                self.a.mov_w_imm(T2, 0x0024_4dd5);
                self.a.bic_w(T1, T1, T2);
                self.a.and_w(T0, T0, T2);
                self.a.orr_w(T1, T1, T0);
                self.a.mov_w_imm(T2, 2);
                self.a.orr_w(T1, T1, T2);
                self.a.str_imm(2, T1, CTX, OFF_EFLAGS);
                self.a.str_imm(2, XZR, CTX, OFF_FKIND);
                self.nzcv = None;
                self.dirty = false;
                self.fc_cf = false;
            }
            Imul if !flags_live && insn.op_count() >= 2 && size == 4 => {
                if insn.op_count() == 3 {
                    self.load(insn, 1, T0);
                    self.a.mov_w_imm(T1, insn.immediate(2) as u32);
                } else {
                    self.load(insn, 0, T0);
                    self.load(insn, 1, T1);
                }
                self.a.mul_w(T0, T0, T1);
                self.store_reg0(insn, T0);
                self.nzcv = None;
            }
            Mul | Imul if !flags_live && insn.op_count() == 1 && size == 4 => {
                self.load(insn, 0, T1);
                if m == Mul { self.a.umull(T0, G[EAX], T1) } else { self.a.smull(T0, G[EAX], T1) }
                self.a.mov_w(G[EAX], T0);
                self.a.lsr_x_imm(G[EDX], T0, 32);
                self.nzcv = None;
            }
            Push if size == 4 => {
                self.load(insn, 0, T0);
                let esp = G[ESP];
                self.a.sub_w_imm(esp, esp, 4);
                self.a.str_uxtw(2, T0, BASE, esp);
            }
            Pop if size == 4 => {
                let esp = G[ESP];
                self.a.ldr_uxtw(2, T0, BASE, esp);
                self.a.add_w_imm(esp, esp, 4);
                self.store(insn, T0, false);
            }
            Cdq => self.a.asr_w_imm(G[EDX], G[EAX], 31),
            Cwde => self.a.sxth_w(G[EAX], G[EAX]),
            Leave => {
                let (esp, ebp) = (G[ESP], G[EBP]);
                self.a.mov_w(esp, ebp);
                self.a.ldr_uxtw(2, ebp, BASE, esp);
                self.a.add_w_imm(esp, esp, 4);
            }
            Xchg if insn.op0_kind() == OpKind::Register && insn.op1_kind() == OpKind::Register && size == 4 => {
                let (r0, r1) = (gr(insn.op0_register()), gr(insn.op1_register()));
                self.a.mov_w(T0, r0);
                self.a.mov_w(r0, r1);
                self.a.mov_w(r1, T0);
            }
            Seto | Setno | Setb | Setae | Sete | Setne | Setbe | Seta | Sets | Setns | Setp | Setnp | Setl | Setge
            | Setle | Setg => {
                let c = self.cond(m);
                self.a.cset_w(T0, c);
                self.store(insn, T0, false);
            }
            Cmovo | Cmovno | Cmovb | Cmovae | Cmove | Cmovne | Cmovbe | Cmova | Cmovs | Cmovns | Cmovp | Cmovnp
            | Cmovl | Cmovge | Cmovle | Cmovg if size == 4 => {
                // Condition first: the generic path calls a helper that clobbers x0-x17.
                let c = self.cond(m);
                self.load(insn, 1, T1);
                let dst = gr(insn.op0_register());
                self.a.csel_w(dst, T1, dst, c);
            }
            Jo | Jno | Jb | Jae | Je | Jne | Jbe | Ja | Js | Jns | Jp | Jnp | Jl | Jge | Jle | Jg => {
                let c = self.cond(m);
                let taken = self.a.bcond_fixup(c);
                self.exit_direct_flags(insn.next_ip32());
                self.a.bind(taken);
                self.exit_direct_flags(insn.near_branch32());
                return false;
            }
            Jmp if insn.op0_kind() == OpKind::NearBranch32 => {
                self.exit_direct_flags(insn.near_branch32());
                return false;
            }
            Jmp => {
                self.load(insn, 0, T5);
                self.flush_flags();
                self.exit_indirect(T5);
                return false;
            }
            Call => {
                if insn.op0_kind() == OpKind::NearBranch32 {
                    self.a.mov_w_imm(T5, insn.near_branch32());
                } else {
                    self.load(insn, 0, T5);
                }
                self.a.mov_w_imm(T0, insn.next_ip32());
                let esp = G[ESP];
                self.a.sub_w_imm(esp, esp, 4);
                self.a.str_uxtw(2, T0, BASE, esp);
                self.flush_flags();
                if insn.op0_kind() == OpKind::NearBranch32 {
                    self.exit_direct(insn.near_branch32());
                } else {
                    self.exit_indirect(T5);
                }
                return false;
            }
            Ret => {
                let esp = G[ESP];
                self.a.ldr_uxtw(2, T5, BASE, esp);
                let n = 4 + if insn.op_count() > 0 { insn.immediate16() as u32 } else { 0 };
                if n < 4096 {
                    self.a.add_w_imm(esp, esp, n);
                } else {
                    self.a.mov_w_imm(T0, n);
                    self.a.add_w(esp, esp, T0);
                }
                self.flush_flags();
                self.exit_indirect(T5);
                return false;
            }
            _ => return self.fallback(insn),
        }
        true
    }

    /// 32-bit register forms operating directly on the host registers that
    /// hold the guest registers. Returns false if not applicable.
    fn fast_path(&mut self, insn: &Instruction, m: Mnemonic, size: u32, flags_live: bool) -> bool {
        use Mnemonic::*;
        if size != 4 || insn.op_count() < 1 || insn.op0_kind() != OpKind::Register {
            return false;
        }
        let d = gr(insn.op0_register());
        let src_reg = insn.op_count() > 1 && insn.op1_kind() == OpKind::Register && insn.op_register(1).size() == 4;
        let src_imm = insn.op_count() > 1
            && matches!(insn.op1_kind(), OpKind::Immediate32 | OpKind::Immediate8to32);
        let imm = if src_imm { insn.immediate(1) as u32 } else { 0 };
        match m {
            Mov if src_reg => self.a.mov_w(d, gr(insn.op1_register())),
            Mov if src_imm => self.a.mov_w_imm(d, imm),
            Movzx if insn.op1_kind() == OpKind::Register && !is_high8(insn.op1_register()) => {
                let s = gr(insn.op1_register());
                if insn.op_register(1).size() == 1 { self.a.uxtb_w(d, s) } else { self.a.uxth_w(d, s) }
            }
            Add | Sub | Cmp | And | Or | Xor | Test if src_reg || src_imm => {
                let b = if src_reg {
                    gr(insn.op1_register())
                } else if imm < 4096 && matches!(m, Add | Sub | Cmp) {
                    u32::MAX // immediate form below
                } else {
                    self.a.mov_w_imm(T1, imm);
                    T1
                };
                let kind = match m {
                    Add => K_ADD,
                    Sub | Cmp => K_SUB,
                    _ => K_LOGIC,
                };
                // Keep the operands for the lazy flag descriptor before overwriting d.
                if flags_live {
                    self.a.mov_w(FA, d);
                    if b == u32::MAX { self.a.mov_w_imm(FB, imm) } else { self.a.mov_w(FB, b) }
                }
                let dst = if matches!(m, Cmp | Test) { XZR } else { d };
                match (m, b == u32::MAX) {
                    (Add, true) => self.a.emit(0x3100_0000 | (imm << 10) | (d << 5) | dst), // adds w, w, #imm
                    (Sub | Cmp, true) => self.a.subs_w_imm(dst, d, imm),
                    (Add, false) => self.a.adds_w(dst, d, b),
                    (Sub | Cmp, false) => self.a.subs_w(dst, d, b),
                    (And | Test, _) => self.a.ands_w(dst, d, b),
                    (Or, _) => {
                        self.a.orr_w(d, d, b);
                        self.a.tst_w(d, d);
                    }
                    _ => {
                        self.a.eor_w(d, d, b);
                        self.a.tst_w(d, d);
                    }
                }
                if flags_live {
                    if matches!(m, Cmp | Test) {
                        // result needed for PF/ZF materialisation
                        match m {
                            Cmp => self.a.sub_w(FR, FA, FB),
                            _ => self.a.and_w(FR, FA, FB),
                        }
                    } else {
                        self.a.mov_w(FR, d);
                    }
                    self.dirty = true;
                } else {
                    self.dirty = false;
                }
                self.nzcv = Some(FlagSrc { kind, size: 4 });
            }
            Inc | Dec if insn.op_count() == 1 => {
                if flags_live {
                    self.carry_into_fc();
                    self.a.mov_w(FA, d);
                    self.a.mov_w_imm(FB, 1);
                }
                if m == Inc {
                    self.a.emit(0x3100_0400 | (d << 5) | d); // adds wd, wd, #1
                } else {
                    self.a.emit(0x7100_0400 | (d << 5) | d); // subs wd, wd, #1
                }
                if flags_live {
                    self.a.mov_w(FR, d);
                    self.dirty = true;
                } else {
                    self.dirty = false;
                }
                self.nzcv = Some(FlagSrc { kind: if m == Inc { K_INC } else { K_DEC }, size: 4 });
            }
            Imul if !flags_live && insn.op_count() == 3 && insn.op1_kind() == OpKind::Register => {
                self.a.mov_w_imm(T1, insn.immediate(2) as u32);
                self.a.mul_w(d, gr(insn.op1_register()), T1);
                self.nzcv = None;
            }
            Imul if !flags_live && insn.op_count() == 2 && src_reg => {
                self.a.mul_w(d, d, gr(insn.op1_register()));
                self.nzcv = None;
            }
            _ => return false,
        }
        true
    }

    // --- SSE ---------------------------------------------------------------------------------------

    /// Operand i (xmm register or memory) in a vector register. Memory is loaded
    /// into `tmp` with the operand's size (narrow loads zero the upper bits).
    fn xsrc(&mut self, insn: &Instruction, i: u32, tmp: u32) -> u32 {
        if insn.op_kind(i) == OpKind::Register {
            return self.xget(xidx(insn.op_register(i)));
        }
        let sz = match insn.memory_size().size() {
            4 => 2,
            8 => 3,
            _ => 4,
        };
        self.ea(insn);
        self.vld(sz, tmp, T4);
        tmp
    }

    /// Translate an SSE instruction natively. None: not handled here (the
    /// caller falls back to the interpreter); Some(cont) otherwise.
    fn sse(&mut self, insn: &Instruction, flags_live: bool) -> Option<bool> {
        if jit_off() & OFF_SSE != 0 {
            return None;
        }
        use Mnemonic::*;
        let m = insn.mnemonic();
        let n = insn.op_count();
        let mut has_xmm = false;
        for i in 0..n {
            match insn.op_kind(i) {
                OpKind::Register => {
                    let r = insn.op_register(i);
                    if r.is_xmm() {
                        has_xmm = true;
                    } else if !(is_gpr(r) && r.size() == 4) {
                        return None;
                    }
                }
                OpKind::Memory => {
                    if matches!(insn.segment_prefix(), Register::FS | Register::GS)
                        || !matches!(insn.memory_size().size(), 4 | 8 | 16)
                    {
                        return None;
                    }
                }
                OpKind::Immediate8 => {}
                _ => return None,
            }
        }
        if insn.has_lock_prefix() {
            return None;
        }
        if !has_xmm && !matches!(m, Cvttss2si | Cvtss2si | Cvttsd2si | Cvtsd2si | Stmxcsr) {
            return None;
        }
        let reg = |i: u32| insn.op_kind(i) == OpKind::Register;
        let xi = |i: u32| xidx(insn.op_register(i));
        let fp = self.fp_ok;
        let is_pd = |m: Mnemonic| {
            matches!(
                m,
                Addpd | Addsd | Subpd | Subsd | Mulpd | Mulsd | Divpd | Divsd | Sqrtpd | Sqrtsd | Minpd | Minsd
                    | Maxpd | Maxsd | Cmppd | Cmpsd | Comisd | Ucomisd | Cvtsi2sd | Cvttsd2si | Cvtsd2si
            )
        };
        let is_scalar = |m: Mnemonic| {
            matches!(
                m,
                Addss | Addsd | Subss | Subsd | Mulss | Mulsd | Divss | Divsd | Sqrtss | Sqrtsd | Minss | Minsd
                    | Maxss | Maxsd | Cmpss | Cmpsd
            )
        };
        match m {
            // ---- data movement ----
            Movaps | Movups | Movapd | Movupd | Movdqa | Movdqu | Lddqu | Movntps | Movntpd | Movntdq => {
                if reg(0) {
                    let d = xi(0);
                    if reg(1) {
                        let s = xi(1);
                        if s != d {
                            let vs = self.xget(s);
                            let vd = self.xdef(d);
                            self.a.vmov(vd, vs);
                        }
                    } else {
                        self.ea(insn);
                        let vd = self.xdef(d);
                        self.vld(4, vd, T4);
                    }
                } else {
                    let vs = self.xget(xi(1));
                    self.ea(insn);
                    self.vst(4, vs, T4);
                }
            }
            Movss | Movsd => {
                let sz = if m == Movss { 2 } else { 3 };
                if reg(0) && reg(1) {
                    let vs = self.xget(xi(1));
                    let vd = self.xmod(xi(0));
                    self.a.ins_elem(sz, vd, 0, vs, 0);
                } else if reg(0) {
                    self.ea(insn);
                    let vd = self.xdef(xi(0));
                    self.vld(sz, vd, T4);
                } else {
                    let vs = self.xget(xi(1));
                    self.ea(insn);
                    self.vst(sz, vs, T4);
                }
            }
            Movd => {
                if reg(0) && insn.op0_register().is_xmm() {
                    if reg(1) {
                        let g = gr(insn.op1_register());
                        let vd = self.xdef(xi(0));
                        self.a.fmov_s_w(vd, g);
                    } else {
                        self.ea(insn);
                        let vd = self.xdef(xi(0));
                        self.vld(2, vd, T4);
                    }
                } else {
                    let vs = self.xget(xi(1));
                    if reg(0) {
                        self.a.fmov_w_s(gr(insn.op0_register()), vs);
                    } else {
                        self.ea(insn);
                        self.vst(2, vs, T4);
                    }
                }
            }
            Movq => {
                if reg(0) && reg(1) {
                    let vs = self.xget(xi(1));
                    let vd = self.xdef(xi(0));
                    self.a.vmov8(vd, vs);
                } else if reg(0) {
                    self.ea(insn);
                    let vd = self.xdef(xi(0));
                    self.vld(3, vd, T4);
                } else {
                    let vs = self.xget(xi(1));
                    self.ea(insn);
                    self.vst(3, vs, T4);
                }
            }
            Movlps | Movlpd | Movhps | Movhpd => {
                let hi = matches!(m, Movhps | Movhpd) as u32;
                if reg(0) {
                    self.ea(insn);
                    self.vld(3, V0, T4);
                    let vd = self.xmod(xi(0));
                    self.a.ins_elem(3, vd, hi, V0, 0);
                } else {
                    let vs = self.xget(xi(1));
                    self.ea(insn);
                    if hi == 1 {
                        self.a.ins_elem(3, V0, 0, vs, 1);
                        self.vst(3, V0, T4);
                    } else {
                        self.vst(3, vs, T4);
                    }
                }
            }
            Movhlps | Movlhps => {
                let vs = self.xget(xi(1));
                let vd = self.xmod(xi(0));
                if m == Movhlps {
                    self.a.ins_elem(3, vd, 0, vs, 1);
                } else {
                    self.a.ins_elem(3, vd, 1, vs, 0);
                }
            }
            Movsldup | Movshdup => {
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xdef(xi(0));
                self.a.vperm(if m == Movsldup { 2 } else { 6 }, 2, vd, vs, vs);
            }
            Movddup => {
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xdef(xi(0));
                self.a.dup_elem(3, vd, vs, 0);
            }
            Movmskps | Movmskpd if reg(0) && reg(1) => {
                let vs = self.xget(xi(1));
                let g = gr(insn.op0_register());
                if m == Movmskps {
                    self.a.vushr(2, V0, vs, 31);
                    self.ldr_const(V1, (3u128 << 96) | (2u128 << 64) | (1u128 << 32));
                    self.a.vushl(2, V0, V0, V1);
                    self.a.addv_4s(V0, V0);
                    self.a.fmov_w_s(g, V0);
                } else {
                    self.a.vushr(3, V0, vs, 63);
                    self.a.umov_w_s(T0, V0, 0);
                    self.a.umov_w_s(T1, V0, 2);
                    self.a.orr_w_lsl(g, T0, T1, 1);
                }
            }
            Stmxcsr => {
                // Exception flags are only accumulated by the interpreter (approximation).
                self.ensure_xbase();
                self.a.ldr_imm(2, T0, XB, MXCSR_OFF - XMM_OFF);
                self.ea(insn);
                self.gst(2, T0, T4);
            }

            // ---- float arithmetic ----
            Addps | Addss | Addpd | Addsd | Subps | Subss | Subpd | Subsd | Mulps | Mulss | Mulpd | Mulsd | Divps
            | Divss | Divpd | Divsd | Sqrtps | Sqrtss | Sqrtpd | Sqrtsd
                if fp =>
            {
                let (dbl, scalar) = (is_pd(m), is_scalar(m));
                let vs = self.xsrc(insn, 1, V1);
                let d = xi(0);
                let sqrt = matches!(m, Sqrtps | Sqrtss | Sqrtpd | Sqrtsd);
                // packed: computed in place (the segment keeps the old value); scalar: via V0
                let (vd, r) = if scalar {
                    (self.xget(d), V0)
                } else if sqrt {
                    let vd = self.xdef(d);
                    (vd, vd)
                } else {
                    let vd = self.xmod(d);
                    (vd, vd)
                };
                // Single precision: x86 NaN results are produced exactly (x86_nan_fixup
                // on a cold path), so no deferred check. The first operand is overwritten by packed ops in
                // place, keep a copy (also when it is the source register).
                let exact = !dbl;
                let (a, b) = if !exact {
                    (vd, vs)
                } else if sqrt {
                    if !scalar && vs == vd {
                        self.a.vmov(V4, vs); // fsqrt overwrites its own source
                        (vd, V4)
                    } else {
                        (vd, vs)
                    }
                } else if scalar {
                    (vd, vs)
                } else {
                    self.a.vmov(V4, vd);
                    (V4, if vs == vd { V4 } else { vs })
                };
                let op = match m {
                    Addps | Addss | Addpd | Addsd => 0,
                    Subps | Subss | Subpd | Subsd => 1,
                    Mulps | Mulss | Mulpd | Mulsd => 2,
                    Divps | Divss | Divpd | Divsd => 3,
                    _ => 4,
                };
                match (op, scalar) {
                    (0, true) => self.a.fadd_s(dbl, r, vd, vs),
                    (1, true) => self.a.fsub_s(dbl, r, vd, vs),
                    (2, true) => self.a.fmul_s(dbl, r, vd, vs),
                    (3, true) => self.a.fdiv_s(dbl, r, vd, vs),
                    (_, true) => self.a.fsqrt_s(dbl, r, vs),
                    (0, false) => self.a.fadd_v(dbl, r, vd, vs),
                    (1, false) => self.a.fsub_v(dbl, r, vd, vs),
                    (2, false) => self.a.fmul_v(dbl, r, vd, vs),
                    (3, false) => self.a.fdiv_v(dbl, r, vd, vs),
                    (_, false) => self.a.fsqrt_v(dbl, r, vs),
                }
                if exact {
                    self.x86_nan_check(r, if sqrt { None } else { Some(a) }, b);
                } else {
                    // scalar ops zero the upper lanes of V0, so a full-width check is fine
                    self.seg_check(dbl, r);
                }
                if scalar {
                    let vd = self.xmod(d);
                    self.a.ins_elem(if dbl { 3 } else { 2 }, vd, 0, V0, 0);
                }
            }
            // x86: min = a < b ? a : b, max = a > b ? a : b (second operand on NaN / equal)
            Minps | Minss | Minpd | Minsd | Maxps | Maxss | Maxpd | Maxsd if fp => {
                let (dbl, scalar) = (is_pd(m), is_scalar(m));
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                if matches!(m, Maxps | Maxss | Maxpd | Maxsd) {
                    self.a.fcmgt_v(dbl, V0, vd, vs);
                } else {
                    self.a.fcmgt_v(dbl, V0, vs, vd);
                }
                if scalar {
                    self.a.vbsl(V0, vd, vs);
                    self.a.ins_elem(if dbl { 3 } else { 2 }, vd, 0, V0, 0);
                } else {
                    self.a.vbif(vd, vs, V0);
                }
            }
            Cmpps | Cmpss | Cmppd | Cmpsd if fp && reg(0) => {
                let (dbl, scalar) = (is_pd(m), is_scalar(m));
                let pred = insn.immediate8() as u32 & 7;
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                match pred {
                    0 | 4 => self.a.fcmeq_v(dbl, V0, vd, vs),
                    1 | 5 => self.a.fcmgt_v(dbl, V0, vs, vd),
                    2 | 6 => self.a.fcmge_v(dbl, V0, vs, vd),
                    _ => {
                        self.a.fcmeq_v(dbl, V0, vd, vd);
                        self.a.fcmeq_v(dbl, V2, vs, vs);
                        self.a.vand(V0, V0, V2);
                    }
                }
                if matches!(pred, 3 | 4 | 5 | 6) {
                    self.a.vnot(V0, V0);
                }
                if scalar {
                    self.a.ins_elem(if dbl { 3 } else { 2 }, vd, 0, V0, 0);
                } else {
                    self.a.vmov(vd, V0);
                }
            }
            Comiss | Ucomiss | Comisd | Ucomisd if fp => {
                let dbl = is_pd(m);
                let vs = self.xsrc(insn, 1, V1);
                let va = self.xget(xi(0));
                self.a.fcmp_s(dbl, va, vs);
                self.nzcv = Some(FlagSrc { kind: K_FCMP, size: 4 });
                self.dirty = flags_live;
            }

            // ---- conversions ----
            Cvtsi2ss | Cvtsi2sd if fp => {
                let dbl = m == Cvtsi2sd;
                let w = if reg(1) {
                    gr(insn.op1_register())
                } else {
                    self.ea(insn);
                    self.gld(2, T0, T4);
                    T0
                };
                self.a.scvtf_s_w(dbl, V0, w);
                let vd = self.xmod(xi(0));
                self.a.ins_elem(if dbl { 3 } else { 2 }, vd, 0, V0, 0);
            }
            Cvttss2si | Cvtss2si | Cvttsd2si | Cvtsd2si if fp && reg(0) => {
                // x86 returns 0x80000000 for NaN and out-of-range values; ARM saturates.
                let dbl = is_pd(m);
                let vs = self.xsrc(insn, 1, V1);
                let g = gr(insn.op0_register());
                let save = self.nzcv.is_some();
                if save {
                    self.a.mrs_nzcv(NZ);
                }
                if matches!(m, Cvttss2si | Cvttsd2si) {
                    self.a.fcvtzs_x(dbl, T0, vs);
                } else {
                    self.a.fcvtns_x(dbl, T0, vs);
                }
                self.a.cmp_x_sxtw(T0, T0);
                self.a.mov_w_imm(T1, 0x8000_0000);
                self.a.csel_w(T0, T0, T1, Cond::Eq);
                self.a.fcmp_s(dbl, vs, vs);
                self.a.csel_w(g, T1, T0, Cond::Vs);
                if save {
                    self.a.msr_nzcv(NZ);
                }
            }
            Cvtss2sd | Cvtsd2ss if fp => {
                let vs = self.xsrc(insn, 1, V1);
                if m == Cvtss2sd {
                    self.a.fcvt_d_s(V0, vs);
                } else {
                    self.a.fcvt_s_d(V0, vs);
                }
                let vd = self.xmod(xi(0));
                self.a.ins_elem(if m == Cvtss2sd { 3 } else { 2 }, vd, 0, V0, 0);
            }
            Cvtps2pd | Cvtpd2ps | Cvtdq2ps | Cvtdq2pd if fp => {
                let vs = self.xsrc(insn, 1, V1);
                match m {
                    Cvtps2pd => self.a.fcvtl_2d(V0, vs),
                    Cvtpd2ps => self.a.fcvtn_2s(V0, vs),
                    Cvtdq2ps => self.a.scvtf_v(false, V0, vs),
                    _ => {
                        self.a.sxtl_2d(V0, vs);
                        self.a.scvtf_v(true, V0, V0);
                    }
                }
                let vd = self.xdef(xi(0));
                self.a.vmov(vd, V0);
            }
            Cvttps2dq | Cvtps2dq if fp => {
                let vs = self.xsrc(insn, 1, V1);
                if m == Cvttps2dq {
                    self.a.fcvtzs_4s(V0, vs);
                } else {
                    self.a.fcvtns_4s(V0, vs);
                }
                // lanes with !(x < 2^31) (too large or NaN) -> 0x80000000
                self.a.movi_4s(V2, 0x4f, 24);
                self.a.fcmgt_v(false, V2, V2, vs);
                self.a.movi_4s(V1, 0x80, 24);
                self.a.vbif(V0, V1, V2);
                let vd = self.xdef(xi(0));
                self.a.vmov(vd, V0);
            }

            Haddps | Haddpd | Hsubps | Hsubpd | Addsubps | Addsubpd if fp => {
                let dbl = matches!(m, Haddpd | Hsubpd | Addsubpd);
                let sz = if dbl { 3 } else { 2 };
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                match m {
                    Haddps | Haddpd => self.a.faddp_v(dbl, vd, vd, vs),
                    Hsubps | Hsubpd => {
                        self.a.vperm(1, sz, V0, vd, vs); // even elements
                        self.a.vperm(5, sz, V2, vd, vs); // odd elements
                        self.a.fsub_v(dbl, vd, V0, V2);
                    }
                    Addsubps => {
                        self.a.fsub_v(false, V0, vd, vs);
                        self.a.fadd_v(false, V2, vd, vs);
                        self.a.rev64_4s(V2, V2);
                        self.a.vperm(2, 2, vd, V0, V2); // [sub0, add1, sub2, add3]
                    }
                    _ => {
                        self.a.fadd_v(true, V2, vd, vs);
                        self.a.fsub_v(true, vd, vd, vs);
                        self.a.ins_elem(3, vd, 1, V2, 1);
                    }
                }
                self.seg_check(dbl, vd);
            }
            // The interpreter defines rcp/rsqrt as the correctly rounded value computed in
            // double precision, with denormal inputs as zero and tiny rcp results flushed
            // (MXCSR is ignored). Same computation here (host FPCR is the default).
            Rcpps | Rcpss | Rsqrtps | Rsqrtss => {
                let rsqrt = matches!(m, Rsqrtps | Rsqrtss);
                let vs = self.xsrc(insn, 1, V1);
                // a' = denormal ? signed zero : a
                self.a.movi_4s(V4, 0x80, 16); // 2^-126
                self.a.facgt_v(false, V5, V4, vs);
                self.a.movi_4s(V6, 0x80, 24); // sign bit
                self.a.vand(V2, vs, V6);
                self.a.vbsl(V5, V2, vs);
                self.a.fcvtl_2d(V0, V5);
                self.a.fcvtl_2d_hi(V2, V5);
                if rsqrt {
                    self.a.v2misc(1, 1, 3, 0x1f, V0, V0); // fsqrt .2d
                    self.a.v2misc(1, 1, 3, 0x1f, V2, V2);
                }
                self.a.fmov_2d_one(V1);
                self.a.fdiv_v(true, V0, V1, V0);
                self.a.fdiv_v(true, V2, V1, V2);
                self.a.fcvtn_2s(V0, V0);
                self.a.fcvtn_4s_hi(V0, V2);
                if rsqrt {
                    // negative (non-zero, non-NaN) inputs -> x86 default NaN
                    self.a.fcmlt_zero_4s(V2, V5);
                    self.ldr_const(V1, 0xffc0_0000_ffc0_0000_ffc0_0000_ffc0_0000);
                    self.a.vbsl(V2, V1, V0);
                    self.a.vmov(V0, V2);
                } else {
                    // tiny result or |a| >= 2^126 -> signed zero
                    self.a.facgt_v(false, V2, V4, V0);
                    self.ldr_const(V1, 0x7e80_0000_7e80_0000_7e80_0000_7e80_0000);
                    self.a.facge_v(false, V1, V5, V1);
                    self.a.vorr(V2, V2, V1);
                    self.a.vand(V1, V0, V6);
                    self.a.vbsl(V2, V1, V0);
                    self.a.vmov(V0, V2);
                }
                if matches!(m, Rcpss | Rsqrtss) {
                    let vd = self.xmod(xi(0));
                    self.a.ins_elem(2, vd, 0, V0, 0);
                } else {
                    let vd = self.xdef(xi(0));
                    self.a.vmov(vd, V0);
                }
            }
            Pmaddwd | Pmulhw | Pmulhuw | Pmuludq => {
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                match m {
                    Pmaddwd => {
                        self.a.vmull(false, false, 1, V0, vd, vs);
                        self.a.vmull(true, false, 1, V2, vd, vs);
                        self.a.vaddp(2, vd, V0, V2);
                    }
                    Pmulhw | Pmulhuw => {
                        let u = m == Pmulhuw;
                        self.a.vmull(false, u, 1, V0, vd, vs);
                        self.a.vmull(true, u, 1, V2, vd, vs);
                        self.a.vperm(5, 1, vd, V0, V2); // high halves
                    }
                    _ => {
                        self.a.xtn_2s(V0, vd);
                        self.a.xtn_2s(V2, vs);
                        self.a.vmull(false, true, 2, vd, V0, V2);
                    }
                }
            }
            Pshuflw | Pshufhw => {
                let imm = insn.immediate8() as u32;
                let o = if m == Pshufhw { 4 } else { 0 };
                let vs = self.xsrc(insn, 1, V1);
                self.a.vmov(V0, vs);
                for k in 0..4 {
                    self.a.ins_elem(1, V0, o + k, vs, o + ((imm >> (2 * k)) & 3));
                }
                let vd = self.xdef(xi(0));
                self.a.vmov(vd, V0);
            }
            Pmovmskb if reg(0) && reg(1) => {
                let vs = self.xget(xi(1));
                let g = gr(insn.op0_register());
                self.a.vushr_q(1, 0, V0, vs, 7);
                self.ldr_const(V1, 0x0706_0504_0302_0100_0706_0504_0302_0100);
                self.a.vushl(0, V0, V0, V1);
                self.a.addv_8b(V2, V0);
                self.a.ext16(V0, V0, V0, 8);
                self.a.addv_8b(V0, V0);
                self.a.umov_w(0, T0, V2, 0);
                self.a.umov_w(0, T1, V0, 0);
                self.a.orr_w_lsl(g, T0, T1, 8);
            }
            Pextrw if reg(0) && insn.code() == iced_x86::Code::Pextrw_r32_xmm_imm8 => {
                let vs = self.xget(xi(1));
                self.a.umov_w(1, gr(insn.op0_register()), vs, insn.immediate8() as u32 & 7);
            }
            Pinsrw if reg(1) => {
                let vd = self.xmod(xi(0));
                self.a.ins_gen(1, vd, insn.immediate8() as u32 & 7, gr(insn.op1_register()));
            }

            // ---- logic ----
            Andps | Andpd | Pand | Andnps | Andnpd | Pandn | Orps | Orpd | Por | Xorps | Xorpd | Pxor => {
                let d = xi(0);
                if reg(1) && xi(1) == d && matches!(m, Xorps | Xorpd | Pxor | Andnps | Andnpd | Pandn) {
                    let vd = self.xdef(d);
                    self.a.vzero(vd);
                } else {
                    let vs = self.xsrc(insn, 1, V1);
                    let vd = self.xmod(d);
                    match m {
                        Andps | Andpd | Pand => self.a.vand(vd, vd, vs),
                        Andnps | Andnpd | Pandn => self.a.vbic(vd, vs, vd),
                        Orps | Orpd | Por => self.a.vorr(vd, vd, vs),
                        _ => self.a.veor(vd, vd, vs),
                    }
                }
            }

            // ---- shuffles ----
            Shufps => {
                let imm = insn.immediate8() as u32;
                let sel = [imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3];
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                if vs == vd && sel.iter().all(|&x| x == sel[0]) {
                    self.a.dup_elem(2, vd, vd, sel[0]);
                } else {
                    self.a.ins_elem(2, V0, 0, vd, sel[0]);
                    self.a.ins_elem(2, V0, 1, vd, sel[1]);
                    self.a.ins_elem(2, V0, 2, vs, sel[2]);
                    self.a.ins_elem(2, V0, 3, vs, sel[3]);
                    self.a.vmov(vd, V0);
                }
            }
            Pshufd => {
                let imm = insn.immediate8() as u32;
                let sel = [imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3];
                let vs = self.xsrc(insn, 1, V1);
                if sel.iter().all(|&x| x == sel[0]) {
                    let vd = self.xdef(xi(0));
                    self.a.dup_elem(2, vd, vs, sel[0]);
                } else {
                    for (k, &j) in sel.iter().enumerate() {
                        self.a.ins_elem(2, V0, k as u32, vs, j);
                    }
                    let vd = self.xdef(xi(0));
                    self.a.vmov(vd, V0);
                }
            }
            Shufpd => {
                let imm = insn.immediate8() as u32;
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                self.a.ins_elem(3, V0, 0, vd, imm & 1);
                self.a.ins_elem(3, V0, 1, vs, (imm >> 1) & 1);
                self.a.vmov(vd, V0);
            }
            Unpcklps | Unpckhps | Unpcklpd | Unpckhpd | Punpcklbw | Punpcklwd | Punpckldq | Punpcklqdq | Punpckhbw
            | Punpckhwd | Punpckhdq | Punpckhqdq => {
                let sz = match m {
                    Punpcklbw | Punpckhbw => 0,
                    Punpcklwd | Punpckhwd => 1,
                    Unpcklps | Unpckhps | Punpckldq | Punpckhdq => 2,
                    _ => 3,
                };
                let hi = matches!(m, Unpckhps | Unpckhpd | Punpckhbw | Punpckhwd | Punpckhdq | Punpckhqdq);
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                self.a.vperm(if hi { 7 } else { 3 }, sz, vd, vd, vs);
            }

            // ---- integer SIMD ----
            Paddb | Paddw | Paddd | Paddq | Psubb | Psubw | Psubd | Psubq | Pmullw | Pcmpeqb | Pcmpeqw | Pcmpeqd
            | Pcmpgtb | Pcmpgtw | Pcmpgtd | Paddsb | Paddsw | Paddusb | Paddusw | Psubsb | Psubsw | Psubusb
            | Psubusw | Pminub | Pmaxub | Pminsw | Pmaxsw | Pavgb | Pavgw => {
                let d = xi(0);
                if reg(1) && xi(1) == d && matches!(m, Psubb | Psubw | Psubd | Psubq | Psubsb | Psubsw | Psubusb | Psubusw) {
                    let vd = self.xdef(d);
                    self.a.vzero(vd);
                } else {
                    let vs = self.xsrc(insn, 1, V1);
                    let vd = self.xmod(d);
                    // (U, size, opcode) of the AdvSIMD "three same" encoding
                    let (u, sz, opc) = match m {
                        Paddb => (0, 0, 0x10),
                        Paddw => (0, 1, 0x10),
                        Paddd => (0, 2, 0x10),
                        Paddq => (0, 3, 0x10),
                        Psubb => (1, 0, 0x10),
                        Psubw => (1, 1, 0x10),
                        Psubd => (1, 2, 0x10),
                        Psubq => (1, 3, 0x10),
                        Pmullw => (0, 1, 0x13),
                        Pcmpeqb => (1, 0, 0x11),
                        Pcmpeqw => (1, 1, 0x11),
                        Pcmpeqd => (1, 2, 0x11),
                        Pcmpgtb => (0, 0, 0x06),
                        Pcmpgtw => (0, 1, 0x06),
                        Pcmpgtd => (0, 2, 0x06),
                        Paddsb => (0, 0, 0x01),
                        Paddsw => (0, 1, 0x01),
                        Paddusb => (1, 0, 0x01),
                        Paddusw => (1, 1, 0x01),
                        Psubsb => (0, 0, 0x05),
                        Psubsw => (0, 1, 0x05),
                        Psubusb => (1, 0, 0x05),
                        Psubusw => (1, 1, 0x05),
                        Pminub => (1, 0, 0x0d),
                        Pmaxub => (1, 0, 0x0c),
                        Pminsw => (0, 1, 0x0d),
                        Pmaxsw => (0, 1, 0x0c),
                        Pavgb => (1, 0, 0x02),
                        _ => (1, 1, 0x02), // Pavgw
                    };
                    self.a.v3(1, u, sz, opc, vd, vd, vs);
                }
            }
            Psllw | Pslld | Psllq | Psrlw | Psrld | Psrlq | Psraw | Psrad if insn.op1_kind() == OpKind::Immediate8 => {
                let sz = match m {
                    Psllw | Psrlw | Psraw => 1,
                    Pslld | Psrld | Psrad => 2,
                    _ => 3,
                };
                let bits = 8u32 << sz;
                let c = insn.immediate8() as u32;
                let d = xi(0);
                let arith = matches!(m, Psraw | Psrad);
                if c == 0 {
                } else if !arith && c >= bits {
                    let vd = self.xdef(d);
                    self.a.vzero(vd);
                } else {
                    let vd = self.xmod(d);
                    match m {
                        Psllw | Pslld | Psllq => self.a.vshl(sz, vd, vd, c),
                        Psrlw | Psrld | Psrlq => self.a.vushr(sz, vd, vd, c),
                        _ => self.a.vsshr(sz, vd, vd, c.min(bits)),
                    }
                }
            }
            Pslldq | Psrldq => {
                let c = insn.immediate8() as u32;
                let d = xi(0);
                if c > 15 {
                    let vd = self.xdef(d);
                    self.a.vzero(vd);
                } else if c != 0 {
                    let vd = self.xmod(d);
                    self.a.vzero(V0);
                    if m == Psrldq {
                        self.a.ext16(vd, vd, V0, c);
                    } else {
                        self.a.ext16(vd, V0, vd, 16 - c);
                    }
                }
            }
            Packssdw | Packsswb | Packuswb => {
                let vs = self.xsrc(insn, 1, V1);
                let vd = self.xmod(xi(0));
                match m {
                    Packssdw => {
                        self.a.sqxtn(false, 1, V0, vd);
                        self.a.sqxtn(true, 1, V0, vs);
                    }
                    Packsswb => {
                        self.a.sqxtn(false, 0, V0, vd);
                        self.a.sqxtn(true, 0, V0, vs);
                    }
                    _ => {
                        self.a.sqxtun(false, 0, V0, vd);
                        self.a.sqxtun(true, 0, V0, vs);
                    }
                }
                self.a.vmov(vd, V0);
            }
            _ => return None,
        }
        Some(true)
    }

    /// Like load(), but reuse T4 when operand 0 already computed the address.
    fn load_keep_ea(&mut self, insn: &Instruction, i: u32, rd: u32) {
        if insn.op_kind(i) == OpKind::Memory {
            let sz = insn.memory_size().size();
            if insn.op0_kind() == OpKind::Memory {
                self.gld(log2(sz), rd, T4);
                return;
            }
            self.ea(insn);
            self.gld(log2(sz), rd, T4);
            return;
        }
        // The address in T4 (operand 0) must survive: load() only touches T4 for memory.
        self.load(insn, i, rd);
    }

    fn store_reg0(&mut self, insn: &Instruction, rs: u32) {
        self.a.mov_w(gr(insn.op0_register()), rs);
    }
}

/// Base condition (as a jcc mnemonic) of a jcc/setcc/cmovcc and whether it is negated.
fn cc_base(m: Mnemonic) -> (Mnemonic, bool) {
    use Mnemonic::*;
    match m {
        Jo | Seto | Cmovo => (Jo, false),
        Jno | Setno | Cmovno => (Jo, true),
        Jb | Setb | Cmovb => (Jb, false),
        Jae | Setae | Cmovae => (Jb, true),
        Je | Sete | Cmove => (Je, false),
        Jne | Setne | Cmovne => (Je, true),
        Jbe | Setbe | Cmovbe => (Jbe, false),
        Ja | Seta | Cmova => (Jbe, true),
        Js | Sets | Cmovs => (Js, false),
        Jns | Setns | Cmovns => (Js, true),
        Jp | Setp | Cmovp => (Jp, false),
        Jnp | Setnp | Cmovnp => (Jp, true),
        Jl | Setl | Cmovl => (Jl, false),
        Jge | Setge | Cmovge => (Jl, true),
        Jle | Setle | Cmovle => (Jle, false),
        _ => (Jle, true), // Jg | Setg | Cmovg
    }
}

fn xidx(r: Register) -> u32 {
    r as u32 - Register::XMM0 as u32
}

/// Conservative: any register operand that is (part of) EBP, or an implicit writer.
fn may_write_ebp(insn: &Instruction) -> bool {
    use Mnemonic::*;
    matches!(insn.mnemonic(), Leave | Enter | Popa | Popad)
        || (0..insn.op_count())
            .any(|i| insn.op_kind(i) == OpKind::Register && insn.op_register(i).full_register32() == Register::EBP)
}

fn is_gpr(r: Register) -> bool {
    interp::gpr_index(r) < 8
}


fn log2(sz: usize) -> u32 {
    match sz {
        1 => 0,
        2 => 1,
        4 => 2,
        _ => 3,
    }
}

fn size_mask(size: u32) -> u32 {
    if size >= 4 { 0xffff_ffff } else { (1 << (size * 8)) - 1 }
}

// --- engine combining JIT and interpreter ---------------------------------------------------------

pub struct JitEngine {
    /// Code for the default MXCSR (0x1f80 control bits): SSE float ops native.
    pub jit: Jit,
    /// Code for any other MXCSR (rounding mode, FTZ, DAZ, unmasked exceptions):
    /// SSE float ops go through the interpreter, data movement/integer SIMD stay native.
    pub jit_nd: Option<Box<Jit>>,
    pub interp: Box<Engine>,
    /// CODE_GEN this engine's translations belong to.
    code_gen: u32,
}

/// MXCSR control bits (exception flags ignored) for which native SSE float code is exact.
pub fn mxcsr_native(mxcsr: u32) -> bool {
    mxcsr & !0x3f == 0x1f80
}

/// Interpreter result of one block for the verifier.
struct Expected {
    eip0: u32,
    insns: Vec<Instruction>,
    st0: CpuState,
    st: CpuState,
    written: Vec<(u32, u8)>,
}

#[inline(never)]
#[cold]
fn verify_compare(ctx: &mut JitCtx, e: &Expected) {
    ctx.materialize();
    let st = unsafe { &*ctx.state };
    let mut diffs = Vec::new();
    const NAMES: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];
    for i in 0..8 {
        if ctx.gpr[i] != e.st.gpr[i] {
            diffs.push(format!("{} jit={:#x} interp={:#x}", NAMES[i], ctx.gpr[i], e.st.gpr[i]));
        }
    }
    if ctx.eip != e.st.eip {
        diffs.push(format!("eip jit={:#x} interp={:#x}", ctx.eip, e.st.eip));
    }
    // Flags are not compared: translated blocks drop flags that are dead at the
    // exit, and a wrong flag inside the block shows up as a wrong eip.
    for i in 0..8 {
        if st.xmm[i] != e.st.xmm[i] {
            diffs.push(format!("xmm{} jit={:02x?} interp={:02x?}", i, st.xmm[i], e.st.xmm[i]));
        }
    }
    let mut bad_mem = 0;
    for &(a, v) in &e.written {
        let now = mem::r8(a);
        if now != v {
            if bad_mem < 8 {
                diffs.push(format!("mem[{:#x}] jit={:#04x} interp={:#04x}", a, now, v));
            }
            bad_mem += 1;
        }
    }
    if diffs.is_empty() {
        return;
    }
    static REPORTED: std::sync::Mutex<Option<std::collections::HashSet<u32>>> = std::sync::Mutex::new(None);
    {
        let mut r = REPORTED.lock().unwrap();
        let set = r.get_or_insert_with(Default::default);
        if set.len() >= 64 || !set.insert(e.eip0) {
            return;
        }
    }
    let mut fmt = iced_x86::IntelFormatter::new();
    use iced_x86::Formatter;
    let mut out = format!("dxcpu verify: block {:#x} differs ({} bad bytes): {}\n", e.eip0, bad_mem, diffs.join(", "));
    out += &format!(
        "  in: eax={:#x} ecx={:#x} edx={:#x} ebx={:#x} esp={:#x} ebp={:#x} esi={:#x} edi={:#x} eflags={:#x}\n",
        e.st0.gpr[0], e.st0.gpr[1], e.st0.gpr[2], e.st0.gpr[3], e.st0.gpr[4], e.st0.gpr[5], e.st0.gpr[6], e.st0.gpr[7], e.st0.eflags
    );
    for insn in &e.insns {
        let mut t = String::new();
        fmt.format(insn, &mut t);
        out += &format!("  {:08x}  {}\n", insn.ip32(), t);
    }
    eprint!("{}", out);
}

impl JitEngine {
    /// Run the block at ctx.eip in the interpreter on a copy of the state,
    /// then undo its memory writes (see verify_on).
    // Out of line: inlined, its thread-local accesses (mem.rs undo log) and
    // stack setup ran on every JitEngine::run, i.e. every thunk call.
    #[inline(never)]
    #[cold]
    fn verify_interp(&mut self, ctx: &mut JitCtx) -> Option<Expected> {
        ctx.materialize();
        let mut st = unsafe { *ctx.state };
        st.gpr = ctx.gpr;
        st.eip = ctx.eip;
        st.eflags = ctx.eflags;
        let st0 = st;
        let insns = self.jit.decode(ctx.eip);
        // Running a locked instruction twice (interpreter, then translated) would let
        // other threads see and lose the interpreter's temporary value.
        if insns.iter().any(|i| {
            i.has_lock_prefix() || (i.mnemonic() == Mnemonic::Xchg && i.op_count() == 2 && (i.op0_kind() == OpKind::Memory || i.op1_kind() == OpKind::Memory))
        }) {
            return None;
        }
        mem::undo_begin();
        let mut ok = true;
        for insn in &insns {
            match interp::exec(&mut self.interp, &mut st, insn) {
                Ok(Step::Next) => {
                    st.eip = insn.next_ip32();
                    if matches!(insn.mnemonic(), Mnemonic::Ldmxcsr | Mnemonic::Fxrstor | Mnemonic::Xrstor) {
                        break;
                    }
                }
                Ok(Step::Jump) => break,
                Err(_) => {
                    ok = false;
                    break;
                }
            }
        }
        let written = mem::undo_rollback();
        if !ok {
            return None;
        }
        Some(Expected { eip0: st0.eip, insns, st0, st, written })
    }

    pub fn new(stub_lo: u32, stub_hi: u32) -> Self {
        JitEngine {
            jit: Jit::new(stub_lo, stub_hi),
            jit_nd: None,
            interp: Box::new(Engine::new(stub_lo, stub_hi)),
            code_gen: CODE_GEN.load(std::sync::atomic::Ordering::Acquire),
        }
    }

    pub fn run(&mut self, st: &mut CpuState, stop: u32) -> Exit {
        let mut ctx = JitCtx::new(st as *mut CpuState, &mut *self.interp as *mut Engine);
        ctx.gpr = st.gpr;
        ctx.eip = st.eip;
        ctx.eflags = st.eflags;
        let exit = loop {
            if ctx.eip == stop {
                break Exit::Stop;
            }
            if ctx.eip >= self.jit.stub_lo && ctx.eip < self.jit.stub_hi {
                break Exit::Stub;
            }
            if STATS_ON.load(std::sync::atomic::Ordering::Relaxed) {
                DISPATCH_COUNT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            }
            // Guest code changed somewhere we translated: start over (no block of
            // this engine is executing while its dispatcher runs).
            let g = CODE_GEN.load(std::sync::atomic::Ordering::Acquire);
            if g != self.code_gen {
                let (lo, hi) = (self.jit.stub_lo, self.jit.stub_hi);
                self.jit = Jit::new(lo, hi);
                self.jit_nd = None;
                self.interp.flush();
                self.code_gen = g;
            }
            // MXCSR only changes through interpreted instructions that end their block
            // (ldmxcsr, fxrstor) or host code between runs, so checking here suffices.
            let native = mxcsr_native(unsafe { (*ctx.state).mxcsr });
            let expect = if verify_on() { self.verify_interp(&mut ctx) } else { None };
            let r = if native {
                self.jit.run_once(&mut ctx)
            } else {
                let (lo, hi) = (self.jit.stub_lo, self.jit.stub_hi);
                self.jit_nd.get_or_insert_with(|| Box::new(Jit::with_mode(lo, hi, false))).run_once(&mut ctx)
            };
            if let Some(e) = expect {
                if r != EXIT_FAULT {
                    verify_compare(&mut ctx, &e);
                }
            }
            if r == EXIT_FAULT {
                break unsafe { std::mem::transmute::<u32, Exit>(ctx.exit_reason) };
            }
        };
        // Exit::Stub is a call into the host (or a return to it): the arithmetic
        // flags are not preserved across calls and no thunk reads them, so the
        // lazy descriptor is dropped instead of materialised (once per thunk call).
        if exit != Exit::Stub {
            ctx.materialize();
        }
        st.gpr = ctx.gpr;
        st.eip = ctx.eip;
        st.eflags = ctx.eflags;
        if exit != Exit::Stop && exit != Exit::Stub {
            st.fault_eip = ctx.eip;
        }
        exit
    }
}
