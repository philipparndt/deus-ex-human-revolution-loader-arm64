# dxcpu JIT design (i386 → AArch64)

Goal: run the game's 32-bit x86 code natively on Apple Silicon at a usable
speed, with the interpreter (`interp.rs`, `fpu.rs`, `sse.rs`) as the reference
and as a fallback for rare instructions. Unicorn is the oracle for testing
(`examples/difftest*.rs`).

## Register mapping (inside translated code)

| host        | use                                           |
|-------------|-----------------------------------------------|
| w19–w26     | guest EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI  |
| x27         | guest memory base (`guest_base`)              |
| x28         | `JitCtx*` (guest eip, lazy flags, helpers)    |
| x0–x17      | scratch / helper call arguments               |
| x18         | reserved by macOS — never touched             |

All guest registers live in callee-saved host registers, so calls to Rust
helpers do not need to spill them (only flags and eip are synced).

## Memory access

A guest access `[addr]` becomes one instruction:
`ldr wN, [x27, wADDR, uxtw]` (zero-extended 32-bit index), likewise for stores
and 8/16-bit sizes. Address arithmetic wraps at 32 bits like on x86.

## Flags

x86 arithmetic is translated to flag-setting AArch64 instructions. 8/16-bit
operations shift their operands to the top of a 32-bit register first, so N,
Z, C and V are correct for the narrow size. The translator tracks statically
which operation produced the current NZCV:

* after SUB/CMP: ZF=Z, SF=N, OF=V, CF=!C → every x86 condition maps directly to
  an AArch64 condition (jb→LO, ja→HI, jl→LT, …);
* after ADD: CF=C;
* after AND/OR/XOR/TEST (ANDS): C=V=0.

PF and AF are only computed when something reads them (`jp` after `ucomiss`,
`pushf`, `lahf`, helper fallback). At block exits, flags are stored as a lazy
descriptor (operation kind, NZCV, result) in `JitCtx`; a successor block or
the interpreter materialises full EFLAGS only when it actually reads them.

## Blocks

* Translation unit: a basic block (ends at a jump/call/ret or after N
  instructions), the same decoding as the interpreter.
* Direct branches end in exit stubs that are patched into direct branches to
  the target block once it is translated (chaining). Indirect branches go
  through a small lookup (hash of eip → code) before falling back to the
  dispatcher.
* Thunk stubs (guest addresses in the stub range) are never translated: the
  dispatcher returns `Exit::Stub` to C, which calls the host function.
* Unsupported instructions: guest registers are written back to the context,
  `jit_interp_one(ctx, insn)` executes the single instruction with the
  interpreter, registers are reloaded. This keeps everything correct while
  hot instructions get native translations over time.

## Code memory

`codebuf.rs`: `mmap(MAP_JIT)`, `pthread_jit_write_protect_np` around writes,
`sys_icache_invalidate`. One engine (block cache + code buffer) per guest
thread and host→guest nesting depth.

## SSE / x87

Phase 1: through the interpreter fallback. Phase 2: SSE scalar/packed float
ops map to NEON/FP (XMM registers cached in v16–v23 within a block, written
back at exits and helper calls); x87 stays mostly interpreted (games built
with SSE use x87 mainly for function return values).
