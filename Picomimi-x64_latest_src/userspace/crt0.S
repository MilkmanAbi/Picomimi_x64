# Picomimi-x64 C Runtime Entry Point
# crt0.S — called by kernel execve after ELF load
#
# Stack layout at entry (Linux ABI):
#   rsp → argc
#          argv[0] ... argv[argc-1]
#          NULL
#          envp[0] ... envp[n-1]
#          NULL
#          auxv...

.section .text
.global _start
.type _start, @function

_start:
    # Clear frame pointer (signals to backtrace that this is the bottom)
    xorq    %rbp, %rbp

    # rsp points at argc — load it
    movq    (%rsp),  %rdi    # argc
    leaq    8(%rsp), %rsi    # argv
    leaq    8(%rsi,%rdi,8), %rdx  # envp = argv + argc + 1

    # Call main(argc, argv, envp)
    call    main

    # main returned — call exit(retval)
    movq    %rax, %rdi
    call    _exit
    # Should not reach here; just halt
1:  hlt
    jmp 1b

.size _start, .-_start

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
