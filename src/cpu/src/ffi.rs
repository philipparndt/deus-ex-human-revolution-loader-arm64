//! C interface (see src/host/dxcpu.h).

use crate::interp::Engine;
use crate::mem;
use crate::state::CpuState;
use iced_x86::{Decoder, DecoderOptions, Formatter, IntelFormatter};

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_set_guest_base(base: *mut u8) {
    mem::set_base(base);
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_engine_new(stub_lo: u32, stub_hi: u32) -> *mut Engine {
    Box::into_raw(Box::new(Engine::new(stub_lo, stub_hi)))
}

#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_engine_flush(eng: *mut Engine) {
    unsafe { (*eng).flush() }
}

/// Run until eip == stop or an exit; returns the Exit reason.
#[unsafe(no_mangle)]
pub extern "C" fn dxcpu_run(eng: *mut Engine, st: *mut CpuState, stop: u32) -> u32 {
    let (eng, st) = unsafe { (&mut *eng, &mut *st) };
    let e = eng.run(st, stop);
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
