//! Guest memory access: the guest's 4 GB address space lives at `base` in the
//! host. Addresses wrap at 32 bits like on real hardware.

use std::sync::atomic::{AtomicPtr, AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};

static BASE: AtomicPtr<u8> = AtomicPtr::new(std::ptr::null_mut());

pub fn set_base(p: *mut u8) {
    BASE.store(p, Ordering::Relaxed);
}

#[inline(always)]
pub fn base() -> *mut u8 {
    BASE.load(Ordering::Relaxed)
}

#[inline(always)]
pub fn ptr(addr: u32) -> *mut u8 {
    unsafe { base().add(addr as usize) }
}

#[inline(always)]
pub fn r8(a: u32) -> u8 {
    unsafe { ptr(a).read() }
}
#[inline(always)]
pub fn r16(a: u32) -> u16 {
    unsafe { (ptr(a) as *const u16).read_unaligned() }
}
#[inline(always)]
pub fn r32(a: u32) -> u32 {
    unsafe { (ptr(a) as *const u32).read_unaligned() }
}
#[inline(always)]
pub fn r64(a: u32) -> u64 {
    unsafe { (ptr(a) as *const u64).read_unaligned() }
}
#[inline(always)]
pub fn r128(a: u32) -> [u8; 16] {
    unsafe { (ptr(a) as *const [u8; 16]).read_unaligned() }
}
#[inline(always)]
pub fn w8(a: u32, v: u8) {
    unsafe { ptr(a).write(v) }
}
#[inline(always)]
pub fn w16(a: u32, v: u16) {
    unsafe { (ptr(a) as *mut u16).write_unaligned(v) }
}
#[inline(always)]
pub fn w32(a: u32, v: u32) {
    unsafe { (ptr(a) as *mut u32).write_unaligned(v) }
}
#[inline(always)]
pub fn w64(a: u32, v: u64) {
    unsafe { (ptr(a) as *mut u64).write_unaligned(v) }
}
#[inline(always)]
pub fn w128(a: u32, v: [u8; 16]) {
    unsafe { (ptr(a) as *mut [u8; 16]).write_unaligned(v) }
}

/// Read `size` bytes (1, 2 or 4) zero-extended.
#[inline(always)]
pub fn rd(a: u32, size: u32) -> u32 {
    match size {
        1 => r8(a) as u32,
        2 => r16(a) as u32,
        _ => r32(a),
    }
}
#[inline(always)]
pub fn wr(a: u32, size: u32, v: u32) {
    match size {
        1 => w8(a, v as u8),
        2 => w16(a, v as u16),
        _ => w32(a, v),
    }
}

/// Atomic read-modify-write on guest memory (for `lock` prefixed instructions).
/// Falls back to a plain (non-atomic) access for misaligned addresses, which
/// x86 permits but which no compiler-generated lock emits in practice.
pub fn atomic_rmw(a: u32, size: u32, f: &mut dyn FnMut(u32) -> u32) -> u32 {
    let p = ptr(a);
    unsafe {
        match size {
            1 => {
                let at = &*(p as *const AtomicU8);
                let mut old = at.load(Ordering::SeqCst);
                loop {
                    let new = f(old as u32) as u8;
                    match at.compare_exchange_weak(old, new, Ordering::SeqCst, Ordering::SeqCst) {
                        Ok(_) => return old as u32,
                        Err(cur) => old = cur,
                    }
                }
            }
            2 if a % 2 == 0 => {
                let at = &*(p as *const AtomicU16);
                let mut old = at.load(Ordering::SeqCst);
                loop {
                    let new = f(old as u32) as u16;
                    match at.compare_exchange_weak(old, new, Ordering::SeqCst, Ordering::SeqCst) {
                        Ok(_) => return old as u32,
                        Err(cur) => old = cur,
                    }
                }
            }
            4 if a % 4 == 0 => {
                let at = &*(p as *const AtomicU32);
                let mut old = at.load(Ordering::SeqCst);
                loop {
                    let new = f(old);
                    match at.compare_exchange_weak(old, new, Ordering::SeqCst, Ordering::SeqCst) {
                        Ok(_) => return old,
                        Err(cur) => old = cur,
                    }
                }
            }
            _ => {
                let old = rd(a, size);
                wr(a, size, f(old));
                old
            }
        }
    }
}

/// Atomic 64-bit compare-exchange (cmpxchg8b). Returns (success, old value).
pub fn atomic_cas64(a: u32, expected: u64, new: u64) -> (bool, u64) {
    let p = ptr(a);
    if a % 8 == 0 {
        let at = unsafe { &*(p as *const AtomicU64) };
        match at.compare_exchange(expected, new, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(v) => (true, v),
            Err(v) => (false, v),
        }
    } else {
        let old = r64(a);
        if old == expected {
            w64(a, new);
            (true, old)
        } else {
            (false, old)
        }
    }
}
