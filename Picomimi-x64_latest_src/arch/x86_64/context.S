/**
 * Picomimi-x64 Context Switch
 * 
 * Low-level task switching routines
 */

.section .text
.code64

.extern per_cpu_bsp_addr

// ============================================================================
// CONTEXT SWITCH
// context_switch(old_context, new_context)
// 
// old_context in %rdi
// new_context in %rsi
// ============================================================================

.global context_switch
context_switch:
    // Save callee-saved registers to old context
    mov %r15, 0(%rdi)
    mov %r14, 8(%rdi)
    mov %r13, 16(%rdi)
    mov %r12, 24(%rdi)
    mov %rbp, 32(%rdi)
    mov %rbx, 40(%rdi)
    
    // Save return address (we'll jump back here on restore)
    lea 1f(%rip), %rax
    mov %rax, 48(%rdi)
    
    // Save stack pointer
    mov %rsp, 56(%rdi)
    
    // Save flags
    pushfq
    pop %rax
    mov %rax, 64(%rdi)
    
    // ---- Now switch to new context ----
    
    // Restore stack pointer first
    mov 56(%rsi), %rsp
    
    // Restore flags
    mov 64(%rsi), %rax
    push %rax
    popfq
    
    // Restore callee-saved registers
    mov 0(%rsi), %r15
    mov 8(%rsi), %r14
    mov 16(%rsi), %r13
    mov 24(%rsi), %r12
    mov 32(%rsi), %rbp
    mov 40(%rsi), %rbx
    
    // Jump to saved return address
    jmp *48(%rsi)

1:
    // Return point for context that was switched away from
    ret

// ============================================================================
// SWITCH TO USER MODE
// switch_to_user(trap_frame)
// 
// trap_frame in %rdi - pointer to user's saved registers
// ============================================================================

.global switch_to_user
switch_to_user:
    // Load trap frame pointer
    mov %rdi, %rsp
    
    // Restore general registers
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    
    // Skip int_no and error_code
    add $16, %rsp
    
    // Ensure correct GS state before entering user space:
    //   GS.base (0xC0000101) = 0  (user has no kernel GS)
    //   KERNEL_GS_BASE (0xC0000102) = per_cpu_bsp  (for swapgs on next syscall)
    // We reload from per_cpu_bsp which is the canonical value.
    push %rax
    push %rcx
    push %rdx
    xor %eax, %eax
    xor %edx, %edx
    mov $0xC0000101, %ecx           // IA32_GS_BASE = 0
    wrmsr
    // Load per_cpu_bsp address into KERNEL_GS_BASE
    mov per_cpu_bsp_addr(%rip), %rax
    mov %rax, %rdx
    shr $32, %rdx
    and $0xFFFFFFFF, %eax
    mov $0xC0000102, %ecx           // IA32_KERNEL_GS_BASE = per_cpu_bsp
    wrmsr
    pop %rdx
    pop %rcx
    pop %rax

    // iretq pops: RIP, CS, RFLAGS, RSP, SS
    iretq

// ============================================================================
// KERNEL THREAD ENTRY
// Entry point for newly created kernel threads
// ============================================================================

.global kthread_entry
kthread_entry:
    // Enable interrupts
    sti
    
    // Pop function argument and entry point from stack
    pop %rdi        // arg
    pop %rax        // entry point
    
    // Call the entry function
    call *%rax
    
    // If thread returns, exit with return value
    mov %rax, %rdi
    call sys_exit
    
    // Should never reach here
1:  cli
    hlt
    jmp 1b

// ============================================================================
// FORK RETURN TO USER
// Entered when a newly forked child task is first scheduled.
// The child's kernel stack has a fork_sc_frame_t pushed by task_fork.
// We restore registers and iretq to user space with rax=0.
// ============================================================================

// Redefine the pop sequence inline (RESTORE_REGS from interrupts.S macros)
.global fork_return_to_user
fork_return_to_user:
    pop %rax        // rax = 0 (fork return value for child)
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    add $16, %rsp   // skip vector and error_code fields
    swapgs          // switch GS back to user GS before entering ring-3
    iretq           // pops: rip, cs, rflags, user_rsp, ss → jumps to user

// ============================================================================
// FORK RETURN
// Child process returns here after context_switch (legacy path, not used)
// ============================================================================

.global ret_from_fork
ret_from_fork:
    // Schedule tail (bookkeeping after context switch)
    // mov %rdi, %rdi  // prev task already in rdi
    call schedule_tail
    
    // Check for pending signals
    // call do_signal
    
    // Return to user (or kernel thread)
    // Get current task's trap frame
    call get_current_task
    mov (%rax), %rdi    // trap_frame pointer
    
    test %rdi, %rdi
    jz 1f
    
    // If trap frame exists, return to user
    jmp switch_to_user

1:
    // No trap frame - kernel thread, just return
    ret

// ============================================================================
// SYSCALL DISPATCH (called from interrupts.S)
// syscall_dispatch_asm(regs)
// ============================================================================

.global syscall_dispatch_asm
syscall_dispatch_asm:
    // regs pointer in %rdi
    
    // Get syscall number from RAX in regs
    // regs->rax is at offset 14*8 = 112 (after r15-rax saves)
    mov 112(%rdi), %rax
    
    // Check syscall number bounds
    cmp $512, %rax
    jae .bad_syscall
    
    // Load syscall table entry
    lea syscall_table(%rip), %r10
    mov (%r10, %rax, 8), %r10
    
    // Load arguments from regs
    // Args are in: rdi, rsi, rdx, r10, r8, r9 (Linux x86_64 ABI)
    // But they're saved at offsets in regs structure
    
    // regs layout: r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax
    // Offsets:     0   8   16  24  32  40  48 56 64  72  80  88  96  104 112
    
    push %rdi           // Save regs pointer
    
    mov 72(%rdi), %r9   // 6th arg (was rdi, but r9 in syscall)
    mov 80(%rdi), %r8   // 5th arg (was rsi, but r8 in syscall)
    mov 48(%rdi), %rcx  // 4th arg (r10 in syscall -> rcx for function call)
    mov 88(%rdi), %rdx  // 3rd arg
    mov 80(%rdi), %rsi  // 2nd arg
    mov 72(%rdi), %rdi  // 1st arg
    
    // Actually, we need to reload from saved user registers
    // This is getting complex - let's use the C dispatcher instead
    
    pop %rdi
    call syscall_dispatch
    
    // Store return value in regs->rax
    // rdi still has regs pointer from C call
    // Actually no, rdi might be clobbered. Let's handle in C.
    
    ret

.bad_syscall:
    mov $-38, %rax      // -ENOSYS
    mov %rax, 112(%rdi) // Store in regs->rax
    ret

.section .note.GNU-stack, "", @progbits
