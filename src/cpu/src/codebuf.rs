//! Executable code memory for the JIT (Apple Silicon: MAP_JIT + per-thread
//! write protection toggling + instruction cache invalidation).

use std::ptr;

unsafe extern "C" {
    fn pthread_jit_write_protect_np(enabled: i32);
    fn sys_icache_invalidate(start: *mut u8, len: usize);
}

const MAP_JIT: i32 = 0x0800;

pub struct CodeBuf {
    base: *mut u8,
    size: usize,
    used: usize,
}

unsafe impl Send for CodeBuf {}

impl CodeBuf {
    pub fn new(size: usize) -> Self {
        let p = unsafe {
            libc_mmap(
                ptr::null_mut(),
                size,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_JIT,
                -1,
                0,
            )
        };
        if p as isize == -1 {
            panic!("codebuf: mmap(MAP_JIT) failed");
        }
        CodeBuf { base: p as *mut u8, size, used: 0 }
    }

    /// Address the next `add` will place code at.
    pub fn next_addr(&self) -> usize {
        self.base as usize + ((self.used + 15) & !15)
    }

    pub fn remaining(&self) -> usize {
        self.size - self.used
    }

    /// Copy instructions into executable memory; returns their address.
    pub fn add(&mut self, code: &[u32]) -> Option<*const u8> {
        let bytes = code.len() * 4;
        let aligned = (self.used + 15) & !15;
        if aligned + bytes > self.size {
            return None;
        }
        let dst = unsafe { self.base.add(aligned) };
        unsafe {
            pthread_jit_write_protect_np(0);
            ptr::copy_nonoverlapping(code.as_ptr() as *const u8, dst, bytes);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(dst, bytes);
        }
        self.used = aligned + bytes;
        Some(dst)
    }

    /// Patch one instruction word in place (block chaining).
    pub fn patch(&mut self, at: *mut u32, insn: u32) {
        unsafe {
            pthread_jit_write_protect_np(0);
            at.write(insn);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(at as *mut u8, 4);
        }
    }

    pub fn reset(&mut self) {
        self.used = 0;
    }

    pub fn contains(&self, p: *const u8) -> bool {
        p >= self.base && (p as usize) < self.base as usize + self.size
    }
}

impl Drop for CodeBuf {
    fn drop(&mut self) {
        unsafe { libc_munmap(self.base, self.size) };
    }
}

const PROT_READ: i32 = 1;
const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;
const MAP_PRIVATE: i32 = 2;
const MAP_ANON: i32 = 0x1000;

unsafe extern "C" {
    #[link_name = "mmap"]
    fn libc_mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    #[link_name = "munmap"]
    fn libc_munmap(addr: *mut u8, len: usize) -> i32;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::a64::*;

    #[test]
    fn run_generated_code() {
        // u32 f(u32 a, u32 b) { return (a + b) * 3 - (a >> 2); }
        let mut a = Asm::new();
        a.add_w(2, 0, 1);
        a.mov_w_imm(3, 3);
        a.mul_w(2, 2, 3);
        a.lsr_w_imm(4, 0, 2);
        a.sub_w(0, 2, 4);
        a.ret();
        let mut cb = CodeBuf::new(1 << 16);
        let p = cb.add(&a.code).unwrap();
        let f: extern "C" fn(u32, u32) -> u32 = unsafe { std::mem::transmute(p) };
        assert_eq!(f(10, 20), 90 - 2);
        assert_eq!(f(0xffff_fff0, 0x20), (0x10u32).wrapping_mul(3).wrapping_sub(0x3fff_fffc));
    }

    #[test]
    fn memory_and_flags() {
        // u32 f(u8 *base, u32 idx) { u32 v = *(u32*)(base+idx); return v == 0x11223344; }
        let mut a = Asm::new();
        a.ldr_uxtw(2, 2, 0, 1);
        a.mov_w_imm(3, 0x1122_3344);
        a.cmp_w(2, 3);
        a.cset_w(0, Cond::Eq);
        a.ret();
        let mut cb = CodeBuf::new(1 << 16);
        let p = cb.add(&a.code).unwrap();
        let f: extern "C" fn(*const u8, u32) -> u32 = unsafe { std::mem::transmute(p) };
        let buf = [0u8, 0, 0, 0, 0x44, 0x33, 0x22, 0x11];
        assert_eq!(f(buf.as_ptr(), 4), 1);
        assert_eq!(f(buf.as_ptr(), 0), 0);
    }

    #[test]
    fn branches() {
        // u32 f(u32 n) { u32 s = 0; while (n) { s += n; n--; } return s; }
        let mut a = Asm::new();
        a.mov_w_imm(1, 0);
        let top = a.pos();
        let done = a.cbz_w_fixup(0);
        a.add_w(1, 1, 0);
        a.sub_w_imm(0, 0, 1);
        a.b_to(top);
        a.bind(done);
        a.mov_w(0, 1);
        a.ret();
        let mut cb = CodeBuf::new(1 << 16);
        let p = cb.add(&a.code).unwrap();
        let f: extern "C" fn(u32) -> u32 = unsafe { std::mem::transmute(p) };
        assert_eq!(f(100), 5050);
    }
}
