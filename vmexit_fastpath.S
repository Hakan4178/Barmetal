/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vmexit_fastpath.S — Phase 27: Assembly Macro Dispatcher
 * Allows O(1) VMEXIT bypasses without pushing GPRs to memory or entering C.
 */

/*
 * MACRO_FAST_VMRUN
 * Common macro to quickly clean VMCB, restore temporary scratch registers
 * exactly as they were saved in `svm_asm.S`, and jump back to VMRUN.
 * Expects: %r14 = vmcb_va
 */
.macro MACRO_FAST_VMRUN
    /* Set VMCB Clean Bits (VMCB_CLEAN_ALL) to avoid HW re-parse */
    movl $0xFFFFFFFF, 0xC0(%r14)

    /* Read regs_ptr from host stack into RAX */
    mov 0(%rsp), %rax

    /* Restore guest scratch registers from guest_regs using RAX */
    mov 0x60(%rax), %r14
    mov 0x30(%rax), %r8
    mov 0x08(%rax), %rcx

    /* Restore vmcb_pa into RAX (needed by the vmrun instruction) */
    mov 16(%rsp), %rax

    /* Blast off -> next VM entry */
    jmp .Ldo_vmrun
.endm

/*
 * MACRO_HANDLE_DRX
 * Phase 25: DRx Stealth Shadowing for AC blindness.
 * Hardware keeps actual BP; we return 0 on DR0-DR7 reads.
 * O(1) inline jump-table execution using reg_offset_table from svm_decode.S
 * Expects: %r14 = vmcb_va
 */
.macro MACRO_HANDLE_DRX
    /* exit_info_1 contains the intercepted DRx/CRx information */
    mov 0x78(%r14), %rcx       /* exit_info_1 */
    and $0xF, %rcx             /* mask to get target general-purpose register index (0-15) */

    /* Load global reg_offset_table (16-bit entries: offset | flags) */
    leaq reg_offset_table(%rip), %r8
    movzwl (%r8, %rcx, 2), %ecx
    
    /* Determine if the target register lives in VMCB or guest_regs */
    test $0x8000, %ecx         /* Check VMCB_FLAG */
    jnz .L_is_vmcb_\@

    /* It's a standard GPR located in guest_regs */
    and $0x7FFF, %ecx          /* strip flag to get offset */
    mov 0(%rsp), %r8           /* r8 = regs_ptr */
    movq $0, (%r8, %rcx)       /* GuestReg = 0 */
    jmp .L_drx_done_\@

.L_is_vmcb_\@:
    /* It's RAX or RSP located in VMCB save area */
    and $0x7FFF, %ecx          /* strip flag to get offset */
    movq $0, (%r14, %rcx)      /* vmcb->save.Reg = 0 */

.L_drx_done_\@:
    /* Emulate the instruction by advancing RIP.
       For DRx intercepts, HW auto-populates `next_rip` (0xD8). */
    mov 0xD8(%r14), %rcx       /* next_rip */
    mov %rcx, 0x178(%r14)      /* save.rip */

    MACRO_FAST_VMRUN
    .align 64                  /* BTB Warming */
.endm
