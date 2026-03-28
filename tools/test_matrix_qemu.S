/*
 * test_matrix_qemu.S
 * 
 * Userspace Micro-Hypervisor (Trampoline Passthrough)
 * Demonstrates intercepting all syscalls made inside the Matrix sandbox
 * and securely executing them dynamically inside the host Userspace process.
 */

.global _start

.section .data
    .align 64
matrix_exit_info:
    .quad 0 /* exit_reason */
    .quad 0 /* rax */
    .quad 0 /* rdi */
    .quad 0 /* rsi */
    .quad 0 /* rdx */
    .quad 0 /* r10 */
    .quad 0 /* r8  */
    .quad 0 /* r9  */
    .quad 0 /* guest_rip */

dev_path:
    .asciz "/dev/ntp_sync"

msg_start:
    .asciz "[Micro-QEMU] Host Trampoline initializing...\n"
msg_matrix_start:
    .asciz "[Matrix-Guest] GUEST TARGET AWAKE! Executing write() natively via proxy!\n"
msg_matrix_done:
    .asciz "[Matrix-Guest] Matrix execution complete. Exiting.\n"

.section .text
_start:
    /* Print "[Micro-QEMU] Host Trampoline initializing...\n" */
    mov $1, %rax        /* sys_write */
    mov $1, %rdi        /* stdout */
    lea msg_start(%rip), %rsi
    mov $45, %rdx       /* length */
    syscall

    /* 1. Open /dev/ntp_sync portal */
    mov $2, %rax        /* sys_open */
    lea dev_path(%rip), %rdi
    mov $2, %rsi        /* O_RDWR */
    syscall
    
    cmp $0, %rax
    jl fail
    
    mov %rax, %r12      /* store fd in r12! */

matrix_trampoline_loop:
    /* Call ioctl: ENTER / RESUME MATRIX */
    mov $16, %rax       /* sys_ioctl */
    mov %r12, %rdi      /* fd */
    mov $0x5301, %rsi   /* SVM_IOCTL_ENTER_MATRIX */
    lea matrix_exit_info(%rip), %rdx /* arg: exit info struct */
    syscall

    /* Did the ioctl call fail entirely? */
    cmp $0, %rax
    jl matrix_done

    /* Check if the caller is the MATRIX Target, or the HOST Trampoline!
     * 0 = We are the HOST Trampoline (VMRUN exited).
     * 1 = We are the MATRIX GUEST (Initial Fork point!).
     */
    cmp $1, %rax
    je matrix_target_entry

    /* If RAX == 0, we are the HOST. Check exit_reason to perform proxy. */
    lea matrix_exit_info(%rip), %rbx
    mov 0(%rbx), %r14
    cmp $1, %r14        /* MATRIX_EXIT_REASON_SYSCALL == 1 */
    jne matrix_done     /* If exit was arbitrary (signals, etc), abandon loop. */

    /* 
     * THE MAGIC TRAP: HOST PROXY TRAMPOLINE!
     * The Matrix requested us to securely execute a syscall for it!
     */
    mov 8(%rbx), %rax   /* RAX (syscall nr) */
    mov 16(%rbx), %rdi  /* RDI */
    mov 24(%rbx), %rsi  /* RSI */
    mov 32(%rbx), %rdx  /* RDX */
    mov 40(%rbx), %r10  /* R10 */
    mov 48(%rbx), %r8   /* R8 */
    mov 56(%rbx), %r9   /* R9 */

run_passthrough_syscall:
    syscall

    /* 
     * We purposefully DO NOT check for -EINTR or -ERESTARTSYS here.
     * We push the raw RAX unconditionally back to the matrix.
     * The Guest's glibc handles the signals and restart semantics organically!
     */

    /* Syscall completed native validation! Store result (new rax) to exit_info. */
    lea matrix_exit_info(%rip), %rbx
    mov %rax, 8(%rbx)

    /* Re-dive into the matrix block to supply the new RAX. */
    jmp matrix_trampoline_loop


matrix_done:
fail:
    /* Terminal sys_exit for the Host */
    mov $60, %rax       /* sys_exit */
    mov $0, %rdi        /* errcode 0 */
    syscall


/* ═══════════════════════════════════════════════════════════════════════════ */
/* 
 * TARGET GUEST LOGIC 
 * (Runs internally under deep trace. All Syscalls are bounced)
 */
matrix_target_entry:
    /* 
     * Target has successfully booted internally! 
     * Issue a sys_write().
     * This 'syscall' executes INSIDE the context. EFER.SCE=0 generates #UD.
     * Host detects #UD, proxies sys_write, yields back. Target thinks it ran natively.
     */
    mov $1, %rax
    mov $1, %rdi
    lea msg_matrix_start(%rip), %rsi
    mov $73, %rdx
    syscall

    /* Attempt a simple nanosleep(1s) - previously emulated explicitly, now proxied natively! */
    /* ... skipped for simplicity ... */

    /* Finalize Matrix operation */
    mov $1, %rax
    mov $1, %rdi
    lea msg_matrix_done(%rip), %rsi
    mov $52, %rdx
    syscall

    /* Target natively exits */
    mov $231, %rax      /* sys_exit_group */
    mov $0, %rdi
    syscall

    /* Failsafe infinite spin if hypervisor declines exit */
1:  jmp 1b
