//! Translated code vs interpreter on hand-written instruction sequences that
//! exercise translator special cases (lazy flags, NaN fixups, ...). Each case
//! runs from the same state through both engines; registers, defined flags,
//! xmm registers and memory must match.
//!
//! Guest memory is process-global (mem::set_base), so the cases run one at a
//! time under LOCK.

use dxcpu::interp::Engine;
use dxcpu::jit::JitEngine;
use dxcpu::mem;
use dxcpu::state::*;
use iced_x86::code_asm::*;
use std::sync::Mutex;

const MEM: usize = 1 << 20;
const CODE: u32 = 0x1000;
const STOP: u32 = 0x8000;
const DATA: u32 = 0x10000;
const STACK: u32 = 0x80000;

static LOCK: Mutex<()> = Mutex::new(());

/// Assemble a sequence at CODE, followed by a jump to STOP.
fn asm(f: impl FnOnce(&mut CodeAssembler) -> Result<(), IcedError>) -> Vec<u8> {
    let mut a = CodeAssembler::new(32).unwrap();
    f(&mut a).unwrap();
    a.jmp(STOP as u64).unwrap();
    a.assemble(CODE as u64).unwrap()
}

struct Out {
    st: CpuState,
    exit: Exit,
    mem: Vec<u8>,
}

fn run(jit: bool, code: &[u8], st: &CpuState, data: &[u8]) -> Out {
    let mut m = vec![0u8; MEM];
    m[CODE as usize..CODE as usize + code.len()].copy_from_slice(code);
    m[DATA as usize..DATA as usize + data.len()].copy_from_slice(data);
    mem::set_base(m.as_mut_ptr());
    let mut s = *st;
    let exit = if jit {
        JitEngine::new(0xFFFF_0000, 0xFFFF_0010).run(&mut s, STOP)
    } else {
        Engine::new(0xFFFF_0000, 0xFFFF_0010).run(&mut s, STOP)
    };
    mem::set_base(std::ptr::null_mut());
    Out { st: s, exit, mem: m }
}

/// Run `code` in both engines and compare; `flags` = EFLAGS bits that must match.
fn check(name: &str, code: &[u8], st: &CpuState, data: &[u8], flags: u32) -> CpuState {
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let a = run(false, code, st, data);
    let b = run(true, code, st, data);
    assert_eq!(a.exit, b.exit, "{name}: exit");
    assert_eq!(a.st.gpr, b.st.gpr, "{name}: gpr");
    assert_eq!(a.st.eip, b.st.eip, "{name}: eip");
    assert_eq!(a.st.eflags & flags, b.st.eflags & flags, "{name}: eflags {:#x} vs {:#x}", a.st.eflags, b.st.eflags);
    for i in 0..8 {
        assert_eq!(a.st.xmm[i], b.st.xmm[i], "{name}: xmm{i}");
    }
    assert!(a.mem == b.mem, "{name}: memory");
    b.st
}

fn state() -> CpuState {
    let mut st = CpuState::new();
    st.gpr[ESP] = STACK;
    st.gpr[EBX] = DATA;
    st.eip = CODE;
    st
}

fn set_ps(st: &mut CpuState, i: usize, v: [u32; 4]) {
    for k in 0..4 {
        st.xmm[i][4 * k..4 * k + 4].copy_from_slice(&v[k].to_le_bytes());
    }
}

const SPECIAL: [u32; 8] = [
    0x3f80_0000, // 1
    0x0000_0000, // 0
    0x7f80_0000, // +inf
    0xff80_0000, // -inf
    0x7fc0_0001, // QNaN
    0xffc0_0002, // -QNaN
    0x7fa0_0003, // SNaN
    0xbf80_0000, // -1
];

/// Single-precision arithmetic with every pair of special operands in every
/// lane position: x86 NaN propagation (first operand wins, quieted) and the
/// x86 default NaN must come out of the cold fixup path exactly.
#[test]
fn sse_single_nan_results() {
    let ops: Vec<(&str, Vec<u8>)> = vec![
        ("addps", asm(|a| a.addps(xmm0, xmm1))),
        ("subps", asm(|a| a.subps(xmm0, xmm1))),
        ("mulps", asm(|a| a.mulps(xmm0, xmm1))),
        ("divps", asm(|a| a.divps(xmm0, xmm1))),
        ("sqrtps", asm(|a| a.sqrtps(xmm0, xmm1))),
        ("sqrtps same", asm(|a| a.sqrtps(xmm0, xmm0))),
        ("addps same", asm(|a| a.addps(xmm0, xmm0))),
        ("addss", asm(|a| a.addss(xmm0, xmm1))),
        ("mulss", asm(|a| a.mulss(xmm0, xmm1))),
        ("divss", asm(|a| a.divss(xmm0, xmm1))),
        ("sqrtss", asm(|a| a.sqrtss(xmm0, xmm1))),
        ("mulps mem", asm(|a| a.mulps(xmm0, xmmword_ptr(ebx)))),
        ("subss mem", asm(|a| a.subss(xmm0, dword_ptr(ebx + 4)))),
        // chained: a fixed-up NaN feeds the next operation
        (
            "chain",
            asm(|a| {
                a.mulps(xmm0, xmm1)?;
                a.addps(xmm0, xmm1)?;
                a.movaps(xmm2, xmm0)?;
                a.divps(xmm2, xmm1)?;
                a.addss(xmm2, xmm0)
            }),
        ),
    ];
    for (name, code) in &ops {
        for (i, &x) in SPECIAL.iter().enumerate() {
            for (j, &y) in SPECIAL.iter().enumerate() {
                let mut st = state();
                let z = SPECIAL[(i + j) % 8];
                set_ps(&mut st, 0, [x, y, z, x]);
                set_ps(&mut st, 1, [y, x, y, z]);
                set_ps(&mut st, 2, [z, z, x, y]);
                let mut data = Vec::new();
                for v in [y, x, z, y] {
                    data.extend_from_slice(&v.to_le_bytes());
                }
                check(&format!("{name} {x:#x} {y:#x}"), code, &st, &data, flags::ARITH);
            }
        }
    }
}

/// inc/dec keep CF; after adc/sbb/shifts it is only in a register (FC).
#[test]
fn carry_kept_by_inc_dec() {
    let codes: Vec<(&str, Vec<u8>)> = vec![
        ("adc inc", asm(|a| {
            a.adc(eax, ebx)?;
            a.inc(ecx)?;
            a.setb(dl)
        })),
        ("sbb dec adc", asm(|a| {
            a.sbb(eax, 0x12345)?;
            a.dec(ecx)?;
            a.adc(edx, 0)
        })),
        ("shl inc", asm(|a| {
            a.shl(eax, 3)?;
            a.inc(ecx)?;
            a.setb(dl)
        })),
        ("adc mem dec sbb8", asm(|a| {
            a.adc(dword_ptr(ebx), edx)?;
            a.dec(edi)?;
            a.sbb(ah, dh)
        })),
    ];
    for (name, code) in &codes {
        for (eax_v, cf) in [(0xffff_fff0u32, 0u32), (0xffff_fff0, 1), (5, 0), (0x8000_0001, 1), (0, 1)] {
            let mut st = state();
            st.gpr[EAX] = eax_v;
            st.gpr[ECX] = 0x7fff_ffff;
            st.gpr[EDX] = 0x8000_0000;
            st.gpr[EDI] = 1;
            st.eflags = 0x202 | cf;
            check(&format!("{name} {eax_v:#x} cf={cf}"), code, &st, &0xfedc_ba98u32.to_le_bytes(), flags::ARITH);
        }
    }
}

/// Stores at every alignment, including ones that cross a 16-byte boundary
/// (TSO store-release has to take its fallback path there).
#[test]
fn unaligned_stores() {
    for off in 0..20i32 {
        let code = asm(|a| {
            a.mov(dword_ptr(ebx + off), eax)?;
            a.mov(word_ptr(ebx + off + 32), cx)?;
            a.mov(byte_ptr(ebx + off + 64), dl)?;
            a.add(dword_ptr(ebx + off + 96), esi)?;
            a.movss(dword_ptr(ebx + off + 128), xmm0)?;
            a.movsd_2(qword_ptr(ebx + off + 160), xmm1)?;
            a.movups(xmmword_ptr(ebx + off + 192), xmm2)?;
            a.movq(qword_ptr(ebx + off + 224), xmm3)?;
            a.fld(dword_ptr(ebx + 252))?;
            a.fst(dword_ptr(ebx + off + 8))?;
            a.fstp(qword_ptr(ebx + off + 40))?;
            a.mov(eax, dword_ptr(ebx + off + 96))
        });
        let mut st = state();
        st.gpr[EAX] = 0x1122_3344;
        st.gpr[ECX] = 0x5566;
        st.gpr[EDX] = 0x77;
        st.gpr[ESI] = 0x0102_0304;
        for i in 0..4 {
            set_ps(&mut st, i, [0x3f80_0000 + i as u32, 0xdead_beef, 0x1234_5678, 0xcafe_f00d]);
        }
        check(&format!("stores at +{off}"), &code, &st, &[0xa5; 256], flags::ARITH);
    }
}

/// cmp/test/add directly before the block's conditional branch, with flags
/// read again in both successor blocks (lazy descriptor stored at the exits).
#[test]
fn flags_across_final_branch() {
    type Op = fn(&mut CodeAssembler) -> Result<(), IcedError>;
    let ops: [(&str, Op); 9] = [
        ("cmp r,r", |a| a.cmp(eax, ebx)),
        ("cmp r,imm", |a| a.cmp(eax, 0x40)),
        ("cmp r,bigimm", |a| a.cmp(eax, 0x1234_5678)),
        ("test r,r", |a| a.test(eax, ebx)),
        ("test r,imm", |a| a.test(eax, 0x8000_0001u32 as i32)),
        ("cmp m,r", |a| a.cmp(dword_ptr(edi), eax)),
        ("cmp r8,r8", |a| a.cmp(al, bl)),
        ("sub m16,imm", |a| a.sub(word_ptr(edi + 2), 0x7fff)),
        ("and r,r", |a| a.and(eax, ebx)),
    ];
    for (name, op) in ops {
        for jcc in 0..4 {
            let code = asm(|a| {
                let mut taken = a.create_label();
                op(a)?;
                match jcc {
                    0 => a.jl(taken)?,
                    1 => a.jp(taken)?,
                    2 => a.jb(taken)?,
                    _ => a.je(taken)?,
                }
                a.setp(dl)?;
                a.seta(dh)?;
                a.adc(esi, 0)?;
                a.jmp(STOP as u64)?;
                a.set_label(&mut taken)?;
                a.setp(cl)?;
                a.pushfd()?;
                a.pop(esi)?;
                a.jo(STOP as u64)
            });
            for (x, y) in [(0x40u32, 0x40u32), (0x7fff_ffff, 0xffff_ffff), (3, 0x8000_0000), (0x8000_0001, 1), (0x41, 0x40)] {
                let mut st = state();
                st.gpr[EAX] = x;
                st.gpr[EBX] = y;
                st.gpr[EDI] = DATA;
                let mut data = y.to_le_bytes().to_vec();
                data.extend_from_slice(&x.to_le_bytes());
                check(&format!("{name} j{jcc} {x:#x} {y:#x}"), &code, &st, &data, flags::ARITH);
            }
        }
    }
}

/// Flags are not preserved across calls and returns: a flag result right
/// before ret / call is not stored as a lazy descriptor; before a conditional
/// branch it is (the successor may read it).
#[test]
fn flags_dead_at_call_and_ret() {
    const STP_FA_FB: u32 = 0x2906_2b89; // stp w9, w10, [x28, #48]
    let stores = |f: &dyn Fn(&mut CodeAssembler) -> Result<(), IcedError>| {
        let code = asm(|a| f(a));
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let mut m = vec![0u8; MEM];
        m[CODE as usize..CODE as usize + code.len()].copy_from_slice(&code);
        mem::set_base(m.as_mut_ptr());
        let (w, body) = dxcpu::jit::Jit::new(0xFFFF_0000, 0xFFFF_0010).translate_words(CODE);
        mem::set_base(std::ptr::null_mut());
        w[..body].iter().filter(|&&x| x == STP_FA_FB).count()
    };
    assert_eq!(stores(&|a| {
        a.add(esp, 0x98)?;
        a.pop(esi)?;
        a.ret()
    }), 0);
    assert_eq!(stores(&|a| {
        a.cmp(eax, ebx)?;
        a.call(dword_ptr(eax + 8))
    }), 0);
    // live: an indirect jump may land anywhere
    assert_eq!(stores(&|a| {
        a.cmp(eax, ebx)?;
        a.jmp(eax)
    }), 1);
    // and a normal call/return sequence still computes the same
    let code = asm(|a| {
        let mut f = a.create_label();
        let mut done = a.create_label();
        a.mov(ecx, 5)?;
        let mut lp = a.create_label();
        a.set_label(&mut lp)?;
        a.call(f)?;
        a.add(eax, ebx)?;
        a.dec(ecx)?;
        a.jne(lp)?;
        a.jmp(done)?;
        a.set_label(&mut f)?;
        a.cmp(eax, 0x100)?;
        a.adc(ebx, 1)?;
        a.add(esp, 0)?;
        a.ret()?;
        a.set_label(&mut done)?;
        a.cmp(eax, ebx)
    });
    let mut st = state();
    st.gpr[EAX] = 0xf0;
    st.gpr[EBX] = 7;
    check("call/ret loop", &code, &st, &[], flags::ARITH);
}

/// Effective addresses of every shape (base, index * scale, small / large /
/// negative displacement, absolute), also with the destination of lea / mov
/// being the base or index register; push/pop of registers, memory and ESP.
#[test]
fn addressing_modes() {
    let mems: Vec<(&str, AsmMemoryOperand)> = vec![
        ("[ebx]", dword_ptr(ebx)),
        ("[ebx+8]", dword_ptr(ebx + 8)),
        ("[ebx+5000]", dword_ptr(ebx + 5000)),
        ("[esi-12]", dword_ptr(esi - 12)),
        ("[esi-5000]", dword_ptr(esi - 5000)),
        ("[ebx+ecx]", dword_ptr(ebx + ecx)),
        ("[ebx+ecx*4+12]", dword_ptr(ebx + ecx * 4 + 12)),
        ("[ebx+ecx*8-4]", dword_ptr(ebx + ecx * 8 - 4)),
        ("[ebx+ecx*2+5000]", dword_ptr(ebx + ecx * 2 + 5000)),
        ("[ecx*4+DATA]", dword_ptr(ecx * 4 + DATA as i32)),
        ("[ecx*4+DATA+5000]", dword_ptr(ecx * 4 + DATA as i32 + 5000)),
        ("[DATA+16]", dword_ptr(DATA as u64 + 16)),
    ];
    for (name, m) in &mems {
        let m = *m;
        let code = asm(|a| {
            a.lea(eax, m)?;
            a.lea(edx, m)?;
            a.mov(edi, m)?;
            a.add(m, 0x55)?;
            a.mov(m, edx)?;
            a.mov(m, 0)?;
            a.movzx(ebp, byte_ptr(eax))?;
            a.movsx(ebp, word_ptr(eax + 2))?;
            a.push(m)?;
            a.push(esp)?;
            a.push(edi)?;
            a.pop(m)?;
            a.pop(ebp)?;
            a.pop(esp)?;
            a.lea(ecx, m)?; // index register as destination
            a.lea(esi, m)?; // base register as destination
            a.lea(ebx, m) // base register as destination
        });
        let mut st = state();
        st.gpr[EBX] = DATA + 0x100;
        st.gpr[ECX] = 3;
        st.gpr[ESI] = DATA + 0x1400;
        let data: Vec<u8> = (0..0x2000u32).map(|i| (i * 7 + 3) as u8).collect();
        check(name, &code, &st, &data, flags::ARITH);
    }
}

/// Modified xmm registers at a block-ending branch (written back once before
/// it) and in a self loop (kept cached across the back edge).
#[test]
fn xmm_across_branches() {
    let code = asm(|a| {
        let mut lp = a.create_label();
        let mut skip = a.create_label();
        a.mulps(xmm0, xmm1)?;
        a.movaps(xmm2, xmm0)?;
        a.cmp(eax, ebx)?;
        a.jl(skip)?;
        a.addps(xmm2, xmm1)?;
        a.set_label(&mut skip)?;
        a.movaps(xmm5, xmm2)?;
        a.set_label(&mut lp)?;
        a.addps(xmm0, xmm1)?;
        a.subss(xmm3, xmm0)?;
        a.dec(ecx)?;
        a.jne(lp)?;
        a.movups(xmmword_ptr(ebx), xmm0)?;
        a.cmp(eax, 7)?;
        a.je(STOP as u64)?;
        a.movaps(xmm4, xmm3)
    });
    for (x, y) in [(1u32, 2u32), (5, 3), (7, 7)] {
        let mut st = state();
        st.gpr[EAX] = x;
        st.gpr[EBX] = DATA + 16 * y;
        st.gpr[ECX] = 3 + y;
        for i in 0..6 {
            set_ps(&mut st, i, [0x3f80_0000 + i as u32 * 0x10_0000, 0x4000_0000, 0xbf80_0000, 0x3e80_0000]);
        }
        check(&format!("xmm branches {x} {y}"), &code, &st, &[0; 256], flags::ARITH);
    }
}

/// pshufd / shufps with every immediate, in place and between registers.
#[test]
fn shuffles() {
    for imm in 0..256u32 {
        let code = asm(|a| {
            a.pshufd(xmm1, xmm0, imm)?;
            a.pshufd(xmm2, xmm2, imm)?;
            a.shufps(xmm3, xmm3, imm)?;
            a.shufps(xmm4, xmm0, imm)?;
            a.pshufd(xmm5, xmmword_ptr(ebx), imm)
        });
        let mut st = state();
        for i in 0..6 {
            let b = 0x1111_1111 * (i as u32 + 1);
            set_ps(&mut st, i, [b, b + 0x0101_0101, b + 0x0202_0202, b + 0x0303_0303]);
        }
        let data: Vec<u8> = (0..16u8).collect();
        check(&format!("shuffles {imm:#x}"), &code, &st, &data, flags::ARITH);
    }
}

/// Calls into the thunk stub range leave the engine with Exit::Stub; the host
/// returns by popping the return address. Many round trips through the
/// dispatcher (block cache lookups, lazy flags dropped at the stub exit).
#[test]
fn thunk_round_trips() {
    const STUB: u32 = 0xFFFF_0004;
    let code = asm(|a| {
        let mut lp = a.create_label();
        a.mov(esi, 50)?;
        a.set_label(&mut lp)?;
        a.add(eax, ebx)?;
        a.cmp(eax, 0x10)?;
        a.call(STUB as u64)?;
        a.adc(ecx, eax)?;
        a.dec(esi)?;
        a.jne(lp)
    });
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let mut results = Vec::new();
    for jit in [false, true] {
        let mut m = vec![0u8; MEM];
        m[CODE as usize..CODE as usize + code.len()].copy_from_slice(&code);
        mem::set_base(m.as_mut_ptr());
        let mut st = state();
        st.gpr[EBX] = 0x1234_5678;
        let mut je = JitEngine::new(0xFFFF_0000, 0xFFFF_0010);
        let mut ie = Engine::new(0xFFFF_0000, 0xFFFF_0010);
        let mut trace = Vec::new();
        let exit = loop {
            let e = if jit { je.run(&mut st, STOP) } else { ie.run(&mut st, STOP) };
            if e != Exit::Stub {
                break e;
            }
            trace.push((st.eip, st.gpr));
            // host: return value in eax, flags as the callee left them (CF set)
            let top = st.gpr[ESP];
            st.eip = mem::r32(top);
            st.gpr[ESP] = top + 4;
            st.gpr[EAX] = st.gpr[EAX].wrapping_mul(3);
            st.eflags |= flags::CF;
        };
        mem::set_base(std::ptr::null_mut());
        results.push((exit, st.gpr, st.eflags & flags::ARITH, trace));
    }
    assert_eq!(results[0].3.len(), 50);
    assert!(results[0] == results[1], "thunk round trips differ");
}

/// Barrier placement under DXCPU_TSO (default): counts of load barriers (dmb
/// ishld) and ordered stores (dmb ishst or stlr) in the block body.
#[test]
fn tso_barrier_shapes() {
    if dxcpu::jit::tso_mode() != 2 {
        return;
    }
    const DMB_ISHLD: u32 = 0xd503_39bf;
    const DMB_ISHST: u32 = 0xd503_3abf;
    let shape = |f: &dyn Fn(&mut CodeAssembler) -> Result<(), IcedError>| {
        let code = asm(|a| f(a));
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let mut m = vec![0u8; MEM];
        m[CODE as usize..CODE as usize + code.len()].copy_from_slice(&code);
        mem::set_base(m.as_mut_ptr());
        let (w, body) = dxcpu::jit::Jit::new(0xFFFF_0000, 0xFFFF_0010).translate_words(CODE);
        mem::set_base(std::ptr::null_mut());
        let w = &w[..body];
        let ld = w.iter().filter(|&&x| x == DMB_ISHLD).count();
        let st = w.iter().filter(|&&x| x == DMB_ISHST || x & 0x3fff_fc00 == 0x089f_fc00).count();
        (ld, st)
    };
    // address-dependent load chain: one barrier after the last load
    assert_eq!(
        shape(&|a| {
            a.mov(ecx, dword_ptr(ebx + 8))?;
            a.mov(eax, dword_ptr(ecx + 4))?;
            a.movss(xmm0, dword_ptr(eax + ecx * 4))
        }),
        (1, 0)
    );
    // independent loads keep theirs
    assert_eq!(
        shape(&|a| {
            a.mov(ecx, dword_ptr(ebx + 8))?;
            a.mov(eax, dword_ptr(esi + 4))
        }),
        (2, 0)
    );
    // a store in between: no elision
    assert_eq!(
        shape(&|a| {
            a.mov(ecx, dword_ptr(ebx))?;
            a.mov(dword_ptr(edi), eax)?;
            a.mov(eax, dword_ptr(ecx))
        }),
        (2, 1)
    );
    // the next load is a stack access without its own barrier: no elision
    assert_eq!(
        shape(&|a| {
            a.mov(ebp, dword_ptr(ebx))?;
            a.mov(eax, dword_ptr(ebp - 8))
        }),
        (1, 0)
    );
    // after `mov ebp, esp`, [ebp + x] is a stack access; not once EBP changed
    assert_eq!(
        shape(&|a| {
            a.push(ebp)?;
            a.mov(ebp, esp)?;
            a.mov(eax, dword_ptr(ebp + 8))?;
            a.mov(dword_ptr(ebp + 12), eax)?;
            a.pop(ebp)?;
            a.mov(ecx, dword_ptr(ebp + 8))
        }),
        (1, 0)
    );
}
