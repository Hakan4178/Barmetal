/* SPDX-License-Identifier: GPL-2.0-only
 *
 * svm_chardev.c — Character Device for Ghost Injection (Matrix Portal)
 *
 * Manges /dev/ntp_sync, allowing injected processes to voluntarily enter
 * the VMRUN sandbox without exposing PTRACE footprint.
 */

#include "ring_minus_one.h"
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/compat.h>
#include <linux/atomic.h>

/* SVM_ENTER_MATRIX command code */
#define SVM_IOCTL_ENTER_MATRIX _IO('S', 0x01)

/* Global lock to prevent multi-thread DoS and VMCB corruption */
static atomic_t matrix_active = ATOMIC_INIT(0);

static long svm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    /* 1. Mimari Uyumsuzluk Koruması: Sadece 64-bit Long Mode izinli */
    if (in_compat_syscall()) {
        pr_warn("[NTP_SYNC] 32-bit (compat) process %d denied sync.\n", current->pid);
        return -EINVAL;
    }

    switch (cmd) {
    case SVM_IOCTL_ENTER_MATRIX:
        /* 
         * 2. Eşzamanlılık (Race Condition) Koruması: Sadece 1 Süreç girebilir.
         * (İleride per-thread VMCB yaparsak bu kilidi açacağız) 
         */
        if (atomic_cmpxchg(&matrix_active, 0, 1) != 0) {
            pr_warn("[NTP_SYNC] Sync interface busy! PID: %d denied.\n", current->pid);
            return -EBUSY;
        }

        pr_info("[NTP_SYNC] Process (PID: %d, Comm: %s) triggered sync!\n",
                current->pid, current->comm);
        
        /* 
         * Phase 3: The actual execution of setup_vmcb_from_user_regs() 
         * and the svm_run_guest() loop will be placed here.
         */

        /* Test amaçlı hemen çıkıyoruz (Kilidi geri ver) */
        atomic_set(&matrix_active, 0);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations svm_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = svm_ioctl,
    /* .compat_ioctl bilerek EKLENMEDİ (32-bit zafiyet tıkacı) */
};

static struct miscdevice svm_misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "ntp_sync",
    .fops  = &svm_fops,
    /* 0666 is intentional securely: 
     * Malware running as non-root user needs to be able to open this device 
     * when we hijack its RIP to execute our shellcode. */
    .mode  = 0666, 
};

int svm_chardev_init(void)
{
    int ret = misc_register(&svm_misc_dev);
    if (ret)
        pr_err("[NTP_SYNC] Failed to initialize ntp character device (err %d)\n", ret);
    else
        pr_info("[NTP_SYNC] Successfully mapped transparent portal at /dev/ntp_sync\n");
    
    return ret;
}

void svm_chardev_exit(void)
{
    misc_deregister(&svm_misc_dev);
    pr_info("[NTP_SYNC] Portal /dev/ntp_sync closed.\n");
}
