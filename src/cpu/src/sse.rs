//! SSE/SSE2/SSE3/SSSE3 and MMX (filled in separately).

use crate::interp::ExecResult;
use crate::state::CpuState;
use iced_x86::Instruction;

/// Execute an SSE/MMX instruction; None if `insn` is not handled here.
pub fn exec(_st: &mut CpuState, _insn: &Instruction) -> Option<ExecResult> {
    None
}
