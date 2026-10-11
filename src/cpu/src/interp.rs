//! Reference interpreter for i386 user-mode code (integer core; x87 in fpu.rs,
//! SSE/MMX in sse.rs).

use crate::mem;
use crate::state::flags::*;
use crate::state::*;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Instruction, Mnemonic, OpKind, Register};
use std::collections::HashMap;
use std::rc::Rc;

pub struct Block {
    pub insns: Vec<Instruction>,
}

/// Per-thread engine: decoded block cache plus configuration.
pub struct Engine {
    blocks: HashMap<u32, Rc<Block>>,
    code_gen: u32, // jit::CODE_GEN of the cached blocks
    pub stub_lo: u32,
    pub stub_hi: u32,
    pub tsc: u64,
}

pub enum Step {
    Next,
    Jump,
}

pub type ExecResult = Result<Step, Exit>;

const PAGE: u32 = 0x4000;
const MAX_BLOCK: usize = 64;

impl Engine {
    pub fn new(stub_lo: u32, stub_hi: u32) -> Self {
        Engine { blocks: HashMap::new(), code_gen: crate::jit::CODE_GEN.load(std::sync::atomic::Ordering::Acquire), stub_lo, stub_hi, tsc: 0 }
    }

    pub fn flush(&mut self) {
        self.blocks.clear();
    }

    fn decode_block(&self, eip: u32) -> Block {
        let mut insns = Vec::new();
        let mut ip = eip;
        while insns.len() < MAX_BLOCK {
            // Read up to the end of the current page, plus a 15-byte overlap only
            // when an instruction straddles the page boundary.
            let room = PAGE - (ip % PAGE);
            let mut len = room.min(64) as usize;
            let mut insn;
            loop {
                let bytes = unsafe { std::slice::from_raw_parts(mem::ptr(ip), len) };
                let mut dec = Decoder::with_ip(32, bytes, ip as u64, DecoderOptions::NONE);
                insn = dec.decode();
                if !insn.is_invalid() || len >= 32 {
                    break;
                }
                len += 16; // instruction crosses the page end
            }
            insns.push(insn);
            ip = insn.next_ip32();
            if insn.is_invalid() || insn.flow_control() != FlowControl::Next {
                break;
            }
        }
        if let Some(last) = insns.last() {
            crate::jit::mark_code(eip, last.next_ip32());
        }
        Block { insns }
    }

    fn block(&mut self, eip: u32) -> Rc<Block> {
        if let Some(b) = self.blocks.get(&eip) {
            return b.clone();
        }
        let b = Rc::new(self.decode_block(eip));
        self.blocks.insert(eip, b.clone());
        b
    }

    /// Run until eip == stop, eip enters the stub range, or an exit occurs.
    pub fn run(&mut self, st: &mut CpuState, stop: u32) -> Exit {
        let watch = watch_addr();
        let mut watched = if watch != 0 { mem::r32(watch) } else { 0 };
        loop {
            let eip = st.eip;
            if eip == stop {
                return Exit::Stop;
            }
            if eip >= self.stub_lo && eip < self.stub_hi {
                return Exit::Stub;
            }
            let g = crate::jit::CODE_GEN.load(std::sync::atomic::Ordering::Acquire);
            if g != self.code_gen {
                self.blocks.clear();
                self.code_gen = g;
            }
            let b = self.block(eip);
            for insn in b.insns.iter() {
                st.icount += 1;
                let r = exec(self, st, insn);
                if watch != 0 && mem::r32(watch) != watched {
                    eprintln!(
                        "dxcpu watch: [{:#x}] {:#010x} -> {:#010x} by instruction at {:#x} (esp={:#x} eax={:#x} ecx={:#x} edx={:#x} ebx={:#x} esi={:#x} edi={:#x})",
                        watch, watched, mem::r32(watch), insn.ip32(), st.gpr[ESP], st.gpr[EAX], st.gpr[ECX], st.gpr[EDX], st.gpr[EBX], st.gpr[ESI], st.gpr[EDI]
                    );
                    let sp = st.gpr[ESP];
                    eprintln!("dxcpu watch: stack {:08x?}", (0..12).map(|i| mem::r32(sp.wrapping_add(4 * i))).collect::<Vec<_>>());
                    let mut bp = st.gpr[EBP];
                    let mut chain = Vec::new();
                    for _ in 0..24 {
                        if bp < sp || bp == 0 || bp > sp.wrapping_add(0x10_0000) {
                            break;
                        }
                        chain.push(format!("{:#x}@{:#x}", mem::r32(bp + 4), bp));
                        bp = mem::r32(bp);
                    }
                    eprintln!("dxcpu watch: frames {}", chain.join(" <- "));
                    watched = mem::r32(watch);
                }
                match r {
                    Ok(Step::Next) => {
                        st.eip = insn.next_ip32();
                        if st.eip == stop {
                            return Exit::Stop;
                        }
                    }
                    Ok(Step::Jump) => break,
                    Err(e) => {
                        st.fault_eip = insn.ip32();
                        st.eip = insn.ip32();
                        st.exit_reason = e as u32;
                        return e;
                    }
                }
            }
        }
    }
}

/// Debug watchpoint (DXCPU_WATCH=0xaddr): report guest instructions that change
/// the 32-bit word at that address.
fn watch_addr() -> u32 {
    static W: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *W.get_or_init(|| {
        std::env::var("DXCPU_WATCH")
            .ok()
            .and_then(|v| u32::from_str_radix(v.trim_start_matches("0x"), 16).ok())
            .unwrap_or(0)
    })
}

// --- registers and operands ------------------------------------------------------------

#[inline]
pub fn gpr_index(r: Register) -> usize {
    (match r {
        Register::EAX | Register::AX | Register::AL => 0,
        Register::ECX | Register::CX | Register::CL => 1,
        Register::EDX | Register::DX | Register::DL => 2,
        Register::EBX | Register::BX | Register::BL => 3,
        Register::ESP | Register::SP | Register::AH => 4,
        Register::EBP | Register::BP | Register::CH => 5,
        Register::ESI | Register::SI | Register::DH => 6,
        Register::EDI | Register::DI | Register::BH => 7,
        _ => 8,
    }) as usize
}

pub fn read_reg(st: &CpuState, r: Register) -> u32 {
    match r {
        Register::EAX | Register::ECX | Register::EDX | Register::EBX | Register::ESP | Register::EBP | Register::ESI
        | Register::EDI => st.gpr[gpr_index(r)],
        Register::AX | Register::CX | Register::DX | Register::BX | Register::SP | Register::BP | Register::SI
        | Register::DI => st.gpr[gpr_index(r)] & 0xffff,
        Register::AL | Register::CL | Register::DL | Register::BL => st.gpr[gpr_index(r)] & 0xff,
        Register::AH | Register::CH | Register::DH | Register::BH => (st.gpr[gpr_index(r) - 4] >> 8) & 0xff,
        Register::CS => 0x1b,
        Register::SS | Register::DS | Register::ES => 0x23,
        Register::FS | Register::GS => 0,
        _ => 0,
    }
}

pub fn write_reg(st: &mut CpuState, r: Register, v: u32) {
    match r {
        Register::EAX | Register::ECX | Register::EDX | Register::EBX | Register::ESP | Register::EBP | Register::ESI
        | Register::EDI => st.gpr[gpr_index(r)] = v,
        Register::AX | Register::CX | Register::DX | Register::BX | Register::SP | Register::BP | Register::SI
        | Register::DI => {
            let i = gpr_index(r);
            st.gpr[i] = (st.gpr[i] & 0xffff_0000) | (v & 0xffff);
        }
        Register::AL | Register::CL | Register::DL | Register::BL => {
            let i = gpr_index(r);
            st.gpr[i] = (st.gpr[i] & 0xffff_ff00) | (v & 0xff);
        }
        Register::AH | Register::CH | Register::DH | Register::BH => {
            let i = gpr_index(r) - 4;
            st.gpr[i] = (st.gpr[i] & 0xffff_00ff) | ((v & 0xff) << 8);
        }
        _ => {} // segment register writes are ignored (flat model)
    }
}

/// Effective address of the memory operand.
#[inline]
pub fn ea(st: &CpuState, insn: &Instruction) -> u32 {
    let mut a = insn.memory_displacement32();
    let base = insn.memory_base();
    if base != Register::None {
        a = a.wrapping_add(read_reg(st, base));
    }
    let index = insn.memory_index();
    if index != Register::None {
        a = a.wrapping_add(read_reg(st, index).wrapping_mul(insn.memory_index_scale()));
    }
    a
}

pub fn op_size(insn: &Instruction, i: u32) -> u32 {
    match insn.op_kind(i) {
        OpKind::Register => insn.op_register(i).size() as u32,
        OpKind::Memory => insn.memory_size().size() as u32,
        OpKind::Immediate8 => 1,
        OpKind::Immediate16 => 2,
        _ => {
            // immediates take the size of the destination
            if i > 0 { op_size(insn, 0) } else { 4 }
        }
    }
}

pub fn read_op(st: &CpuState, insn: &Instruction, i: u32) -> u32 {
    match insn.op_kind(i) {
        OpKind::Register => read_reg(st, insn.op_register(i)),
        OpKind::Memory => mem::rd(ea(st, insn), insn.memory_size().size() as u32),
        _ => insn.immediate(i) as u32,
    }
}

pub fn write_op(st: &mut CpuState, insn: &Instruction, i: u32, v: u32) {
    match insn.op_kind(i) {
        OpKind::Register => write_reg(st, insn.op_register(i), v),
        OpKind::Memory => mem::wr(ea(st, insn), insn.memory_size().size() as u32, v),
        _ => {}
    }
}

#[inline]
fn mask(size: u32) -> u32 {
    if size >= 4 { 0xffff_ffff } else { (1u32 << (size * 8)) - 1 }
}
#[inline]
fn sign_bit(size: u32) -> u32 {
    1u32 << (size * 8 - 1)
}

pub fn push32(st: &mut CpuState, v: u32) {
    st.gpr[ESP] = st.gpr[ESP].wrapping_sub(4);
    mem::w32(st.gpr[ESP], v);
}
pub fn pop32(st: &mut CpuState) -> u32 {
    let v = mem::r32(st.gpr[ESP]);
    st.gpr[ESP] = st.gpr[ESP].wrapping_add(4);
    v
}

// --- flags --------------------------------------------------------------------------------

#[inline]
fn parity(v: u32) -> u32 {
    if (v as u8).count_ones() % 2 == 0 { PF } else { 0 }
}

#[inline]
fn szp(size: u32, r: u32) -> u32 {
    let r = r & mask(size);
    (if r == 0 { ZF } else { 0 }) | (if r & sign_bit(size) != 0 { SF } else { 0 }) | parity(r)
}

#[inline]
fn set_flags(st: &mut CpuState, which: u32, values: u32) {
    st.eflags = (st.eflags & !which) | (values & which);
}

pub fn add_flags(size: u32, a: u32, b: u32, c: u32) -> (u32, u32) {
    let m = mask(size);
    let (a, b) = (a & m, b & m);
    let wide = a as u64 + b as u64 + c as u64;
    let r = (wide as u32) & m;
    let mut f = szp(size, r);
    if wide > m as u64 {
        f |= CF;
    }
    if ((a ^ r) & (b ^ r)) & sign_bit(size) != 0 {
        f |= OF;
    }
    if (a ^ b ^ r) & 0x10 != 0 {
        f |= AF;
    }
    (r, f)
}

pub fn sub_flags(size: u32, a: u32, b: u32, c: u32) -> (u32, u32) {
    let m = mask(size);
    let (a, b) = (a & m, b & m);
    let r = a.wrapping_sub(b).wrapping_sub(c) & m;
    let mut f = szp(size, r);
    if (b as u64 + c as u64) > a as u64 {
        f |= CF;
    }
    if ((a ^ b) & (a ^ r)) & sign_bit(size) != 0 {
        f |= OF;
    }
    if (a ^ b ^ r) & 0x10 != 0 {
        f |= AF;
    }
    (r, f)
}

pub fn logic_flags(size: u32, r: u32) -> u32 {
    szp(size, r)
}

pub fn cond(st: &CpuState, m: Mnemonic) -> bool {
    let f = st.eflags;
    let cf = f & CF != 0;
    let zf = f & ZF != 0;
    let sf = f & SF != 0;
    let of = f & OF != 0;
    let pf = f & PF != 0;
    use Mnemonic::*;
    match m {
        Jo | Seto | Cmovo => of,
        Jno | Setno | Cmovno => !of,
        Jb | Setb | Cmovb => cf,
        Jae | Setae | Cmovae => !cf,
        Je | Sete | Cmove => zf,
        Jne | Setne | Cmovne => !zf,
        Jbe | Setbe | Cmovbe => cf || zf,
        Ja | Seta | Cmova => !cf && !zf,
        Js | Sets | Cmovs => sf,
        Jns | Setns | Cmovns => !sf,
        Jp | Setp | Cmovp => pf,
        Jnp | Setnp | Cmovnp => !pf,
        Jl | Setl | Cmovl => sf != of,
        Jge | Setge | Cmovge => sf == of,
        Jle | Setle | Cmovle => zf || sf != of,
        Jg | Setg | Cmovg => !zf && sf == of,
        _ => false,
    }
}

// --- instruction semantics -----------------------------------------------------------------------

fn branch_target(insn: &Instruction) -> u32 {
    insn.near_branch32()
}

/// Read-modify-write on operand 0, atomically if it is a locked memory operand.
fn rmw(st: &mut CpuState, insn: &Instruction, f: &mut dyn FnMut(u32) -> u32) -> u32 {
    if insn.op0_kind() == OpKind::Memory {
        let a = ea(st, insn);
        let size = insn.memory_size().size() as u32;
        if insn.has_lock_prefix() || insn.mnemonic() == Mnemonic::Xchg {
            return mem::atomic_rmw(a, size, f);
        }
        let old = mem::rd(a, size);
        mem::wr(a, size, f(old));
        old
    } else {
        let r = insn.op0_register();
        let old = read_reg(st, r);
        let new = f(old);
        write_reg(st, r, new);
        old
    }
}

fn alu(st: &mut CpuState, insn: &Instruction) {
    let size = op_size(insn, 0);
    let b = read_op(st, insn, 1);
    let carry = st.eflags & CF;
    let m = insn.mnemonic();
    let mut flags = 0;
    let store = !matches!(m, Mnemonic::Cmp | Mnemonic::Test);
    let mut compute = |a: u32| -> u32 {
        let (r, f) = match m {
            Mnemonic::Add => add_flags(size, a, b, 0),
            Mnemonic::Adc => add_flags(size, a, b, carry),
            Mnemonic::Sub | Mnemonic::Cmp => sub_flags(size, a, b, 0),
            Mnemonic::Sbb => sub_flags(size, a, b, carry),
            Mnemonic::And | Mnemonic::Test => {
                let r = a & b & mask(size);
                (r, logic_flags(size, r))
            }
            Mnemonic::Or => {
                let r = (a | b) & mask(size);
                (r, logic_flags(size, r))
            }
            Mnemonic::Xor => {
                let r = (a ^ b) & mask(size);
                (r, logic_flags(size, r))
            }
            _ => unreachable!(),
        };
        flags = f;
        if store { r } else { a }
    };
    if store {
        rmw(st, insn, &mut compute);
    } else {
        let a = read_op(st, insn, 0);
        compute(a);
    }
    set_flags(st, ARITH, flags);
}

fn shift(st: &mut CpuState, insn: &Instruction) -> ExecResult {
    let size = op_size(insn, 0);
    let bits = size * 8;
    let count = if insn.op_count() > 1 { read_op(st, insn, 1) & 0x1f } else { 1 };
    if count == 0 {
        return Ok(Step::Next);
    }
    let m = mask(size);
    let a = read_op(st, insn, 0) & m;
    let cf_in = st.eflags & CF != 0;
    let (r, cf, of): (u32, bool, bool);
    match insn.mnemonic() {
        Mnemonic::Shl | Mnemonic::Sal => {
            let wide = (a as u64) << count;
            r = (wide as u32) & m;
            cf = (wide >> bits) & 1 != 0;
            of = ((r & sign_bit(size)) != 0) != cf;
        }
        Mnemonic::Shr => {
            r = ((a as u64) >> count) as u32 & m;
            cf = ((a as u64) >> (count - 1)) & 1 != 0;
            of = a & sign_bit(size) != 0;
        }
        Mnemonic::Sar => {
            let sa = ((a << (32 - bits)) as i32) >> (32 - bits); // sign-extend
            r = (((sa as i64) >> count.min(31)) as u32) & m;
            cf = (((sa as i64) >> (count - 1).min(31)) & 1) != 0;
            of = false;
        }
        Mnemonic::Rol => {
            let c = count % bits;
            r = if c == 0 { a } else { ((a << c) | (a >> (bits - c))) & m };
            cf = r & 1 != 0;
            of = ((r & sign_bit(size)) != 0) != cf;
            let f = (if cf { CF } else { 0 }) | (if of { OF } else { 0 });
            write_op(st, insn, 0, r);
            set_flags(st, CF | OF, f);
            return Ok(Step::Next);
        }
        Mnemonic::Ror => {
            let c = count % bits;
            r = if c == 0 { a } else { ((a >> c) | (a << (bits - c))) & m };
            cf = r & sign_bit(size) != 0;
            of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1 != 0;
            let f = (if cf { CF } else { 0 }) | (if of { OF } else { 0 });
            write_op(st, insn, 0, r);
            set_flags(st, CF | OF, f);
            return Ok(Step::Next);
        }
        Mnemonic::Rcl | Mnemonic::Rcr => {
            let c = count % (bits + 1);
            let mut v = a as u64 | ((cf_in as u64) << bits); // bits+1 wide
            let w = bits + 1;
            let wm = (1u64 << w) - 1;
            if c != 0 {
                v = if insn.mnemonic() == Mnemonic::Rcl {
                    ((v << c) | (v >> (w - c))) & wm
                } else {
                    ((v >> c) | (v << (w - c))) & wm
                };
            }
            let res = (v as u32) & m;
            let cfo = (v >> bits) & 1 != 0;
            let ofo = if insn.mnemonic() == Mnemonic::Rcl {
                ((res & sign_bit(size)) != 0) != cfo
            } else {
                ((res >> (bits - 1)) ^ (res >> (bits - 2))) & 1 != 0
            };
            write_op(st, insn, 0, res);
            set_flags(st, CF | OF, (if cfo { CF } else { 0 }) | (if ofo { OF } else { 0 }));
            return Ok(Step::Next);
        }
        _ => return Err(Exit::Unimplemented),
    }
    write_op(st, insn, 0, r);
    // AF is undefined after shifts; 0 (the JIT's lazy flags cannot keep the old AF)
    let f = szp(size, r) | (if cf { CF } else { 0 }) | (if of { OF } else { 0 });
    set_flags(st, ARITH, f);
    Ok(Step::Next)
}

fn shld_shrd(st: &mut CpuState, insn: &Instruction) {
    let size = op_size(insn, 0);
    let bits = size * 8;
    let count = read_op(st, insn, 2) & 0x1f;
    if count == 0 {
        return;
    }
    let a = read_op(st, insn, 0) as u64;
    let b = read_op(st, insn, 1) as u64;
    let m = mask(size) as u64;
    let (r, cf) = if insn.mnemonic() == Mnemonic::Shld {
        let wide = (a << bits) | b;
        let res = (wide << count) >> bits;
        (res & m, (a >> (bits - count)) & 1 != 0)
    } else {
        let wide = (b << bits) | a;
        let res = wide >> count;
        (res & m, (a >> (count - 1)) & 1 != 0)
    };
    let r = r as u32;
    write_op(st, insn, 0, r);
    let of = ((r ^ a as u32) & sign_bit(size)) != 0;
    set_flags(st, ARITH, szp(size, r) | (if cf { CF } else { 0 }) | (if of { OF } else { 0 })); // AF undefined: 0
}

fn bit_op(st: &mut CpuState, insn: &Instruction) {
    let size = op_size(insn, 0);
    let bitoff = read_op(st, insn, 1);
    let (addr_bits, bit) = if insn.op0_kind() == OpKind::Memory && insn.op1_kind() == OpKind::Register {
        // register bit offset may address outside the operand (signed)
        let off = bitoff as i32;
        let base = ea(st, insn) as i64 + ((off >> 3) as i64 & !((size as i64) - 1));
        (Some(base as u32), (off & (size as i32 * 8 - 1)) as u32)
    } else {
        (None, bitoff & (size * 8 - 1))
    };
    let read = |st: &CpuState| -> u32 {
        match addr_bits {
            Some(a) => mem::rd(a, size),
            None => read_op(st, insn, 0),
        }
    };
    let v = read(st);
    let cf = (v >> bit) & 1 != 0;
    let nv = match insn.mnemonic() {
        Mnemonic::Bts => Some(v | (1 << bit)),
        Mnemonic::Btr => Some(v & !(1 << bit)),
        Mnemonic::Btc => Some(v ^ (1 << bit)),
        _ => None,
    };
    if let Some(nv) = nv {
        match addr_bits {
            Some(a) => {
                if insn.has_lock_prefix() {
                    let mnem = insn.mnemonic();
                    mem::atomic_rmw(a, size, &mut |old| match mnem {
                        Mnemonic::Bts => old | (1 << bit),
                        Mnemonic::Btr => old & !(1 << bit),
                        _ => old ^ (1 << bit),
                    });
                } else {
                    mem::wr(a, size, nv)
                }
            }
            None => write_op(st, insn, 0, nv),
        }
    }
    set_flags(st, CF, if cf { CF } else { 0 });
}

fn string_op(st: &mut CpuState, insn: &Instruction) -> ExecResult {
    if (0..insn.op_count()).any(|i| matches!(insn.op_kind(i), OpKind::MemorySegSI | OpKind::MemoryESDI | OpKind::MemorySegDI)) {
        return Err(Exit::Unimplemented); // 16-bit address size: never emitted by compilers
    }
    let m = insn.mnemonic();
    let size: u32 = match m {
        Mnemonic::Movsb | Mnemonic::Stosb | Mnemonic::Lodsb | Mnemonic::Cmpsb | Mnemonic::Scasb => 1,
        Mnemonic::Movsw | Mnemonic::Stosw | Mnemonic::Lodsw | Mnemonic::Cmpsw | Mnemonic::Scasw => 2,
        _ => 4,
    };
    let step = if st.eflags & DF != 0 { (size as i32).wrapping_neg() as u32 } else { size };
    let rep = insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix();
    if rep && st.eflags & DF == 0 && bulk_string_op(st, m, size) {
        return Ok(Step::Next);
    }
    loop {
        if rep && st.gpr[ECX] == 0 {
            break;
        }
        match m {
            Mnemonic::Movsb | Mnemonic::Movsw | Mnemonic::Movsd => {
                let v = mem::rd(st.gpr[ESI], size);
                mem::wr(st.gpr[EDI], size, v);
                st.gpr[ESI] = st.gpr[ESI].wrapping_add(step);
                st.gpr[EDI] = st.gpr[EDI].wrapping_add(step);
            }
            Mnemonic::Stosb | Mnemonic::Stosw | Mnemonic::Stosd => {
                mem::wr(st.gpr[EDI], size, st.gpr[EAX]);
                st.gpr[EDI] = st.gpr[EDI].wrapping_add(step);
            }
            Mnemonic::Lodsb | Mnemonic::Lodsw | Mnemonic::Lodsd => {
                let v = mem::rd(st.gpr[ESI], size);
                let r = match size {
                    1 => Register::AL,
                    2 => Register::AX,
                    _ => Register::EAX,
                };
                write_reg(st, r, v);
                st.gpr[ESI] = st.gpr[ESI].wrapping_add(step);
            }
            Mnemonic::Cmpsb | Mnemonic::Cmpsw | Mnemonic::Cmpsd => {
                let a = mem::rd(st.gpr[ESI], size);
                let b = mem::rd(st.gpr[EDI], size);
                let (_, f) = sub_flags(size, a, b, 0);
                set_flags(st, ARITH, f);
                st.gpr[ESI] = st.gpr[ESI].wrapping_add(step);
                st.gpr[EDI] = st.gpr[EDI].wrapping_add(step);
            }
            Mnemonic::Scasb | Mnemonic::Scasw | Mnemonic::Scasd => {
                let b = mem::rd(st.gpr[EDI], size);
                let (_, f) = sub_flags(size, st.gpr[EAX], b, 0);
                set_flags(st, ARITH, f);
                st.gpr[EDI] = st.gpr[EDI].wrapping_add(step);
            }
            _ => return Err(Exit::Unimplemented),
        }
        if !rep {
            break;
        }
        st.gpr[ECX] = st.gpr[ECX].wrapping_sub(1);
        let compares = matches!(
            m,
            Mnemonic::Cmpsb | Mnemonic::Cmpsw | Mnemonic::Cmpsd | Mnemonic::Scasb | Mnemonic::Scasw | Mnemonic::Scasd
        );
        if compares {
            let zf = st.eflags & ZF != 0;
            if insn.has_repe_prefix() && !zf {
                break;
            }
            if insn.has_repne_prefix() && zf {
                break;
            }
        }
    }
    Ok(Step::Next)
}

/// `rep movs`/`rep stos` going forward as one host copy/fill (memcpy/memset in
/// guest code). Returns false when the element loop must run instead: address
/// wrap-around, a forward-overlapping move (x86 replicates the pattern there,
/// memmove would not) or the verifier's write log.
fn bulk_string_op(st: &mut CpuState, m: Mnemonic, size: u32) -> bool {
    use std::sync::atomic::Ordering;
    let is_movs = matches!(m, Mnemonic::Movsb | Mnemonic::Movsw | Mnemonic::Movsd);
    let is_stos = matches!(m, Mnemonic::Stosb | Mnemonic::Stosw | Mnemonic::Stosd);
    if !(is_movs || is_stos) || mem::UNDO_ON.load(Ordering::Relaxed) {
        return false;
    }
    let n = st.gpr[ECX] as u64 * size as u64;
    let (dst, src) = (st.gpr[EDI] as u64, st.gpr[ESI] as u64);
    if n == 0 || dst + n > 1 << 32 || (is_movs && src + n > 1 << 32) {
        return false;
    }
    let tso = mem::TSO.load(Ordering::Relaxed);
    if tso {
        std::sync::atomic::fence(Ordering::SeqCst); // x86 string ops are ordered like their elements
    }
    unsafe {
        let d = mem::ptr(st.gpr[EDI]);
        if is_movs {
            if dst > src && dst < src + n {
                return false;
            }
            std::ptr::copy(mem::ptr(st.gpr[ESI]), d, n as usize);
            st.gpr[ESI] = (src + n) as u32;
        } else {
            match size {
                1 => std::ptr::write_bytes(d, st.gpr[EAX] as u8, n as usize),
                2 => {
                    let v = (st.gpr[EAX] as u16).to_le_bytes();
                    for i in 0..(n as usize / 2) {
                        std::ptr::copy_nonoverlapping(v.as_ptr(), d.add(2 * i), 2);
                    }
                }
                _ => {
                    let v = st.gpr[EAX].to_le_bytes();
                    for i in 0..(n as usize / 4) {
                        std::ptr::copy_nonoverlapping(v.as_ptr(), d.add(4 * i), 4);
                    }
                }
            }
        }
    }
    if tso {
        std::sync::atomic::fence(Ordering::SeqCst);
    }
    st.gpr[EDI] = (dst + n) as u32;
    st.gpr[ECX] = 0;
    true
}

fn cpuid(st: &mut CpuState) {
    let leaf = st.gpr[EAX];
    let (a, b, c, d): (u32, u32, u32, u32) = match leaf {
        0 => (0xd, 0x756e_6547, 0x6c65_746e, 0x4965_6e69), // "GenuineIntel"
        // Family 6 (Core 2-class): FPU, TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2 / SSE3, SSSE3
        1 => (0x0001_06a5, 0x0000_0800, 0x0000_0201, 0x0780_8111 | (1 << 4) | (1 << 15) | (1 << 23) | (1 << 24) | (1 << 25) | (1 << 26)),
        2 => (0x0055_0301, 0, 0, 0x0007_0000),
        4 => match st.gpr[ECX] {
            0 => (0x1c00_4121, 0x01c0_003f, 0x3f, 0),
            1 => (0x1c00_4122, 0x01c0_003f, 0x3f, 0),
            2 => (0x1c00_4143, 0x01c0_003f, 0x1ff, 0),
            3 => (0x1c03_c163, 0x02c0_003f, 0x1fff, 6),
            _ => (0, 0, 0, 0),
        },
        0x8000_0000 => (0x8000_0008, 0, 0, 0),
        0x8000_0001 => (0, 0, 1, 1 << 29),
        0x8000_0002..=0x8000_0004 => {
            let brand = b"Intel(R) Core(TM) i7 CPU (dxhost)               \0";
            let off = ((leaf - 0x8000_0002) * 16) as usize;
            let w = |i: usize| u32::from_le_bytes([brand[off + i], brand[off + i + 1], brand[off + i + 2], brand[off + i + 3]]);
            (w(0), w(4), w(8), w(12))
        }
        0x8000_0006 => (0, 0, 0x0100_6040, 0),
        0x8000_0008 => (0x3024, 0, 0, 0),
        _ => (0, 0, 0, 0),
    };
    st.gpr[EAX] = a;
    st.gpr[EBX] = b;
    st.gpr[ECX] = c;
    st.gpr[EDX] = d;
}

fn mul_div(st: &mut CpuState, insn: &Instruction) -> ExecResult {
    let size = op_size(insn, 0);
    let m = insn.mnemonic();
    // Two/three operand imul
    if m == Mnemonic::Imul && insn.op_count() >= 2 {
        let (a, b) = if insn.op_count() == 3 {
            (read_op(st, insn, 1), insn.immediate(2) as u32)
        } else {
            (read_op(st, insn, 0), read_op(st, insn, 1))
        };
        let (sa, sb) = sext(size, a, b);
        let wide = sa * sb;
        let r = (wide as u32) & mask(size);
        let fits = sext1(size, r) as i64 == wide;
        write_op(st, insn, 0, r);
        let f = szp(size, r) | (if fits { 0 } else { CF | OF });
        set_flags(st, ARITH, f);
        return Ok(Step::Next);
    }
    let src = read_op(st, insn, 0);
    match (m, size) {
        (Mnemonic::Mul, _) | (Mnemonic::Imul, _) => {
            let a = match size {
                1 => st.gpr[EAX] & 0xff,
                2 => st.gpr[EAX] & 0xffff,
                _ => st.gpr[EAX],
            };
            let (lo, hi, overflow) = if m == Mnemonic::Mul {
                let wide = a as u64 * (src & mask(size)) as u64;
                let bits = size * 8;
                let lo = (wide as u32) & mask(size);
                let hi = ((wide >> bits) as u32) & mask(size);
                (lo, hi, hi != 0)
            } else {
                let (sa, sb) = sext(size, a, src);
                let wide = sa * sb;
                let bits = size * 8;
                let lo = (wide as u32) & mask(size);
                let hi = ((wide >> bits) as u32) & mask(size);
                (lo, hi, sext1(size, lo) as i64 != wide)
            };
            match size {
                1 => write_reg(st, Register::AX, lo | (hi << 8)),
                2 => {
                    write_reg(st, Register::AX, lo);
                    write_reg(st, Register::DX, hi);
                }
                _ => {
                    st.gpr[EAX] = lo;
                    st.gpr[EDX] = hi;
                }
            }
            let f = szp(size, lo) | (if overflow { CF | OF } else { 0 });
            set_flags(st, ARITH, f);
        }
        (Mnemonic::Div, _) => {
            let d = (src & mask(size)) as u64;
            if d == 0 {
                return Err(Exit::Fault);
            }
            let n: u64 = match size {
                1 => (st.gpr[EAX] & 0xffff) as u64,
                2 => (((st.gpr[EDX] & 0xffff) << 16) | (st.gpr[EAX] & 0xffff)) as u64,
                _ => ((st.gpr[EDX] as u64) << 32) | st.gpr[EAX] as u64,
            };
            let q = n / d;
            let r = n % d;
            if q > mask(size) as u64 {
                return Err(Exit::Fault);
            }
            store_divres(st, size, q as u32, r as u32);
        }
        (Mnemonic::Idiv, _) => {
            let d = sext1(size, src & mask(size)) as i64;
            if d == 0 {
                return Err(Exit::Fault);
            }
            let n: i64 = match size {
                1 => (st.gpr[EAX] & 0xffff) as i16 as i64,
                2 => ((((st.gpr[EDX] & 0xffff) << 16) | (st.gpr[EAX] & 0xffff)) as i32) as i64,
                _ => (((st.gpr[EDX] as u64) << 32) | st.gpr[EAX] as u64) as i64,
            };
            let q = n.wrapping_div(d);
            let r = n.wrapping_rem(d);
            let lim = (sign_bit(size) as i64) - 1;
            if q > lim || q < -lim - 1 {
                return Err(Exit::Fault);
            }
            store_divres(st, size, q as u32 & mask(size), r as u32 & mask(size));
        }
        _ => return Err(Exit::Unimplemented),
    }
    Ok(Step::Next)
}

fn store_divres(st: &mut CpuState, size: u32, q: u32, r: u32) {
    match size {
        1 => write_reg(st, Register::AX, (q & 0xff) | ((r & 0xff) << 8)),
        2 => {
            write_reg(st, Register::AX, q);
            write_reg(st, Register::DX, r);
        }
        _ => {
            st.gpr[EAX] = q;
            st.gpr[EDX] = r;
        }
    }
}

fn sext1(size: u32, v: u32) -> i32 {
    match size {
        1 => v as u8 as i8 as i32,
        2 => v as u16 as i16 as i32,
        _ => v as i32,
    }
}
fn sext(size: u32, a: u32, b: u32) -> (i64, i64) {
    (sext1(size, a) as i64, sext1(size, b) as i64)
}

pub fn exec(eng: &mut Engine, st: &mut CpuState, insn: &Instruction) -> ExecResult {
    use Mnemonic::*;
    let m = insn.mnemonic();
    match m {
        Nop | Pause | Lfence | Sfence | Mfence | Prefetchnta | Prefetcht0 | Prefetcht1 | Prefetcht2 | Prefetchw
        | Emms | Wait => {
            if m == Emms {
                st.x87.empty = 0xff;
            }
        }
        Mov => {
            let v = read_op(st, insn, 1);
            write_op(st, insn, 0, v);
        }
        Movzx => {
            let v = read_op(st, insn, 1);
            write_op(st, insn, 0, v);
        }
        Movsx => {
            let v = sext1(op_size(insn, 1), read_op(st, insn, 1)) as u32;
            write_op(st, insn, 0, v);
        }
        Lea => {
            let a = ea(st, insn);
            write_op(st, insn, 0, a);
        }
        Push => {
            let v = read_op(st, insn, 0);
            if insn.stack_pointer_increment() == -2 {
                st.gpr[ESP] = st.gpr[ESP].wrapping_sub(2);
                mem::w16(st.gpr[ESP], v as u16);
            } else {
                push32(st, v);
            }
        }
        Pop => {
            if op_size(insn, 0) == 2 {
                let v = mem::r16(st.gpr[ESP]) as u32;
                st.gpr[ESP] = st.gpr[ESP].wrapping_add(2);
                write_op(st, insn, 0, v);
            } else {
                let v = pop32(st);
                // pop [esp+x] uses esp after the increment
                write_op(st, insn, 0, v);
            }
        }
        Pushad => {
            let sp = st.gpr[ESP];
            for i in 0..8 {
                push32(st, if i == ESP { sp } else { st.gpr[i] });
            }
        }
        Popad => {
            for i in (0..8).rev() {
                let v = pop32(st);
                if i != ESP {
                    st.gpr[i] = v;
                }
            }
        }
        Pushfd | Pushf => push32(st, (st.eflags & 0x00fc_0fd5) | 2),
        Popfd | Popf => {
            let v = pop32(st);
            st.eflags = (st.eflags & !0x0024_4dd5) | (v & 0x0024_4dd5) | 2;
        }
        Lahf => {
            let ah = (st.eflags & (SF | ZF | AF | PF | CF)) | 2;
            write_reg(st, Register::AH, ah);
        }
        Sahf => {
            let ah = read_reg(st, Register::AH);
            set_flags(st, SF | ZF | AF | PF | CF, ah);
        }
        Xchg => {
            let b = read_op(st, insn, 1);
            let old = rmw(st, insn, &mut |_| b);
            write_op(st, insn, 1, old);
        }
        Xadd => {
            let size = op_size(insn, 0);
            let b = read_op(st, insn, 1);
            let mut fl = 0;
            let old = rmw(st, insn, &mut |a| {
                let (r, f) = add_flags(size, a, b, 0);
                fl = f;
                r
            });
            write_op(st, insn, 1, old);
            set_flags(st, ARITH, fl);
        }
        Cmpxchg => {
            let size = op_size(insn, 0);
            let acc = st.gpr[EAX] & mask(size);
            let new = read_op(st, insn, 1);
            let mut swapped = false;
            // The closure runs again when a locked CAS loses a race, so `swapped`
            // must reflect the last attempt only.
            let old = rmw(st, insn, &mut |cur| {
                swapped = cur & mask(size) == acc;
                if swapped { new } else { cur }
            });
            let (_, f) = sub_flags(size, acc, old, 0);
            set_flags(st, ARITH, f);
            if !swapped {
                let r = match size {
                    1 => Register::AL,
                    2 => Register::AX,
                    _ => Register::EAX,
                };
                write_reg(st, r, old);
            }
        }
        Cmpxchg8b => {
            let a = ea(st, insn);
            let expected = ((st.gpr[EDX] as u64) << 32) | st.gpr[EAX] as u64;
            let new = ((st.gpr[ECX] as u64) << 32) | st.gpr[EBX] as u64;
            let (ok, old) = mem::atomic_cas64(a, expected, new);
            if ok {
                st.eflags |= ZF;
            } else {
                st.eflags &= !ZF;
                st.gpr[EAX] = old as u32;
                st.gpr[EDX] = (old >> 32) as u32;
            }
        }
        Add | Adc | Sub | Sbb | And | Or | Xor | Cmp | Test => alu(st, insn),
        Inc | Dec => {
            let size = op_size(insn, 0);
            let mut fl = 0;
            rmw(st, insn, &mut |a| {
                let (r, f) = if m == Inc { add_flags(size, a, 1, 0) } else { sub_flags(size, a, 1, 0) };
                fl = f;
                r
            });
            set_flags(st, ARITH & !CF, fl);
        }
        Neg => {
            let size = op_size(insn, 0);
            let mut fl = 0;
            rmw(st, insn, &mut |a| {
                let (r, f) = sub_flags(size, 0, a, 0);
                fl = f;
                r
            });
            set_flags(st, ARITH, fl);
        }
        Not => {
            let size = op_size(insn, 0);
            rmw(st, insn, &mut |a| !a & mask(size));
        }
        Shl | Sal | Shr | Sar | Rol | Ror | Rcl | Rcr => return shift(st, insn).map(|_| Step::Next),
        Shld | Shrd => shld_shrd(st, insn),
        Mul | Imul | Div | Idiv => return mul_div(st, insn),
        Bt | Bts | Btr | Btc => bit_op(st, insn),
        Bsf | Bsr => {
            let size = op_size(insn, 0);
            let v = read_op(st, insn, 1) & mask(size);
            if v == 0 {
                st.eflags |= ZF;
            } else {
                st.eflags &= !ZF;
                let r = if m == Bsf { v.trailing_zeros() } else { 31 - v.leading_zeros() };
                write_op(st, insn, 0, r);
            }
        }
        Bswap => {
            let v = read_op(st, insn, 0);
            write_op(st, insn, 0, v.swap_bytes());
        }
        Cbw => {
            let v = st.gpr[EAX] as u8 as i8 as i16 as u16 as u32;
            write_reg(st, Register::AX, v);
        }
        Cwde => st.gpr[EAX] = st.gpr[EAX] as u16 as i16 as i32 as u32,
        Cwd => {
            let v = if st.gpr[EAX] & 0x8000 != 0 { 0xffff } else { 0 };
            write_reg(st, Register::DX, v);
        }
        Cdq => st.gpr[EDX] = if st.gpr[EAX] & 0x8000_0000 != 0 { 0xffff_ffff } else { 0 },
        Clc => st.eflags &= !CF,
        Stc => st.eflags |= CF,
        Cmc => st.eflags ^= CF,
        Cld => st.eflags &= !DF,
        Std => st.eflags |= DF,
        Seto | Setno | Setb | Setae | Sete | Setne | Setbe | Seta | Sets | Setns | Setp | Setnp | Setl | Setge
        | Setle | Setg => {
            let v = cond(st, m) as u32;
            write_op(st, insn, 0, v);
        }
        Cmovo | Cmovno | Cmovb | Cmovae | Cmove | Cmovne | Cmovbe | Cmova | Cmovs | Cmovns | Cmovp | Cmovnp
        | Cmovl | Cmovge | Cmovle | Cmovg => {
            let v = read_op(st, insn, 1); // memory is read even if not taken
            if cond(st, m) {
                write_op(st, insn, 0, v);
            }
        }
        Jo | Jno | Jb | Jae | Je | Jne | Jbe | Ja | Js | Jns | Jp | Jnp | Jl | Jge | Jle | Jg => {
            if cond(st, m) {
                st.eip = branch_target(insn);
                return Ok(Step::Jump);
            }
            st.eip = insn.next_ip32();
            return Ok(Step::Jump);
        }
        Jecxz | Jcxz => {
            st.eip = if st.gpr[ECX] == 0 { branch_target(insn) } else { insn.next_ip32() };
            return Ok(Step::Jump);
        }
        Loop | Loope | Loopne => {
            st.gpr[ECX] = st.gpr[ECX].wrapping_sub(1);
            let zf = st.eflags & ZF != 0;
            let take = st.gpr[ECX] != 0 && match m {
                Loope => zf,
                Loopne => !zf,
                _ => true,
            };
            st.eip = if take { branch_target(insn) } else { insn.next_ip32() };
            return Ok(Step::Jump);
        }
        Jmp => {
            st.eip = match insn.op0_kind() {
                OpKind::NearBranch32 | OpKind::NearBranch16 => branch_target(insn),
                _ => read_op(st, insn, 0),
            };
            return Ok(Step::Jump);
        }
        Call => {
            let target = match insn.op0_kind() {
                OpKind::NearBranch32 | OpKind::NearBranch16 => branch_target(insn),
                _ => read_op(st, insn, 0),
            };
            push32(st, insn.next_ip32());
            st.eip = target;
            return Ok(Step::Jump);
        }
        Ret => {
            let ret = pop32(st);
            if insn.op_count() > 0 {
                st.gpr[ESP] = st.gpr[ESP].wrapping_add(insn.immediate16() as u32);
            }
            st.eip = ret;
            return Ok(Step::Jump);
        }
        Leave => {
            st.gpr[ESP] = st.gpr[EBP];
            st.gpr[EBP] = pop32(st);
        }
        Enter => {
            let size = insn.immediate16() as u32;
            let level = insn.immediate8_2nd() as u32 & 0x1f;
            push32(st, st.gpr[EBP]);
            let frame = st.gpr[ESP];
            for i in 1..level {
                let v = mem::r32(st.gpr[EBP].wrapping_sub(4 * i));
                push32(st, v);
            }
            if level > 0 {
                push32(st, frame);
            }
            st.gpr[EBP] = frame;
            st.gpr[ESP] = st.gpr[ESP].wrapping_sub(size);
        }
        Movsb | Movsw | Movsd if insn.op_count() == 0 || insn.op0_kind() == OpKind::MemoryESEDI => {
            return string_op(st, insn);
        }
        Stosb | Stosw | Stosd | Lodsb | Lodsw | Lodsd | Cmpsb | Cmpsw | Cmpsd | Scasb | Scasw | Scasd
            if insn.op_count() == 0 || !matches!(insn.op0_kind(), OpKind::Register) || is_string_form(insn) =>
        {
            return string_op(st, insn);
        }
        Cpuid => cpuid(st),
        Rdtsc => {
            eng.tsc = eng.tsc.wrapping_add(1);
            let t = host_ticks().wrapping_add(eng.tsc);
            st.gpr[EAX] = t as u32;
            st.gpr[EDX] = (t >> 32) as u32;
        }
        Int3 | Hlt | Ud2 | Int => return Err(Exit::Halt),
        Xlatb => {
            let a = st.gpr[EBX].wrapping_add(st.gpr[EAX] & 0xff);
            let v = mem::r8(a) as u32;
            write_reg(st, Register::AL, v);
        }
        _ => {
            if let Some(r) = crate::fpu::exec(st, insn) {
                return r;
            }
            if let Some(r) = crate::sse::exec(st, insn) {
                return r;
            }
            return Err(Exit::Unimplemented);
        }
    }
    Ok(Step::Next)
}

fn is_string_form(insn: &Instruction) -> bool {
    matches!(
        insn.op0_kind(),
        OpKind::MemoryESEDI | OpKind::MemorySegESI | OpKind::MemorySegSI | OpKind::MemoryESDI
    ) || matches!(insn.op1_kind(), OpKind::MemorySegESI | OpKind::MemoryESEDI)
}

fn host_ticks() -> u64 {
    use std::time::Instant;
    // One epoch for the whole process: the TSC is a machine-wide clock and games
    // compare readings taken on different threads.
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    // 2.4 GHz: the clock rate the host reports to the guest (hw.cpufrequency and
    // Gestalt fall back to 2.4 GHz on Apple Silicon, src/host/api/sys.c).
    START.get_or_init(Instant::now).elapsed().as_nanos() as u64 * 12 / 5
}
