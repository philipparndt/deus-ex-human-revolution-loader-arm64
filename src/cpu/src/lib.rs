//! dxcpu: native i386 execution engine for dxhost (arm64 hosts).
//!
//! Stage 1 is a reference interpreter over iced-x86 decoded instructions,
//! validated against Unicorn (differential tests). A JIT that translates hot
//! code to arm64 builds on top of it and falls back to the interpreter for
//! rare instructions.

pub mod ffi;
pub mod fpu;
pub mod interp;
pub mod mem;
pub mod sse;
pub mod state;

pub use state::{CpuState, Exit};
