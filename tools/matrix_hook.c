#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#define SVM_IOCTL_ENTER_MATRIX _IO('S', 0x01)

/* 
 * 1. NATIVE SYSCALL BYPASS
 * Glibc'nin syscall wrapper'larını (open, ioctl, close) kullanmıyoruz. 
 * VMP veya anti-cheat'ler libc hook atarak izleme yaparsa, bizim çağrılarımızı 
 * asla göremezler. Doğrudan donanım kesmesi atıyoruz.
 */
static inline long native_syscall(long number, long arg1, long arg2, long arg3) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/*
 * 2. EARLY EXECUTION (CONSTRUCTOR)
 * Hedef yazılımın main() veya OEP (_start) işlemleri başlamadan hemen önce çalışır.
 */
static void __attribute__((constructor)) init_matrix(void) {
    /*
     * 3. STRING OBFUSCATION
     * "/dev/ntp_sync" string'inin .rodata section'da kabak gibi görünmesini engelliyoruz.
     * VMP static analizi veya signature taramaları bu string'i bulamaz.
     * Stack üzerinde karakter karakter inşa edilir.
     */
    volatile char dev_path[] = {'/', 'd', 'e', 'v', '/', 'n', 't', 'p', '_', 's', 'y', 'n', 'c', '\0'};

    // sys_open
    long fd = native_syscall(2, (long)dev_path, 2 /* O_RDWR */, 0);
    
    if (fd >= 0) {
        // sys_ioctl -> MATRIX'E GİRİŞ YAPILDI
        native_syscall(16, fd, SVM_IOCTL_ENTER_MATRIX, 0);
        
        /* 
         * 4. STEALTH FILE DESCRIPTOR
         * Bu satır işlendiğinde ARTIK MATRIX'İN İÇİNDEYİZ.
         * Hemen açtığımız dosya tanımlayıcısını (fd) kapatıyoruz.
         * Anti-Cheat'in /proc/self/fd/ dizin taramaları Matrix cihazımızı ASLA bulamaz.
         * Ancak host thread'imiz (sys_ioctl kernel sürecimiz) hala aktif kaldığı için
         * Matrix yıkılmaz.
         */
        native_syscall(3, fd, 0, 0);
        
        /* 
         * 5. ENVIRONMENT PURGE (İsteğe Bağlı)
         * İzi tamamen kaybettirmek adına çevre değişkenleri vs. manipüle edilebilir.
         */
    }
}
