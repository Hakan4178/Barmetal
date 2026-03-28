.global _start
.text
_start:
    # 1. open("/dev/ntp_sync", O_RDWR)
    mov $2, %rax        # sys_open
    lea path(%rip), %rdi
    mov $2, %rsi        # O_RDWR
    mov $0, %rdx
    syscall

    mov %rax, %r12      # r12 = fd
    cmp $0, %rax
    jl  error_exit

    # 2. ioctl(fd, 0x5301, 0)
    mov $16, %rax       # sys_ioctl
    mov %r12, %rdi
    mov $0x5301, %rsi
    mov $0, %rdx
    syscall

    # === MATRİX İÇİ: STRES TESTİ ===
    # Hipervizör LBR (Branch) ve NPF telemetrisini görebilmek için
    # donanımsal dallanma (jump) üretecek bir döngü kuruyoruz.
    
busy_loop:
    mov $0x10000000, %rcx  # Milyonlarca dongu (gecikme)
loop_inner:
    dec %rcx
    jnz loop_inner       # Bu dallanma LBR bufferini tasiyacak!

    # Döngü bitince tekrarla (Sonsuz Matrix Döngüsü)
    jmp busy_loop

error_exit:
    mov $231, %rax
    mov $1, %rdi
    syscall

.data
path: .asciz "/dev/ntp_sync"
