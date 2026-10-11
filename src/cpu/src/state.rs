//! Guest CPU state (shared with C as `struct dxcpu_state`, see dxcpu.h).

/// EFLAGS bits.
pub mod flags {
    pub const CF: u32 = 1 << 0;
    pub const PF: u32 = 1 << 2;
    pub const AF: u32 = 1 << 4;
    pub const ZF: u32 = 1 << 6;
    pub const SF: u32 = 1 << 7;
    pub const TF: u32 = 1 << 8;
    pub const IF: u32 = 1 << 9;
    pub const DF: u32 = 1 << 10;
    pub const OF: u32 = 1 << 11;
    pub const ID: u32 = 1 << 21;
    pub const ARITH: u32 = CF | PF | AF | ZF | SF | OF;
}

pub const EAX: usize = 0;
pub const ECX: usize = 1;
pub const EDX: usize = 2;
pub const EBX: usize = 3;
pub const ESP: usize = 4;
pub const EBP: usize = 5;
pub const ESI: usize = 6;
pub const EDI: usize = 7;

/// Why `run` returned.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exit {
    /// eip reached the requested stop address.
    Stop = 0,
    /// eip entered the thunk stub range; the caller dispatches to the host.
    Stub = 1,
    /// A guest fault (bad memory access, invalid opcode, ...). Details in fault_*.
    Fault = 2,
    /// An instruction the translator does not implement yet.
    Unimplemented = 3,
    /// The guest executed `hlt` or a breakpoint.
    Halt = 4,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct X87 {
    /// Physical registers (values kept as f64; 80-bit precision is not modelled).
    pub st: [f64; 8],
    pub top: u32,
    /// Bit i set = physical register i is empty.
    pub empty: u32,
    pub cw: u16,
    pub sw: u16,
}

#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct CpuState {
    pub gpr: [u32; 8],
    pub eip: u32,
    pub eflags: u32,
    pub xmm: [[u8; 16]; 8],
    pub mxcsr: u32,
    pub x87: X87,
    /// Exit information (filled when run returns).
    pub exit_reason: u32,
    pub fault_addr: u32,
    pub fault_eip: u32,
    pub pad: u32,
    /// Executed instruction counter (statistics).
    pub icount: u64,
}

impl CpuState {
    pub fn new() -> Self {
        CpuState {
            gpr: [0; 8],
            eip: 0,
            eflags: 0x202,
            xmm: [[0; 16]; 8],
            mxcsr: 0x1f80,
            x87: X87 { st: [0.0; 8], top: 0, empty: 0xff, cw: 0x37f, sw: 0 },
            exit_reason: 0,
            fault_addr: 0,
            fault_eip: 0,
            pad: 0,
            icount: 0,
        }
    }
}

impl Default for CpuState {
    fn default() -> Self {
        Self::new()
    }
}
