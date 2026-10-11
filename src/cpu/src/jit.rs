//! i386 → AArch64 block translator (see docs/JIT.md).
//!
//! Hot integer instructions are translated natively; everything else runs
//! through the interpreter one instruction at a time (`helper_interp`).

use crate::a64::*;
use crate::codebuf::CodeBuf;
use crate::interp::{self, Engine, Step};
use crate::mem;
use crate::state::flags::*;
use crate::state::*;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Instruction, Mnemonic, OpKind, Register};
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
// Lazy flag operands within a block.
const FA: u32 = 9;
const FB: u32 = 10;
const FR: u32 = 11;
const FC: u32 = 12; // carry in for inc/dec

pub const K_MAT: u32 = 0;
pub const K_ADD: u32 = 1;
pub const K_SUB: u32 = 2;
pub const K_LOGIC: u32 = 3;
pub const K_INC: u32 = 4;
pub const K_DEC: u32 = 5;

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
extern "C" fn helper_interp(ctx: *mut JitCtx, insn: *const Instruction) -> u32 {
    let ctx = unsafe { &mut *ctx };
    let insn = unsafe { &*insn };
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
}

const BUF_SIZE: usize = 64 << 20;

impl Jit {
    pub fn new(stub_lo: u32, stub_hi: u32) -> Self {
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
        }
    }

    fn decode(&self, eip: u32) -> Vec<Instruction> {
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
        let code = match self.blocks.get(&ctx.eip) {
            Some(&c) => c,
            None => self.translate(ctx.eip),
        };
        (self.enter)(ctx as *mut JitCtx, code)
    }

    fn translate(&mut self, eip: u32) -> *const u8 {
        if self.buf.remaining() < 1 << 20 {
            // Out of code space: start over (all chains are dropped with the blocks).
            let (lo, hi) = (self.stub_lo, self.stub_hi);
            *self = Jit::new(lo, hi);
        }
        let insns = self.decode(eip);
        let start = self.buf.next_addr();
        let mut t = Translator::new(start, self.exit_addr);
        for (i, insn) in insns.iter().enumerate() {
            let live = flags_live_after(&insns, i);
            let cont = t.insn(insn, live);
            if !cont {
                break;
            }
            if i + 1 == insns.len() {
                t.flush_flags();
                t.exit_direct(insn.next_ip32());
            }
        }
        let code = std::mem::take(&mut t.a.code);
        let p = self.buf.add(&code).expect("jit buffer");
        debug_assert_eq!(p as usize, start);
        self.keep.extend(t.keep.drain(..));
        for (pos, target) in t.exits {
            let at = unsafe { (p as *mut u32).add(pos) };
            if target >= self.stub_lo && target < self.stub_hi {
                continue; // stubs always go through the dispatcher
            }
            if let Some(&tp) = self.blocks.get(&target) {
                self.link(at, tp);
            } else {
                self.unlinked.entry(target).or_default().push(at);
            }
        }
        self.blocks.insert(eip, p);
        self.translated += 1;
        if let Some(waiters) = self.unlinked.remove(&eip) {
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
            return true;
        }
    }
    true
}

// --- per-block translator ----------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
struct FlagSrc {
    kind: u32,
    size: u32,
}

struct Translator {
    a: Asm,
    start: usize,
    exit_addr: usize,
    /// (position of patchable branch word, target eip)
    exits: Vec<(usize, u32)>,
    keep: Vec<Box<Instruction>>,
    /// Host NZCV currently reflects this x86 flag producer (FA/FB/FR/FC hold its operands).
    nzcv: Option<FlagSrc>,
    /// The lazy descriptor in registers must be stored to ctx at exits.
    dirty: bool,
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
    fn new(start: usize, exit_addr: usize) -> Self {
        Translator { a: Asm::new(), start, exit_addr, exits: Vec::new(), keep: Vec::new(), nzcv: None, dirty: false }
    }

    fn b_exit(&mut self) {
        let here = self.start + self.a.pos() * 4;
        let off = (self.exit_addr as isize - here as isize) / 4;
        self.a.emit(0x1400_0000 | ((off as u32) & 0x03ff_ffff));
    }

    /// Store the lazy flag descriptor (if flags were produced in this block).
    fn flush_flags(&mut self) {
        if let (true, Some(f)) = (self.dirty, self.nzcv) {
            self.a.mov_w_imm(T0, f.kind);
            self.a.mov_w_imm(T1, f.size);
            self.a.stp_w(T0, T1, CTX, OFF_FKIND);
            self.a.stp_w(FA, FB, CTX, OFF_FA);
            self.a.stp_w(FR, FC, CTX, OFF_FRES);
            self.dirty = false;
        }
    }

    /// Exit to a known guest eip: a patchable branch (initially to the next
    /// instruction) followed by the slow path into the dispatcher.
    fn exit_direct(&mut self, target: u32) {
        let pos = self.a.pos();
        self.a.emit(0x1400_0001); // b +1 (patched to the target block later)
        self.exits.push((pos, target));
        self.a.mov_w_imm(T0, target);
        self.a.str_imm(2, T0, CTX, OFF_EIP);
        self.a.mov_w_imm(0, EXIT_DISPATCH);
        self.b_exit();
    }

    /// Exit with the guest eip in register `r`.
    fn exit_indirect(&mut self, r: u32) {
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
        let mut have = false;
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
                self.a.ldr_uxtw(log2(sz), rd, BASE, T4);
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
                self.a.str_uxtw(log2(sz), rs, BASE, T4);
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
            K_SUB => match m {
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
            K_ADD => match m {
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
    fn cond_generic(&mut self, m: Mnemonic) {
        self.flush_flags();
        self.a.mov_x(0, CTX);
        self.a.mov_w_imm(1, m as u32);
        self.a.ldr_imm(3, 16, CTX, OFF_HELPER_COND);
        self.a.blr(16);
        self.a.cmp_w(0, XZR);
        // After the helper the descriptor in ctx is materialised; register flags are gone.
        self.nzcv = None;
    }

    /// Returns a host condition that is true iff the x86 condition holds.
    fn cond(&mut self, m: Mnemonic) -> Cond {
        if let Some(c) = self.host_cond(m) {
            return c;
        }
        self.cond_generic(m);
        Cond::Ne
    }

    /// Current CF value into FC (for inc/dec, which preserve CF).
    fn carry_into_fc(&mut self) {
        match self.nzcv {
            Some(f) if f.kind == K_SUB => self.a.cset_w(FC, Cond::Lo),
            Some(f) if f.kind == K_ADD => self.a.cset_w(FC, Cond::Hs),
            Some(f) if f.kind == K_LOGIC => self.a.mov_w_imm(FC, 0),
            Some(f) if f.kind == K_INC || f.kind == K_DEC => {} // FC already holds CF
            _ => {
                self.flush_flags();
                self.a.mov_x(0, CTX);
                self.a.ldr_imm(3, 16, CTX, OFF_HELPER_FLAGS);
                self.a.blr(16);
                self.a.ldr_imm(2, FC, CTX, OFF_EFLAGS);
                self.a.ubfx_w(FC, FC, 0, 1);
            }
        }
    }

    // --- fallback ------------------------------------------------------------------------------

    fn fallback(&mut self, insn: &Instruction) -> bool {
        let boxed = Box::new(*insn);
        let ptr = &*boxed as *const Instruction as u64;
        self.keep.push(boxed);
        self.flush_flags();
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
        if insn.flow_control() != FlowControl::Next {
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
        use Mnemonic::*;
        let m = insn.mnemonic();
        let size = if insn.op_count() > 0 { interp::op_size(insn, 0) } else { 4 };
        // Instructions with prefixes we do not model natively go to the interpreter.
        if insn.has_lock_prefix() || insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix()
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
        match m {
            Nop => {}
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
            Seto | Setno | Setb | Setae | Sete | Setne | Setbe | Seta | Sets | Setns | Setl | Setge | Setle | Setg => {
                let c = self.cond(m);
                self.a.cset_w(T0, c);
                self.store(insn, T0, false);
            }
            Cmovo | Cmovno | Cmovb | Cmovae | Cmove | Cmovne | Cmovbe | Cmova | Cmovs | Cmovns | Cmovl | Cmovge
            | Cmovle | Cmovg if size == 4 => {
                // Condition first: the generic path calls a helper that clobbers x0-x17.
                let c = self.cond(m);
                self.load(insn, 1, T1);
                let dst = gr(insn.op0_register());
                self.a.csel_w(dst, T1, dst, c);
            }
            Jo | Jno | Jb | Jae | Je | Jne | Jbe | Ja | Js | Jns | Jl | Jge | Jle | Jg => {
                let c = self.cond(m);
                let taken = self.a.bcond_fixup(c);
                self.flush_flags_both();
                self.exit_direct(insn.next_ip32());
                self.a.bind(taken);
                self.flush_flags_both();
                self.exit_direct(insn.near_branch32());
                return false;
            }
            Jmp if insn.op0_kind() == OpKind::NearBranch32 => {
                self.flush_flags();
                self.exit_direct(insn.near_branch32());
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

    /// Flag descriptor must be valid on both sides of a conditional branch.
    fn flush_flags_both(&mut self) {
        let saved = (self.nzcv, self.dirty);
        self.flush_flags();
        self.nzcv = saved.0;
        self.dirty = saved.1;
    }

    /// Like load(), but reuse T4 when operand 0 already computed the address.
    fn load_keep_ea(&mut self, insn: &Instruction, i: u32, rd: u32) {
        if insn.op_kind(i) == OpKind::Memory {
            let sz = insn.memory_size().size();
            if insn.op0_kind() == OpKind::Memory {
                self.a.ldr_uxtw(log2(sz), rd, BASE, T4);
                return;
            }
            self.ea(insn);
            self.a.ldr_uxtw(log2(sz), rd, BASE, T4);
            return;
        }
        // The address in T4 (operand 0) must survive: load() only touches T4 for memory.
        self.load(insn, i, rd);
    }

    fn store_reg0(&mut self, insn: &Instruction, rs: u32) {
        self.a.mov_w(gr(insn.op0_register()), rs);
    }
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
    pub jit: Jit,
    pub interp: Box<Engine>,
}

impl JitEngine {
    pub fn new(stub_lo: u32, stub_hi: u32) -> Self {
        JitEngine { jit: Jit::new(stub_lo, stub_hi), interp: Box::new(Engine::new(stub_lo, stub_hi)) }
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
            let r = self.jit.run_once(&mut ctx);
            if r == EXIT_FAULT {
                break unsafe { std::mem::transmute::<u32, Exit>(ctx.exit_reason) };
            }
        };
        ctx.materialize();
        st.gpr = ctx.gpr;
        st.eip = ctx.eip;
        st.eflags = ctx.eflags;
        if exit != Exit::Stop && exit != Exit::Stub {
            st.fault_eip = ctx.eip;
        }
        exit
    }
}
