//! JIT vs interpreter differential test for SSE translation: random blocks
//! mixing SSE/SSE2/SSE3 instructions (translated ones and some interpreter
//! fallbacks) with integer instructions, run through both engines. XMM
//! registers (bit-exact), GPRs, EFLAGS (defined bits), MXCSR control bits and
//! memory are compared. Register/memory contents mix random bits with special
//! float values (NaNs, infinities, zeros, denormals, values near the int32
//! limits). Some blocks run with a non-default MXCSR (rounding, FTZ, DAZ), some
//! loop back to their start a few times (block chaining with cached xmm state).
//!
//! MXCSR exception flags (bits 0-5) are not compared: translated code does not
//! accumulate them.
//!
//!   cargo run --release --example jittest_sse -- [blocks] [seed]

use dxcpu::interp::Engine;
use dxcpu::jit::JitEngine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::{
    Decoder, DecoderOptions, Formatter, Instruction, InstructionInfoFactory, IntelFormatter, Mnemonic, OpAccess,
    OpKind, Register, RflagsBits,
};

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
    fn u32(&mut self) -> u32 {
        (self.next().wrapping_mul(0x2545_f491_4f6c_dd1d) >> 32) as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.u32() % n
    }
}

fn special32(rng: &mut Rng) -> u32 {
    let sign = if rng.below(2) == 0 { 0 } else { 0x8000_0000 };
    let v = match rng.below(16) {
        0 => 0,
        1 => 0x7f80_0000,                              // inf
        2 => 0x7fc0_0000 | (rng.u32() & 0x003f_ffff), // qnan
        3 => 0x7f80_0001 | (rng.u32() & 0x003f_ffff), // snan
        4 => rng.u32() & 0x007f_ffff,                  // denormal
        5 => 0x0080_0000 | (rng.u32() & 0xf),          // tiny normal
        6 => 0x7f7f_ffff - (rng.u32() & 0xf),          // huge
        7 => (rng.below(2000) as f32).to_bits(),       // small integer
        8 => (rng.below(200) as f32 + 0.5).to_bits(),  // halves (rounding ties)
        9 => 0x4f00_0000 + rng.below(4) - 2,           // around 2^31
        10 => 0x3f80_0000,                             // 1.0
        11 => 0x0000_0001 + rng.below(4),              // smallest denormals
        12 => 0x7fc0_0000,                             // default qnan
        13 => (rng.below(2000) as f32 * 0.25).to_bits(),
        _ => ((rng.u32() % 64 + 100) << 23) | (rng.u32() & 0x7f_ffff), // moderate exponents
    };
    (v & 0x7fff_ffff) | sign
}

fn special64(rng: &mut Rng) -> u64 {
    let sign = if rng.below(2) == 0 { 0 } else { 1u64 << 63 };
    let r = rng.next();
    let v = match rng.below(15) {
        0 => 0,
        1 => 0x7ff0_0000_0000_0000,
        2 => 0x7ff8_0000_0000_0000 | (r & 0x0007_ffff_ffff_ffff),
        3 => 0x7ff0_0000_0000_0001 | (r & 0x0007_ffff_ffff_ffff),
        4 => r & 0x000f_ffff_ffff_ffff,
        5 => 0x0010_0000_0000_0000 | (r & 0xf),
        6 => 0x7fef_ffff_ffff_ffff - (r & 0xf),
        7 => (rng.below(2000) as f64).to_bits(),
        8 => (rng.below(200) as f64 + 0.5).to_bits(),
        9 => (2147483648.0f64 + (rng.below(7) as f64 - 3.0) * 0.5).to_bits(),
        10 => 0x3ff0_0000_0000_0000,
        11 => (special32(rng) as f32 as f64).to_bits(),
        12 => 0x36a0_0000_0000_0000 | (r & 0x000f_ffff_ffff_ffff), // rounds to f32 denormal
        13 => 0x47e0_0000_0000_0000 | (r & 0x000f_ffff_ffff_ffff), // near f32 overflow
        _ => ((r % 128 + 960) << 52) | (r & 0x000f_ffff_ffff_ffff),
    };
    (v & 0x7fff_ffff_ffff_ffff) | sign
}

fn rand_vec(rng: &mut Rng) -> [u8; 16] {
    let mut v = [0u8; 16];
    match rng.below(5) {
        0 => {
            for b in v.iter_mut() {
                *b = rng.u32() as u8;
            }
        }
        1 | 2 => {
            for i in 0..4 {
                let x = if rng.below(6) == 0 { rng.u32() } else { special32(rng) };
                v[4 * i..4 * i + 4].copy_from_slice(&x.to_le_bytes());
            }
        }
        3 => {
            for i in 0..4 {
                let x = (rng.below(5) as i32 - 2) as u32 ^ if rng.below(2) == 0 { 0x8000_0000 } else { 0 };
                v[4 * i..4 * i + 4].copy_from_slice(&x.to_le_bytes());
            }
        }
        _ => {
            for i in 0..2 {
                let x = if rng.below(6) == 0 { rng.next() } else { special64(rng) };
                v[8 * i..8 * i + 8].copy_from_slice(&x.to_le_bytes());
            }
        }
    }
    v
}

/// Second opcode bytes (after 0F) of interesting SSE instructions.
const SSE_OPS: &[u8] = &[
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x50, 0x51,
    0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x7c, 0x7e, 0x7f, 0xc2, 0xc6, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc,
    0xdd, 0xde, 0xdf, 0xe0, 0xe3, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf5,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xae, 0xc4, 0xc5, 0xd0, 0xe4, 0xf4, 0x7d,
];

fn mem_ok(insn: &Instruction) -> bool {
    insn.memory_base() == Register::EBX
        && insn.memory_index() == Register::None
        && (insn.memory_displacement32() as i32).abs() <= 1024
        && matches!(insn.memory_segment(), Register::DS | Register::SS)
}

fn regs_ok(insn: &Instruction, info: &mut InstructionInfoFactory) -> bool {
    let m = insn.mnemonic();
    for r in info.info(insn).used_registers() {
        let reg = r.register().full_register32();
        let writes = !matches!(r.access(), OpAccess::Read | OpAccess::CondRead | OpAccess::None);
        if writes && (reg == Register::EBX || reg == Register::ESI || (reg == Register::ESP && !matches!(m, Mnemonic::Push | Mnemonic::Pop))) {
            return false;
        }
    }
    true
}

fn sse_ok(insn: &Instruction, info: &mut InstructionInfoFactory) -> bool {
    if insn.is_invalid() || insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix() {
        return false;
    }
    let code = format!("{:?}", insn.code());
    if code.starts_with("VEX") || code.starts_with("EVEX") || code.starts_with("XOP") || code.starts_with("D3NOW") {
        return false;
    }
    let m = insn.mnemonic();
    // stmxcsr: the stored exception flags legitimately differ (not accumulated by translated code)
    if matches!(m, Mnemonic::Ldmxcsr | Mnemonic::Stmxcsr | Mnemonic::Fxsave | Mnemonic::Fxrstor | Mnemonic::Maskmovdqu | Mnemonic::Maskmovq)
        || format!("{:?}", m).starts_with("Prefetch")
        || format!("{:?}", m).ends_with("fence")
    {
        return false;
    }
    let mut touches = m == Mnemonic::Stmxcsr;
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => {
                if !mem_ok(insn) {
                    return false;
                }
            }
            OpKind::Register => {
                let r = insn.op_register(i);
                if r.is_mm() || r.is_segment_register() || r.is_cr() || r.is_dr() {
                    return false;
                }
                if r.is_xmm() {
                    touches = true;
                }
            }
            OpKind::Immediate8 => {}
            _ => return false,
        }
    }
    if !touches && !matches!(m, Mnemonic::Cvttss2si | Mnemonic::Cvtss2si | Mnemonic::Cvttsd2si | Mnemonic::Cvtsd2si) {
        return false;
    }
    regs_ok(insn, info)
}

fn int_ok(insn: &Instruction, info: &mut InstructionInfoFactory) -> bool {
    use Mnemonic::*;
    if insn.is_invalid() || insn.has_rep_prefix() || insn.has_repe_prefix() || insn.has_repne_prefix() {
        return false;
    }
    let m = insn.mnemonic();
    let s = format!("{:?}", m);
    let allowed = matches!(m, Mov | Movzx | Lea | Add | Sub | And | Or | Xor | Cmp | Test | Inc | Dec | Neg | Adc | Sbb)
        || s.starts_with("Set")
        || s.starts_with("Cmov");
    if !allowed {
        return false;
    }
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Memory => {
                if !mem_ok(insn) {
                    return false;
                }
            }
            OpKind::Register
                if insn.op_register(i).is_segment_register() || insn.op_register(i).is_dr() || insn.op_register(i).is_cr() =>
            {
                return false
            }
            OpKind::Register | OpKind::Immediate8 | OpKind::Immediate16 | OpKind::Immediate32
            | OpKind::Immediate8to16 | OpKind::Immediate8to32 => {}
            _ => return false,
        }
    }
    regs_ok(insn, info)
}

fn flag_mask(bits: u32) -> u32 {
    let mut m = 0u32;
    if bits & RflagsBits::AF != 0 { m |= flags::AF; }
    if bits & RflagsBits::OF != 0 { m |= flags::OF; }
    if bits & RflagsBits::CF != 0 { m |= flags::CF; }
    if bits & RflagsBits::ZF != 0 { m |= flags::ZF; }
    if bits & RflagsBits::SF != 0 { m |= flags::SF; }
    if bits & RflagsBits::PF != 0 { m |= flags::PF; }
    m
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let blocks: usize = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(20000);
    let seed: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(7);
    let mut rng = Rng(seed | 1);
    let mut info = InstructionInfoFactory::new();
    let mut fmt = IntelFormatter::new();
    let mut mem_a = vec![0u8; MEM];
    let mut mem_b = vec![0u8; MEM];
    let jcc_ops: [u8; 16] = [0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f];
    let mut failures = 0;
    let mut executed: u64 = 0;
    let mut by_mnemonic: std::collections::BTreeMap<String, u64> = Default::default();
    for blk in 0..blocks {
        let mut code: Vec<u8> = Vec::new();
        let mut listing = Vec::new();
        let mut undefined_mask: u32 = 0;
        let maxn = match rng.below(8) {
            0 => 130, // longer than a translation block (96 instructions)
            1 | 2 => 60,
            _ => 16,
        };
        let mxcsr_slot = DATA + 0xe00; // [ebx+0x600], out of reach of the random operands
        let n = 1 + rng.below(maxn) as usize;
        let looping = rng.below(4) == 0;
        let mut count = 0;
        while count < n {
            let mut bytes = [0u8; 16];
            for b in bytes.iter_mut() {
                *b = rng.u32() as u8;
            }
            if rng.below(40) == 0 {
                // ldmxcsr [ebx+0x600]: switches the JIT's translation mode mid-run
                code.extend_from_slice(&[0x0f, 0xae, 0x93, 0x00, 0x06, 0x00, 0x00]);
                listing.push("ldmxcsr [ebx+600h]".into());
                count += 1;
                continue;
            }
            let sse = rng.below(5) != 0;
            if !sse && rng.below(3) == 0 {
                // setcc r/m8 or cmovcc r32, r/m32 (including the parity forms used after ucomiss)
                bytes[0] = 0x0f;
                bytes[1] = if rng.below(2) == 0 { 0x90 } else { 0x40 } | rng.below(16) as u8;
                if rng.below(2) == 0 {
                    bytes[1] = (bytes[1] & 0xf0) | if rng.below(2) == 0 { 0xa } else { 0xb };
                }
                bytes[2] = if rng.below(2) == 0 { 0xc0 | (bytes[2] & 0x3f) } else { 0x40 | (bytes[2] & 0x38) | 3 };
            }
            if sse {
                // [prefix] 0F op modrm ...
                let mut k = 0;
                match rng.below(4) {
                    0 => {}
                    1 => { bytes[0] = 0x66; k = 1; }
                    2 => { bytes[0] = 0xf3; k = 1; }
                    _ => { bytes[0] = 0xf2; k = 1; }
                }
                bytes[k] = 0x0f;
                bytes[k + 1] = SSE_OPS[rng.below(SSE_OPS.len() as u32) as usize];
                // bias ModRM: register form or [ebx+disp8]
                let modrm = bytes[k + 2];
                bytes[k + 2] = match if matches!(bytes[k + 1], 0x71..=0x73) { 0 } else { rng.below(3) } {
                    0 => 0xc0 | (modrm & 0x3f),
                    1 => 0x40 | (modrm & 0x38) | 3,
                    _ => modrm,
                };
            }
            let mut dec = Decoder::with_ip(32, &bytes, (CODE as usize + code.len()) as u64, DecoderOptions::NONE);
            let insn = dec.decode();
            let good = if sse { sse_ok(&insn, &mut info) } else { int_ok(&insn, &mut info) };
            if !good {
                continue;
            }
            code.extend_from_slice(&bytes[..insn.len()]);
            let w = insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set() | insn.rflags_undefined();
            if w != 0 {
                let def = flag_mask((insn.rflags_written() | insn.rflags_cleared() | insn.rflags_set()) & !insn.rflags_undefined());
                undefined_mask = (undefined_mask & !def) | flag_mask(insn.rflags_undefined());
            }
            *by_mnemonic.entry(format!("{:?}", insn.mnemonic())).or_default() += 1;
            let mut t = String::new();
            fmt.format(&insn, &mut t);
            listing.push(t);
            count += 1;
        }
        let mut iters = 1;
        if looping {
            // dec esi; jnz CODE  (esi = iteration count, never written by the block)
            code.push(0x4e);
            let here = CODE as usize + code.len();
            code.extend_from_slice(&[0x0f, 0x85]);
            code.extend_from_slice(&((CODE as i64 - (here as i64 + 6)) as i32).to_le_bytes());
            listing.push("dec esi; jnz start".into());
            undefined_mask = 0;
            iters = 1 + rng.below(3);
        }
        // jcc L1; mov edx,1; jmp STOP; L1: mov edx,2; jmp STOP
        let jcc = jcc_ops[rng.below(16) as usize];
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
            st.gpr[i] = match rng.below(4) {
                0 => rng.below(40),
                1 => special32(&mut rng),
                _ => rng.u32(),
            };
        }
        st.gpr[EBX] = DATA + 0x800;
        st.gpr[ESP] = STACK;
        st.gpr[ESI] = iters;
        st.eflags = 0x202 | (rng.u32() & flags::ARITH);
        st.eip = CODE;
        for i in 0..8 {
            st.xmm[i] = rand_vec(&mut rng);
        }
        if rng.below(8) == 0 {
            // non-default MXCSR: rounding mode, FTZ, DAZ
            st.mxcsr = 0x1f80 | (rng.below(4) << 13) | if rng.below(2) == 0 { 0x8000 } else { 0 } | if rng.below(2) == 0 { 0x40 } else { 0 };
        }
        for m in [&mut mem_a, &mut mem_b] {
            m[CODE as usize..CODE as usize + 0x1000].fill(0);
            m[CODE as usize..CODE as usize + code.len()].copy_from_slice(&code);
            m[(STACK - 256) as usize..(STACK + 256) as usize].fill(0x5a);
        }
        for i in 0..0x100usize {
            let v = rand_vec(&mut rng);
            mem_a[DATA as usize + 16 * i..DATA as usize + 16 * i + 16].copy_from_slice(&v);
            mem_b[DATA as usize + 16 * i..DATA as usize + 16 * i + 16].copy_from_slice(&v);
        }
        let mx: u32 = if rng.below(2) == 0 {
            0x1f80
        } else {
            0x1f80 | (rng.below(4) << 13) | if rng.below(2) == 0 { 0x8000 } else { 0 } | if rng.below(2) == 0 { 0x40 } else { 0 }
        };
        for m in [&mut mem_a, &mut mem_b] {
            m[mxcsr_slot as usize..mxcsr_slot as usize + 4].copy_from_slice(&mx.to_le_bytes());
        }
        let mut sa = st;
        let mut sb = st;
        mem::set_base(mem_a.as_mut_ptr());
        let mut ie = Engine::new(0xFFFF_0000, 0xFFFF_0010);
        let ea = ie.run(&mut sa, STOP);
        mem::set_base(mem_b.as_mut_ptr());
        let mut je = JitEngine::new(0xFFFF_0000, 0xFFFF_0010);
        let eb = je.run(&mut sb, STOP);
        executed += (n as u64) * iters as u64;

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
            for i in 0..8 {
                if sa.xmm[i] != sb.xmm[i] {
                    diffs.push(format!(
                        "xmm{}: interp {:032x} jit {:032x}",
                        i,
                        u128::from_le_bytes(sa.xmm[i]),
                        u128::from_le_bytes(sb.xmm[i])
                    ));
                }
            }
            if (sa.eflags ^ sb.eflags) & (flags::ARITH | flags::DF) & !undefined_mask != 0 {
                diffs.push(format!("eflags: interp {:#x} jit {:#x}", sa.eflags & flags::ARITH, sb.eflags & flags::ARITH));
            }
            if (sa.mxcsr ^ sb.mxcsr) & !0x3f != 0 {
                diffs.push(format!("mxcsr: interp {:#x} jit {:#x}", sa.mxcsr, sb.mxcsr));
            }
            if mem_a[DATA as usize..DATA as usize + 0x1000] != mem_b[DATA as usize..DATA as usize + 0x1000]
                || mem_a[(STACK - 256) as usize..(STACK + 256) as usize] != mem_b[(STACK - 256) as usize..(STACK + 256) as usize]
            {
                diffs.push("memory differs".into());
            }
        }
        if !diffs.is_empty() {
            failures += 1;
            println!("MISMATCH in block {} (mxcsr {:#x}, esi {})", blk, st.mxcsr, iters);
            for i in 0..8 {
                println!("    in xmm{}: {:032x}", i, u128::from_le_bytes(st.xmm[i]));
            }
            println!("    in gpr {:08x?} eflags {:#x}", st.gpr, st.eflags);
            for l in &listing {
                println!("    {}", l);
            }
            for d in diffs {
                println!("  {}", d);
            }
            if failures > 10 {
                break;
            }
        }
    }
    if std::env::var("SHOW").is_ok() {
        for (k, v) in &by_mnemonic {
            println!("{:12} {}", k, v);
        }
    }
    println!("{} blocks, {} instructions executed, {} mismatches", blocks, executed, failures);
}
