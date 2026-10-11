// Rosetta backend mode switches (x86_64 host <-> i386 compatibility mode).
//
// Guest -> host: a thunk stub does `mov eax, id; jmp gate32`; the gate far-jumps
// to a 64-bit trampoline that jumps to _rosetta_host_entry with rsp still on
// the guest stack. We build a struct guest_regs there, call host_dispatch and
// far-return to the guest return address.
//
// Host -> guest: _rosetta_enter32 saves the host context in a frame recorded in
// the thread-local _rosetta_cur_frame, switches to the guest stack (whose top
// holds the return-gate address) and far-returns into 32-bit code. When the
// guest function returns, the return gate lands in _rosetta_return64, which
// restores the host context from the frame.

#define REGS_SIZE 64
// struct guest_regs offsets
#define R_EAX 0
#define R_ECX 4
#define R_EDX 8
#define R_EBX 12
#define R_ESP 16
#define R_EBP 20
#define R_ESI 24
#define R_EDI 28
#define R_ID 32
#define R_POP 36
#define R_FPRET 40
#define R_RET 48
#define R_FRET 56

// struct rosetta_frame offsets
#define F_SAVED_RSP 0
#define F_RET 16
#define F_FRET 24
#define F_FP 32

    .text

    .globl _rosetta_host_entry
_rosetta_host_entry:
    mov %esp, %esp              // clear undefined upper halves after the mode switch
    mov %eax, %eax
    mov %rsp, %r11              // r11 = guest esp (points at the return address)
    sub $(REGS_SIZE + 32), %rsp
    and $-16, %rsp
    mov %eax, R_ID(%rsp)
    movl $0, R_EAX(%rsp)
    mov %ecx, R_ECX(%rsp)
    mov %edx, R_EDX(%rsp)
    mov %ebx, R_EBX(%rsp)
    mov %r11d, R_ESP(%rsp)
    mov %ebp, R_EBP(%rsp)
    mov %esi, R_ESI(%rsp)
    mov %edi, R_EDI(%rsp)
    movl $0, R_POP(%rsp)
    movl $0, R_FPRET(%rsp)
    movq $0, R_RET(%rsp)
    mov %rsp, %rdi
    cld
    call _host_dispatch
    mov R_ESI(%rsp), %esi
    mov R_EDI(%rsp), %edi
    cmpl $0, R_FPRET(%rsp)
    je 1f
    fldl R_FRET(%rsp)
1:
    mov R_RET(%rsp), %eax
    mov R_RET+4(%rsp), %edx
    mov R_ESP(%rsp), %r11d      // guest esp
    mov R_POP(%rsp), %ecx
    mov (%r11), %r10d           // guest return address
    lea 4(%r11,%rcx), %r11      // esp after `ret pop`
    lea -16(%r11), %rsp
    mov %r10, (%rsp)
    mov _rosetta_cs32(%rip), %r10
    mov %r10, 8(%rsp)
    lretq

// void rosetta_enter32(uint32_t eip, uint32_t esp, struct rosetta_frame *frame)
    .globl _rosetta_enter32
_rosetta_enter32:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp
    mov %rsp, F_SAVED_RSP(%rdx)
    mov %esi, %esp
    mov _rosetta_cs32(%rip), %rax
    push %rax
    push %rdi
    lretq

    .globl _rosetta_return64
_rosetta_return64:
    mov %eax, %eax
    mov %edx, %edx
    mov %esp, %esp
    and $-16, %rsp
    push %rax
    push %rdx
    movq _rosetta_cur_frame@TLVP(%rip), %rdi
    call *(%rdi)                // rax = &rosetta_cur_frame
    mov (%rax), %rdi            // rdi = frame
    pop %rdx
    pop %rax
    mov %eax, F_RET(%rdi)
    mov %edx, F_RET+4(%rdi)
    cmpl $0, F_FP(%rdi)
    je 1f
    fstpl F_FRET(%rdi)
1:
    mov F_SAVED_RSP(%rdi), %rsp
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

// void rosetta_jump32(uint32_t eip, uint32_t esp) -- never returns
    .globl _rosetta_jump32
_rosetta_jump32:
    mov %esi, %esp
    mov _rosetta_cs32(%rip), %rax
    push %rax
    push %rdi
    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    xor %ebp, %ebp
    lretq

// Reserve the guest's 4 GB window at exec time, before dyld or malloc can
// place anything there. gmem_init() replaces it with its own mappings.
    .zerofill __GUEST, __reserve, _guest_reserve, 0xFFFE8000, 14
