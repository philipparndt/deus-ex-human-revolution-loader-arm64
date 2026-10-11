//! Differential tester: random x86 instructions executed one at a time in
//! Unicorn (reference) and in dxcpu's interpreter; registers, defined flags
//! and memory are compared after every instruction.
//!
//!   cargo run --release --example difftest -- [int|x87|sse] [iterations] [seed]

use dxcpu::interp::Engine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::{
    Decoder, DecoderOptions, Formatter, Instruction, InstructionInfoFactory, IntelFormatter, Mnemonic, OpAccess,
    OpKind, Register, RflagsBits,
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
}

#[derive(Clone, Copy, PartialEq)]
enum Family {
    Int,
    X87,
    Sse,
}

fn family_of(m: Mnemonic) -> Option<Family> {
    let s = format!("{:?}", m);
    let s = s.as_str();
    use Mnemonic::*;
    if matches!(
        m,
        Mov | Movzx | Movsx | Lea | Push | Pop | Xchg | Xadd | Cmpxchg | Add | Adc | Sub | Sbb | And | Or | Xor | Cmp
            | Test | Inc | Dec | Neg | Not | Shl | Sal | Shr | Sar | Rol | Ror | Rcl | Rcr | Shld | Shrd | Mul | Imul
            | Bt | Bts | Btr | Btc | Bsf | Bsr | Bswap | Cbw | Cwde | Cwd | Cdq | Clc | Stc | Cmc | Lahf | Sahf
            | Movsb | Movsw | Stosb | Stosw | Stosd | Lodsb | Lodsw | Lodsd | Cmpsb | Cmpsw | Scasb | Scasw | Scasd
            | Xlatb
    ) || s.starts_with("Set")
        || s.starts_with("Cmov")
    {
        return Some(Family::Int);
    }
    if s.starts_with('F') {
        return Some(Family::X87);
    }
    Some(Family::Sse) // everything else is filtered by register usage below
}

fn acceptable(insn: &Instruction, fam: Family, info: &mut InstructionInfoFactory) -> bool {
    if insn.is_invalid() || insn.has_rep_prefix() || insn.has_repne_prefix() || insn.has_repe_prefix() {
        return false;
    }
    let m = insn.mnemonic();
    // Undocumented F6/F7 /1 TEST alias (Unicorn rejects it, compilers never emit it)
    // and privileged debug/control register moves.
    if insn.code() == iced_x86::Code::Test_rm8_imm8_F6r1 || insn.code() == iced_x86::Code::Test_rm32_imm32_F7r1
        || insn.code() == iced_x86::Code::Test_rm16_imm16_F7r1 {
        return false;
    }
    for i in 0..insn.op_count() {
        if insn.op_kind(i) == OpKind::Register && (insn.op_register(i).is_dr() || insn.op_register(i).is_cr() || insn.op_register(i).is_tr()) {
            return false;
        }
    }
    if family_of(m) != Some(fam) {
        return false;
    }
    // reject privileged/control/system/stack-unbalancing instructions
    if matches!(
        m,
        Mnemonic::Pushad | Mnemonic::Popad | Mnemonic::Pushfd | Mnemonic::Popfd | Mnemonic::Pushf | Mnemonic::Popf
    ) {
        return false;
    }
    if fam == Family::Sse {
        // must touch xmm or mm registers, and nothing exotic
        let s = format!("{:?}", insn.code());
        if !(s.contains("xmm") || s.contains("mm")) || s.starts_with("EVEX") || s.starts_with("VEX") {
            return false;
        }
    }
    // memory operands: only [ebx+disp8] or string forms
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
            OpKind::MemorySegESI | OpKind::MemoryESEDI | OpKind::MemorySegSI | OpKind::MemoryESDI => {
                if insn.memory_segment() != Register::DS && insn.op_kind(i) != OpKind::MemoryESEDI {
                    return false;
                }
            }
            OpKind::Register if insn.op_register(i).is_segment_register() => return false,
            OpKind::Register | OpKind::Immediate8 | OpKind::Immediate16 | OpKind::Immediate32
            | OpKind::Immediate8to16 | OpKind::Immediate8to32 | OpKind::Immediate8_2nd => {}
            _ => return false,
        }
    }
    // keep the pointer registers stable (string ops move esi/edi a little, fine)
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
        if writes && reg == Register::EBX {
            return false;
        }
        if writes && reg == Register::ESP && !matches!(m, Mnemonic::Push | Mnemonic::Pop) {
            return false;
        }
        if writes && matches!(reg, Register::ESI | Register::EDI) && !is_string(m) {
            return false;
        }
    }
    true
}

fn is_string(m: Mnemonic) -> bool {
    use Mnemonic::*;
    matches!(m, Movsb | Movsw | Stosb | Stosw | Stosd | Lodsb | Lodsw | Lodsd | Cmpsb | Cmpsw | Scasb | Scasw | Scasd)
}

fn undefined_flags(insn: &Instruction) -> u32 {
    let u = insn.rflags_undefined();
    let mut f = 0;
    if u & RflagsBits::CF != 0 { f |= flags::CF; }
    if u & RflagsBits::PF != 0 { f |= flags::PF; }
    if u & RflagsBits::AF != 0 { f |= flags::AF; }
    if u & RflagsBits::ZF != 0 { f |= flags::ZF; }
    if u & RflagsBits::SF != 0 { f |= flags::SF; }
    if u & RflagsBits::OF != 0 { f |= flags::OF; }
    // Shifts/rotates by more than one bit leave OF undefined.
    if matches!(insn.mnemonic(), Mnemonic::Shl | Mnemonic::Sal | Mnemonic::Shr | Mnemonic::Sar | Mnemonic::Rol
        | Mnemonic::Ror | Mnemonic::Rcl | Mnemonic::Rcr | Mnemonic::Shld | Mnemonic::Shrd) {
        f |= flags::OF | flags::AF;
    }
    f
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

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let fam = match args.get(1).map(|s| s.as_str()) {
        Some("x87") => Family::X87,
        Some("sse") => Family::Sse,
        _ => Family::Int,
    };
    let iterations: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(20000);
    let seed: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(0x1234_5678);
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
    let mut info = InstructionInfoFactory::new();
    let mut fmt = IntelFormatter::new();
    let (mut tested, mut failures) = (0usize, 0usize);
    let mut per_mnemonic: std::collections::HashMap<String, (usize, usize)> = Default::default();
    let mut unimpl: std::collections::BTreeMap<String, usize> = Default::default();

    while tested < iterations && failures < 40 {
        // random candidate instruction
        let mut bytes = [0u8; 16];
        for b in bytes.iter_mut() {
            *b = rng.u32() as u8;
        }
        let mut dec = Decoder::with_ip(32, &bytes, CODE as u64, DecoderOptions::NONE);
        let insn = dec.decode();
        if !acceptable(&insn, fam, &mut info) {
            continue;
        }
        let len = insn.len();
        // initial state
        for m in [&mut mem_uc, &mut mem_ip] {
            m[CODE as usize..CODE as usize + len].copy_from_slice(&bytes[..len]);
            m[CODE as usize + len] = 0xF4;
        }
        for m in [&mut mem_uc, &mut mem_ip] {
            m[(STACK - 256) as usize..(STACK + 256) as usize].fill(0x5a);
        }
        for i in 0..(DATA_SIZE as usize) {
            let v = rng.u32() as u8;
            mem_uc[DATA as usize + i] = v;
            mem_ip[DATA as usize + i] = v;
        }
        let mut st = CpuState::new();
        for i in 0..8 {
            st.gpr[i] = if rng.u32() % 4 == 0 { rng.u32() % 64 } else { rng.u32() };
        }
        st.gpr[EBX] = DATA + 0x800;
        st.gpr[ESI] = DATA + 0x400;
        st.gpr[EDI] = DATA + 0xC00;
        st.gpr[ESP] = STACK;
        st.eflags = 0x202 | (rng.u32() & flags::ARITH);
        st.eip = CODE;
        for i in 0..8 {
            for b in st.xmm[i].iter_mut() {
                *b = rng.u32() as u8;
            }
        }
        for i in 0..8 {
            wreg(&uc, UC_REGS[i], st.gpr[i] as u64);
        }
        wreg(&uc, UC_EFLAGS, st.eflags as u64);
        for i in 0..8 {
            unsafe { uc_reg_write(uc.0, UC_XMM0 + i as i32, st.xmm[i].as_ptr() as *const c_void) };
        }
        wreg(&uc, UC_MXCSR, 0x1f80);
        let before = st;

        // The code bytes changed behind Unicorn's back: drop its translation.
        unsafe { uc_ctl(uc.0, UC_CTL_TB_REMOVE_CACHE_W, CODE as u64, CODE as u64 + 32) };
        let uc_err = unsafe { uc_emu_start(uc.0, CODE as u64, 0, 0, 1) };
        eng.flush();
        let exit = eng.run(&mut st, CODE + len as u32);
        if exit == Exit::Unimplemented {
            *unimpl.entry(format!("{:?}", insn.mnemonic())).or_default() += 1;
            continue;
        }
        tested += 1;
        let mut text = String::new();
        fmt.format(&insn, &mut text);
        let e = per_mnemonic.entry(format!("{:?}", insn.mnemonic())).or_default();
        e.0 += 1;

        let mut diffs = Vec::new();
        if uc_err != 0 || exit != Exit::Stop {
            if !(uc_err != 0 && exit == Exit::Fault) {
                diffs.push(format!("exit: unicorn err {} vs dxcpu {:?}", uc_err, exit));
            }
        } else {
            for i in 0..8 {
                let u = rreg(&uc, UC_REGS[i]) as u32;
                if u != st.gpr[i] {
                    diffs.push(format!("{}: unicorn {:#010x} dxcpu {:#010x}", ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"][i], u, st.gpr[i]));
                }
            }
            let ueip = rreg(&uc, UC_EIP) as u32;
            if ueip != st.eip {
                diffs.push(format!("eip: unicorn {:#x} dxcpu {:#x}", ueip, st.eip));
            }
            let fmask = (flags::ARITH | flags::DF) & !undefined_flags(&insn);
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
            if fam == Family::X87 {
                let usw = rreg(&uc, UC_FPSW) as u32;
                let utop = (usw >> 11) & 7;
                if utop != st.x87.top {
                    diffs.push(format!("x87 top: unicorn {} dxcpu {}", utop, st.x87.top));
                }
                let _ = (UC_FPCW, UC_FPTAG, UC_ST0);
            }
            if mem_uc[DATA as usize..(DATA + DATA_SIZE) as usize] != mem_ip[DATA as usize..(DATA + DATA_SIZE) as usize]
            {
                diffs.push("data memory differs".into());
            }
            if mem_uc[(STACK - 64) as usize..(STACK + 64) as usize] != mem_ip[(STACK - 64) as usize..(STACK + 64) as usize] {
                diffs.push("stack memory differs".into());
            }
        }
        if !diffs.is_empty() {
            failures += 1;
            e.1 += 1;
            println!("MISMATCH  {:<40} bytes {:02x?}", text, &bytes[..len]);
            println!("   in: gpr {:08x?} eflags {:#x}", before.gpr, before.eflags);
            for d in diffs {
                println!("   {}", d);
            }
        }
    }
    println!("\n{} instructions tested, {} mismatches", tested, failures);
    let mut bad: Vec<_> = per_mnemonic.iter().filter(|(_, v)| v.1 > 0).collect();
    bad.sort();
    for (m, (n, f)) in bad {
        println!("  {:<12} {}/{} failed", m, f, n);
    }
    if !unimpl.is_empty() {
        println!("unimplemented (skipped): {:?}", unimpl);
    }
}
