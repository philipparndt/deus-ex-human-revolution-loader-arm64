//! C interface (see src/host/dxcpu.h).

use crate::interp::Engine as InterpEngine;
use crate::jit::JitEngine;
use crate::mem;
use crate::state::CpuState;
use iced_x86::{Decoder, DecoderOptions, Formatter, IntelFormatter};

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_set_guest_base(base: *mut u8) {
    mem::set_base(base);
}

/// Guest code at [addr, addr+len) may have changed (mprotect to executable,
/// munmap, mmap MAP_FIXED). Returns true if translations were invalidated.
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_code_changed(addr: u32, len: u32) -> bool {
    crate::jit::code_changed(addr, len)
}

/// Host query "is [addr, addr+len) mapped guest memory" (speculative decoding).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_set_mapped_query(f: extern "C" fn(u32, u32) -> bool) {
    mem::set_mapped_query(f);
}

/// Execution engine: JIT by default, pure interpreter with DXCPU_INTERP=1.
pub enum Engine {
    Jit(JitEngine),
    Interp(InterpEngine),
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_engine_new(stub_lo: u32, stub_hi: u32) -> *mut Engine {
    if std::env::var_os("DXCPU_STATS").is_some() {
        crate::jit::STATS_ON.store(true, std::sync::atomic::Ordering::Relaxed);
    }
    let e = if std::env::var_os("DXCPU_INTERP").is_some() {
        Engine::Interp(InterpEngine::new(stub_lo, stub_hi))
    } else {
        Engine::Jit(JitEngine::new(stub_lo, stub_hi))
    };
    Box::into_raw(Box::new(e))
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_engine_flush(eng: *mut Engine) {
    match unsafe { &mut *eng } {
        Engine::Jit(j) => *j = JitEngine::new(j.jit.stub_lo, j.jit.stub_hi),
        Engine::Interp(i) => i.flush(),
    }
}

/// Run until eip == stop or an exit; returns the Exit reason.
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_run(eng: *mut Engine, st: *mut CpuState, stop: u32) -> u32 {
    let (eng, st) = unsafe { (&mut *eng, &mut *st) };
    let e = match eng {
        Engine::Jit(j) => j.run(st, stop),
        Engine::Interp(i) => i.run(st, stop),
    };
    st.exit_reason = e as u32;
    e as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_state_size() -> u32 {
    std::mem::size_of::<CpuState>() as u32
}

/// Disassemble the instruction at `addr` into buf (for diagnostics).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_disasm(addr: u32, buf: *mut u8, len: u32) -> u32 {
    let bytes = unsafe { std::slice::from_raw_parts(mem::ptr(addr), 16) };
    let mut dec = Decoder::with_ip(32, bytes, addr as u64, DecoderOptions::NONE);
    let insn = dec.decode();
    let mut out = String::new();
    IntelFormatter::new().format(&insn, &mut out);
    let n = out.len().min(len as usize - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(out.as_ptr(), buf, n);
        *buf.add(n) = 0;
    }
    insn.len() as u32
}

/// New zeroed CPU state (flat 32-bit user mode defaults).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_state_new() -> *mut CpuState {
    Box::into_raw(Box::new(CpuState::new()))
}

/// Register access by index: 0-7 = eax..edi, 8 = eip, 9 = eflags,
/// 10 = fault eip, 11 = fault address.
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_get_reg(st: *const CpuState, idx: u32) -> u32 {
    let st = unsafe { &*st };
    match idx {
        0..=7 => st.gpr[idx as usize],
        8 => st.eip,
        9 => st.eflags,
        10 => st.fault_eip,
        11 => st.fault_addr,
        _ => 0,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_set_reg(st: *mut CpuState, idx: u32, v: u32) {
    let st = unsafe { &mut *st };
    match idx {
        0..=7 => st.gpr[idx as usize] = v,
        8 => st.eip = v,
        9 => st.eflags = v,
        _ => {}
    }
}

/// Push a value onto the x87 stack (float/double function results).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_x87_push(st: *mut CpuState, v: f64) {
    let x = unsafe { &mut (*st).x87 };
    x.top = (x.top + 7) & 7;
    x.st[x.top as usize] = v;
    x.empty &= !(1 << x.top);
}

/// Pop st(0).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_x87_pop(st: *mut CpuState) -> f64 {
    let x = unsafe { &mut (*st).x87 };
    let v = x.st[x.top as usize];
    x.empty |= 1 << x.top;
    x.top = (x.top + 1) & 7;
    v
}

/// Executed instruction count of this state (statistics).
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_icount(st: *const CpuState) -> u64 {
    unsafe { (*st).icount }
}

/// Print JIT statistics (DXCPU_STATS=1): interpreter fallbacks per mnemonic.
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_print_stats() {
    use std::sync::atomic::Ordering;
    if !crate::jit::STATS_ON.load(Ordering::Relaxed) {
        return;
    }
    let mut v: Vec<(u64, usize)> = crate::jit::FALLBACK_COUNT
        .iter()
        .enumerate()
        .map(|(i, c)| (c.swap(0, Ordering::Relaxed), i))
        .filter(|(c, _)| *c > 0)
        .collect();
    v.sort_by(|a, b| b.cmp(a));
    let total: u64 = v.iter().map(|(c, _)| c).sum();
    eprintln!(
        "dxcpu: {} dispatcher entries, {} interpreted instructions",
        crate::jit::DISPATCH_COUNT.swap(0, Ordering::Relaxed),
        total
    );
    for (c, i) in v.iter().take(25) {
        let m: iced_x86::Mnemonic = unsafe { std::mem::transmute(*i as u16) };
        eprintln!("  fallback {:>12} x {:?}", c, m);
    }
}
