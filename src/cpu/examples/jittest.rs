//! JIT vs interpreter differential test: random blocks of integer
//! instructions (plus a conditional branch) run through both engines.
//!
//!   cargo run --release --example jittest -- [blocks] [seed]

use dxcpu::interp::Engine;
use dxcpu::jit::JitEngine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, InstructionInfoFactory, IntelFormatter, Mnemonic, OpAccess, OpKind, Register};

const MEM: usize = 1 << 20;
const CODE: u32 = 0x1000;
const STOP: u32 = 0x8000;
const DATA: u32 = 0x10000;
const STACK: u32 = 0x80000;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

fn ok(insn: &Instruction, info: &mut InstructionInfoFactory) -> bool {
    use Mnemonic::*;
    if insn.is_invalid() || insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix() {
        return false;
    }
    let m = insn.mnemonic();
    let s = format!("{:?}", m);
    let allowed = matches!(
        m,
        Mov | Movzx | Movsx | Lea | Push | Pop | Xchg | Add | Adc | Sub | Sbb | And | Or | Xor | Cmp | Test | Inc
            | Dec | Neg | Not | Shl | Shr | Sar | Rol | Ror | Imul | Mul | Bt | Bswap | Cdq | Cwde
            | Clc | Stc | Cmc | Sahf
    ) || s.starts_with("Set")
        || s.starts_with("Cmov");
    if !allowed {
        return false;
    }
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => {
                if insn.memory_base() != Register::EBX || insn.memory_index() != Register::None
                    || (insn.memory_displacement32() as i32).abs() > 1024
                    || !matches!(insn.memory_segment(), Register::DS | Register::SS)
                {
                    return false;
                }
            }
            OpKind::Register if insn.op_register(i).is_segment_register() || insn.op_register(i).is_dr() || insn.op_register(i).is_cr() => return false,
            OpKind::Register | OpKind::Immediate8 | OpKind::Immediate16 | OpKind::Immediate32
            | OpKind::Immediate8to16 | OpKind::Immediate8to32 => {}
            _ => return false,
        }
    }
    let ii = info.info(insn);
    for r in ii.used_registers() {
        let reg = r.register().full_register32();
        let writes = !matches!(r.access(), OpAccess::Read | OpAccess::CondRead | OpAccess::None);
        if writes && (reg == Register::EBX || (reg == Register::ESP && !matches!(m, Push | Pop))) {
            return false;
        }
    }
    if m == Pop && insn.op0_kind() == OpKind::Register && insn.op0_register() == Register::ESP {
        return false;
    }
    // keep push/pop balanced-ish: only allow 32-bit forms
    if matches!(m, Push | Pop) && insn.stack_pointer_increment().abs() != 4 {
        return false;
    }
    true
}

fn written_defined(insn: &Instruction) -> u32 {
    let w = (insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set()) & !insn.rflags_undefined();
    let mut m = 0u32;
    if w & iced_x86::RflagsBits::AF != 0 { m |= flags::AF; }
    if w & iced_x86::RflagsBits::OF != 0 { m |= flags::OF; }
    if w & iced_x86::RflagsBits::CF != 0 { m |= flags::CF; }
    if w & iced_x86::RflagsBits::ZF != 0 { m |= flags::ZF; }
    if w & iced_x86::RflagsBits::SF != 0 { m |= flags::SF; }
    if w & iced_x86::RflagsBits::PF != 0 { m |= flags::PF; }
    m
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let blocks: usize = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(5000);
    let seed: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(99);
    let mut rng = Rng(seed | 1);
    let mut info = InstructionInfoFactory::new();
    let mut fmt = IntelFormatter::new();
    let mut mem_a = vec![0u8; MEM];
    let mut mem_b = vec![0u8; MEM];
    let jcc_ops: [u8; 14] = [0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7c, 0x7d, 0x7e, 0x7f];
    let mut failures = 0;
    for blk in 0..blocks {
        // build a random block
        let mut code: Vec<u8> = Vec::new();
        let mut listing = Vec::new();
        let mut undefined_mask: u32 = 0; // flags left undefined by the last flag writer
        let n = 1 + (rng.next() % 12) as usize;
        while listing.len() < n {
            let mut bytes = [0u8; 16];
            for b in bytes.iter_mut() {
                *b = rng.next() as u8;
            }
            let mut dec = Decoder::with_ip(32, &bytes, (CODE as usize + code.len()) as u64, DecoderOptions::NONE);
            let insn = dec.decode();
            if !ok(&insn, &mut info) {
                continue;
            }
            code.extend_from_slice(&bytes[..insn.len()]);
            let w = insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set() | insn.rflags_undefined();
            if w != 0 {
                let u = insn.rflags_undefined();
                let mut m = 0u32;
                if u & iced_x86::RflagsBits::AF != 0 { m |= flags::AF; }
                if u & iced_x86::RflagsBits::OF != 0 { m |= flags::OF; }
                if u & iced_x86::RflagsBits::CF != 0 { m |= flags::CF; }
                if u & iced_x86::RflagsBits::ZF != 0 { m |= flags::ZF; }
                if u & iced_x86::RflagsBits::SF != 0 { m |= flags::SF; }
                if u & iced_x86::RflagsBits::PF != 0 { m |= flags::PF; }
                // undefined bits stay undefined until something defines them again
                undefined_mask = (undefined_mask & !written_defined(&insn)) | m;
            }
            let mut t = String::new();
            fmt.format(&insn, &mut t);
            listing.push(t);
        }
        // jcc L1; mov edx,1; jmp STOP; L1: mov edx,2; jmp STOP
        let jcc = jcc_ops[(rng.next() % 14) as usize];
        code.extend_from_slice(&[jcc, 10]);
        code.extend_from_slice(&[0xba, 1, 0, 0, 0]);
        let here = CODE as usize + code.len();
        code.push(0xe9);
        code.extend_from_slice(&((STOP as i64 - (here as i64 + 5)) as i32).to_le_bytes());
        code.extend_from_slice(&[0xba, 2, 0, 0, 0]);
        let here = CODE as usize + code.len();
        code.push(0xe9);
        code.extend_from_slice(&((STOP as i64 - (here as i64 + 5)) as i32).to_le_bytes());
        listing.push(format!("j{:02x} ...", jcc));

        let mut st = CpuState::new();
        for i in 0..8 {
            st.gpr[i] = if rng.next() % 3 == 0 { (rng.next() % 40) as u32 } else { rng.next() as u32 };
        }
        st.gpr[EBX] = DATA + 0x800;
        st.gpr[ESP] = STACK;
        st.eflags = 0x202 | (rng.next() as u32 & flags::ARITH);
        st.eip = CODE;
        for m in [&mut mem_a, &mut mem_b] {
            m[CODE as usize..CODE as usize + code.len()].copy_from_slice(&code);
            m[(STACK - 256) as usize..(STACK + 256) as usize].fill(0x5a);
        }
        for i in 0..0x1000usize {
            let v = rng.next() as u8;
            mem_a[DATA as usize + i] = v;
            mem_b[DATA as usize + i] = v;
        }
        let mut sa = st;
        let mut sb = st;
        mem::set_base(mem_a.as_mut_ptr());
        let mut ie = Engine::new(0xFFFF_0000, 0xFFFF_0010);
        let ea = ie.run(&mut sa, STOP);
        mem::set_base(mem_b.as_mut_ptr());
        let mut je = JitEngine::new(0xFFFF_0000, 0xFFFF_0010);
        let eb = je.run(&mut sb, STOP);

        let mut diffs = Vec::new();
        if ea != eb {
            diffs.push(format!("exit: interp {:?} jit {:?}", ea, eb));
        }
        if ea == Exit::Stop && eb == Exit::Stop {
            for i in 0..8 {
                if sa.gpr[i] != sb.gpr[i] {
                    diffs.push(format!("{}: interp {:#x} jit {:#x}", ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"][i], sa.gpr[i], sb.gpr[i]));
                }
            }
            if (sa.eflags ^ sb.eflags) & (flags::ARITH | flags::DF) & !undefined_mask != 0 {
                diffs.push(format!("eflags: interp {:#x} jit {:#x}", sa.eflags & flags::ARITH, sb.eflags & flags::ARITH));
            }
            if mem_a[DATA as usize..DATA as usize + 0x1000] != mem_b[DATA as usize..DATA as usize + 0x1000]
                || mem_a[(STACK - 256) as usize..(STACK + 256) as usize] != mem_b[(STACK - 256) as usize..(STACK + 256) as usize]
            {
                diffs.push("memory differs".into());
            }
        }
        if !diffs.is_empty() {
            failures += 1;
            println!("MISMATCH in block {} (in: gpr {:08x?} eflags {:#x})", blk, st.gpr, st.eflags);
            for l in &listing {
                println!("    {}", l);
            }
            for d in diffs {
                println!("  {}", d);
            }
            if failures > 15 {
                break;
            }
        }
    }
    println!("{} blocks, {} mismatches", blocks, failures);
}
