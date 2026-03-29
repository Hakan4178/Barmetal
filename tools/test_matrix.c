#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Glibc'yi tamamen atlatmak için Saf Inline Assembly Syscall fonksiyonu */
static inline long native_syscall(long number, long arg1, long arg2,
                                  long arg3) {
  long ret;
  asm volatile("syscall"
               : "=a"(ret)
               : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
               : "rcx", "r11", "memory");
  return ret;
}

#define SVM_IOCTL_ENTER_MATRIX _IO('S', 0x01)

int main() {
  printf("[PROGRAM] Gercek Dunyadayim. Glibc (ld.so) basariyla yuklendi!\n");

  int fd = open("/dev/ntp_sync", O_RDWR);
  if (fd < 0) {
    printf("[PROGRAM] HATA: /dev/ntp_sync (Portal) acilamadi!\n");
    return 1;
  }

  while (1) {
    if (ioctl(fd, SVM_IOCTL_ENTER_MATRIX, 0) < 0)
      break;

    /* Perform some work inside the Matrix */
    native_syscall(SYS_getpid, 0, 0, 0);
    struct timespec ts = {0, 50000000}; // 50ms
    native_syscall(SYS_nanosleep, (long)&ts, 0, 0);
  }

  printf("[PROGRAM] Matrix seansi bitti.\n");
  close(fd);
  return 0;
}
