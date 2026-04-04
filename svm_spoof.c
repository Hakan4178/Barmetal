#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include "ring_minus_one.h"

static struct kretprobe getpid_kprobe;
static bool kprobe_active = false;
extern pid_t matrix_owner_pid;
extern atomic_t matrix_active;

// Kretprobe Entry Handler (İsteğe bağlı, dönüşte yakalayacağımız için boş bırakıyoruz)
static int getpid_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    return 0;
}

// Kretprobe Return Handler (Gerçek Spoofing İşlemi Burada Gerçekleşir)
static int getpid_return_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    // Sadece hedef sürecin syscall'larını spoofla! Diğer OS süreçlerini bozma.
    if (matrix_owner_pid != 0 && current->pid == matrix_owner_pid && atomic_read(&matrix_active)) {
        
        /* 
         * [PHASE 16 ELSE - NATIVE KPROBE HOOK]
         * Target process `getpid()` çağırdığında, dönüş değerini kalıcı olarak
         * manipüle ediyoruz. Bu işlem Host Kernel seviyesinde gerçekleştiği için,
         * VMRUN modunda çalışan Guest Süreç hiçbir "VMExit" veya "Context Switch"
         * gecikmesi yaşamaz. 0x7B (Syscall Trap) gecikmesi %100 atlatılır.
         */
        
        // Örnek: getpid() her zaman 1337 döndürsün (Hayalet PID)
        regs->ax = 1337;
    }
    return 0;
}

int init_syscall_spoofing(void) {
    getpid_kprobe.kp.symbol_name = "__x64_sys_getpid";
    getpid_kprobe.handler = getpid_return_handler;
    getpid_kprobe.entry_handler = getpid_entry_handler;
    getpid_kprobe.maxactive = 20;

    int ret = register_kretprobe(&getpid_kprobe);
    if (ret < 0) {
        pr_err("[THERMAL] __x64_sys_getpid Kprobe hook failed: %d\n", ret);
        return ret;
    }
    
    kprobe_active = true;
    pr_info("[THERMAL] Phase 16 Fallback: __x64_sys_getpid hooked natively for Matrix tracking.\n");
    return 0;
}

void cleanup_syscall_spoofing(void) {
    if (kprobe_active) {
        unregister_kretprobe(&getpid_kprobe);
        kprobe_active = false;
        pr_info("[THERMAL] Phase 16 Fallback (Kprobe) cleanup done.\n");
    }
}
