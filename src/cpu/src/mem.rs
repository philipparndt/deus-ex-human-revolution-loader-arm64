//! Guest memory access: the guest's 4 GB address space lives at `base` in the
//! host. Addresses wrap at 32 bits like on real hardware.

use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU8, AtomicU16, AtomicU32, AtomicU64, Ordering};

// Write log for the JIT verifier (DXCPU_VERIFY): while a thread records, every
// write through this module saves the old bytes first so they can be restored.
pub static UNDO_ON: AtomicBool = AtomicBool::new(false);
/// x86 TSO ordering for interpreted accesses (see jit::tso_on).
pub static TSO: AtomicBool = AtomicBool::new(false);

// As in translated code: dmb ishld after loads (orders them before later loads
// and stores), dmb ishst before stores. A Rust release fence would be a full
// dmb ish on every store.
#[inline(always)]
fn tso_load() {
    if TSO.load(Ordering::Relaxed) {
        #[cfg(target_arch = "aarch64")]
        unsafe {
            std::arch::asm!("dmb ishld", options(nostack, preserves_flags))
        };
        #[cfg(not(target_arch = "aarch64"))]
        std::sync::atomic::fence(Ordering::Acquire);
    }
}
#[inline(always)]
fn tso_store() {
    if TSO.load(Ordering::Relaxed) {
        #[cfg(target_arch = "aarch64")]
        unsafe {
            std::arch::asm!("dmb ishst", options(nostack, preserves_flags))
        };
        #[cfg(not(target_arch = "aarch64"))]
        std::sync::atomic::fence(Ordering::Release);
    }
}
thread_local! {
    static RECORDING: Cell<bool> = const { Cell::new(false) };
    static UNDO: RefCell<Vec<(u32, u8)>> = const { RefCell::new(Vec::new()) };
}

#[inline(always)]
fn log(a: u32, n: u32) {
    if UNDO_ON.load(Ordering::Relaxed) {
        log_slow(a, n);
    }
}
#[cold]
fn log_slow(a: u32, n: u32) {
    if RECORDING.with(|r| r.get()) {
        UNDO.with(|u| {
            let mut u = u.borrow_mut();
            for i in 0..n {
                let b = a.wrapping_add(i);
                u.push((b, unsafe { ptr(b).read() }));
            }
        });
    }
}

/// Start recording writes on this thread.
pub fn undo_begin() {
    UNDO.with(|u| u.borrow_mut().clear());
    RECORDING.with(|r| r.set(true));
}

/// Stop recording, restore the old bytes and return the written addresses with
/// the values they had before restoring.
pub fn undo_rollback() -> Vec<(u32, u8)> {
    RECORDING.with(|r| r.set(false));
    UNDO.with(|u| {
        let log = std::mem::take(&mut *u.borrow_mut());
        let mut written: Vec<(u32, u8)> = Vec::new();
        let mut seen = std::collections::HashSet::new();
        for &(a, _) in &log {
            if seen.insert(a) {
                written.push((a, unsafe { ptr(a).read() }));
            }
        }
        for &(a, old) in log.iter().rev() {
            unsafe { ptr(a).write(old) };
        }
        written
    })
}

static BASE: AtomicPtr<u8> = AtomicPtr::new(std::ptr::null_mut());

static MAPPED_QUERY: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());

pub fn set_mapped_query(f: extern "C" fn(u32, u32) -> bool) {
    MAPPED_QUERY.store(f as *mut (), Ordering::Relaxed);
}

/// May [a, a+len) be read without faulting? Without a host query: unknown, false.
pub fn readable(a: u32, len: u32) -> bool {
    let f = MAPPED_QUERY.load(Ordering::Relaxed);
    if f.is_null() {
        return false;
    }
    let f: extern "C" fn(u32, u32) -> bool = unsafe { std::mem::transmute(f) };
    f(a, len)
}

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
    let v = unsafe { ptr(a).read() };
    tso_load();
    v
}
#[inline(always)]
pub fn r16(a: u32) -> u16 {
    let v = unsafe { (ptr(a) as *const u16).read_unaligned() };
    tso_load();
    v
}
#[inline(always)]
pub fn r32(a: u32) -> u32 {
    let v = unsafe { (ptr(a) as *const u32).read_unaligned() };
    tso_load();
    v
}
#[inline(always)]
pub fn r64(a: u32) -> u64 {
    let v = unsafe { (ptr(a) as *const u64).read_unaligned() };
    tso_load();
    v
}
#[inline(always)]
pub fn r128(a: u32) -> [u8; 16] {
    let v = unsafe { (ptr(a) as *const [u8; 16]).read_unaligned() };
    tso_load();
    v
}
#[inline(always)]
pub fn w8(a: u32, v: u8) {
    log(a, 1);
    tso_store();
    unsafe { ptr(a).write(v) }
}
#[inline(always)]
pub fn w16(a: u32, v: u16) {
    log(a, 2);
    tso_store();
    unsafe { (ptr(a) as *mut u16).write_unaligned(v) }
}
#[inline(always)]
pub fn w32(a: u32, v: u32) {
    log(a, 4);
    tso_store();
    unsafe { (ptr(a) as *mut u32).write_unaligned(v) }
}
#[inline(always)]
pub fn w64(a: u32, v: u64) {
    log(a, 8);
    tso_store();
    unsafe { (ptr(a) as *mut u64).write_unaligned(v) }
}
#[inline(always)]
pub fn w128(a: u32, v: [u8; 16]) {
    log(a, 16);
    tso_store();
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
    log(a, size);
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
    log(a, 8);
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
