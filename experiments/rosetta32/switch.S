// uint32_t call32(uint32_t fn, uint32_t stack_top, uint32_t cs32)
// Switches to the 32-bit code segment and jumps to fn with esp = stack_top.
// The 32-bit code returns through a gate that far-jumps back to back64.
    .text
    .globl _call32
_call32:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, _saved_rsp64(%rip)
    mov %esi, %esp          // 32-bit stack (zero-extends into rsp)
    push %rdx               // CS for lretq
    push %rdi               // RIP for lretq
    lretq

    .globl _back64
_back64:
    mov _saved_rsp64(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
