/*
 * AMD64 SVM – Ring -1 Engine (V3.0 Refactored)
 *
 * Reusable VMRUN engine with:
 *   - NPT (Nested Page Table) support
 *   - TSC Offset compensation for stealth
 *   - Exported symbols for use by svm_dump module
 *
 * Original fixes preserved:
 *   Fix 1: VMRUN intercept (AMD mandatory)
 *   Fix 2-3: MSRPM/IOPM allocation
 *   Fix 4: Host GPR preservation
 *   Fix 5: GIF management (clgi/stgi)
 *   Fix 6: CR3 PCID masking
 *   Fix 7: set_memory_x via kprobes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kprobes.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>
#include <asm/svm.h>
#include <asm/io.h>
#include <linux/mm.h>

#include "npt_walk.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hakan");
MODULE_DESCRIPTION("AMD-V SVM Ring -1 Engine V3.0 – NPT + TSC Stealth");

/* ========== Global Değişkenler ========== */
static struct vmcb *guest_vmcb;
static phys_addr_t  guest_vmcb_pa;

static void        *hsave_va;
static phys_addr_t  hsave_pa;

static u8 *guest_code_page;
static u8 *guest_stack_page;

static void        *msrpm_va;   /* MSR Permission Map - 8KB (2 sayfa) */
static phys_addr_t  msrpm_pa;

static void        *iopm_va;    /* IO Permission Map - 12KB (3 sayfa, 4 tahsis) */
static phys_addr_t  iopm_pa;

/* set_memory_x / set_memory_nx fonksiyon işaretçileri */
typedef int (*set_memory_x_t)(unsigned long addr, int numpages);
typedef int (*set_memory_nx_t)(unsigned long addr, int numpages);
static set_memory_x_t  my_set_memory_x;
static set_memory_nx_t my_set_memory_nx;

/* Guest kodu: HLT + sonsuz döngü */
static const u8 guest_code_bin[] = {
    0xf4,        /* hlt */
    0xeb, 0xfc   /* jmp -2 */
};

/* ========== Yardımcı Fonksiyonlar ========== */

static bool svm_supported(void)
{
    u32 eax, ebx, ecx, edx;

    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return !!(ecx & (1 << 2));
}

static int resolve_hidden_symbols(void)
{
    struct kprobe kp_x  = { .symbol_name = "set_memory_x" };
    struct kprobe kp_nx = { .symbol_name = "set_memory_nx" };

    if (register_kprobe(&kp_x) < 0) {
        pr_err("set_memory_x adresi bulunamadı!\n");
        return -EFAULT;
    }
    my_set_memory_x = (set_memory_x_t)kp_x.addr;
    unregister_kprobe(&kp_x);

    if (register_kprobe(&kp_nx) < 0) {
        pr_err("set_memory_nx adresi bulunamadı!\n");
        return -EFAULT;
    }
    my_set_memory_nx = (set_memory_nx_t)kp_nx.addr;
    unregister_kprobe(&kp_nx);

    pr_info("Semboller çözümlendi: set_memory_x=%pK, set_memory_nx=%pK\n",
            my_set_memory_x, my_set_memory_nx);
    return 0;
}

void vmrun_safe(u64 vmcb_pa)
{
    asm volatile (
        "push %%rbx  \n\t"
        "push %%rcx  \n\t"
        "push %%rdx  \n\t"
        "push %%rsi  \n\t"
        "push %%rdi  \n\t"
        "push %%rbp  \n\t"
        "push %%r8   \n\t"
        "push %%r9   \n\t"
        "push %%r10  \n\t"
        "push %%r11  \n\t"
        "push %%r12  \n\t"
        "push %%r13  \n\t"
        "push %%r14  \n\t"
        "push %%r15  \n\t"
        "clgi        \n\t"
        "vmrun %%rax \n\t"
        "stgi        \n\t"
        "pop %%r15   \n\t"
        "pop %%r14   \n\t"
        "pop %%r13   \n\t"
        "pop %%r12   \n\t"
        "pop %%r11   \n\t"
        "pop %%r10   \n\t"
        "pop %%r9    \n\t"
        "pop %%r8    \n\t"
        "pop %%rbp   \n\t"
        "pop %%rdi   \n\t"
        "pop %%rsi   \n\t"
        "pop %%rdx   \n\t"
        "pop %%rcx   \n\t"
        "pop %%rbx   \n\t"
        :
        : "a"(vmcb_pa)
        : "memory", "cc"
    );
}
EXPORT_SYMBOL_GPL(vmrun_safe);

/* ========== TSC Offset Compensation ========== */

/* Forward declarations for exported symbols */
u64  vmrun_tsc_compensated(struct vmcb *vmcb, u64 vmcb_pa);
void tsc_offset_reset(void);
int  vmcb_prepare_npt(struct vmcb *vmcb, u64 g_rip, u64 g_rsp,
                      u64 g_cr3, struct npt_context *npt);

static s64 cumulative_tsc_offset;

/*
 * vmrun_tsc_compensated - Execute VMRUN and compensate TSC
 * Subtracts the hypervisor execution time from the guest's TSC view.
 */
u64 vmrun_tsc_compensated(struct vmcb *vmcb, u64 vmcb_pa)
{
    u64 tsc_before, tsc_after, exit_code;

    vmcb->control.tsc_offset = cumulative_tsc_offset;

    tsc_before = rdtsc();
    vmrun_safe(vmcb_pa);
    tsc_after = rdtsc();

    /* Subtract hypervisor time from guest's future TSC reads */
    cumulative_tsc_offset -= (s64)(tsc_after - tsc_before);

    exit_code = ((u64)vmcb->control.exit_code_hi << 32) |
                 vmcb->control.exit_code;
    return exit_code;
}
EXPORT_SYMBOL_GPL(vmrun_tsc_compensated);

/*
 * tsc_offset_reset - Reset the cumulative TSC offset
 */
void tsc_offset_reset(void)
{
    cumulative_tsc_offset = 0;
}
EXPORT_SYMBOL_GPL(tsc_offset_reset);

static void raw_cr3_flush(void)
{
    unsigned long cr3;

    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ========== Modül Init ========== */

static int __init svm_module_init(void)
{
    u64 efer_val, cr0, cr4, rflags;
    unsigned long cr3;
    u16 cs, ds, ss, es, fs, gs, tr;
    struct desc_ptr gdtr, idtr;
    u64 fs_base, gs_base;
    u64 guest_rsp;
    u64 exit_code;
    int ret;

    pr_info("=== SVM Modülü Başlatılıyor ===\n");

    /* 1) SVM desteği kontrol */
    if (!svm_supported()) {
        pr_err("SVM bu CPU'da desteklenmiyor.\n");
        return -ENODEV;
    }
    pr_info("SVM desteği mevcut.\n");

    /* 2) Gizli semboller */
    ret = resolve_hidden_symbols();
    if (ret < 0)
        return ret;

    /* 3) EFER.SVME etkinleştir */
    rdmsrl(MSR_EFER, efer_val);
    if (!(efer_val & EFER_SVME)) {
        efer_val |= EFER_SVME;
        wrmsrl(MSR_EFER, efer_val);
        pr_info("SVME biti etkinleştirildi.\n");
    }

    /* 4) HSAVE alanı */
    hsave_va = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!hsave_va) {
        pr_err("HSAVE tahsis hatası.\n");
        return -ENOMEM;
    }
    hsave_pa = virt_to_phys(hsave_va);
    wrmsrl(MSR_VM_HSAVE_PA, hsave_pa);
    pr_info("HSAVE PA: 0x%llx\n", (u64)hsave_pa);

    /* 5) VMCB */
    guest_vmcb = (struct vmcb *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!guest_vmcb) {
        ret = -ENOMEM;
        goto err_hsave;
    }
    guest_vmcb_pa = virt_to_phys(guest_vmcb);
    pr_info("VMCB PA: 0x%llx\n", (u64)guest_vmcb_pa);

    /* 6) MSRPM - 8KB (2 sayfa), sıfır = tüm MSR erişimine izin ver */
    msrpm_va = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
    if (!msrpm_va) {
        pr_err("MSRPM tahsis hatası.\n");
        ret = -ENOMEM;
        goto err_vmcb;
    }
    msrpm_pa = virt_to_phys(msrpm_va);
    pr_info("MSRPM PA: 0x%llx\n", (u64)msrpm_pa);

    /* 7) IOPM - 12KB (3 sayfa gerekli, 4 tahsis), sıfır = tüm IO'ya izin ver */
    iopm_va = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 2);
    if (!iopm_va) {
        pr_err("IOPM tahsis hatası.\n");
        ret = -ENOMEM;
        goto err_msrpm;
    }
    iopm_pa = virt_to_phys(iopm_va);
    pr_info("IOPM PA: 0x%llx\n", (u64)iopm_pa);

    /* 8) Guest kod ve stack sayfaları */
    guest_code_page  = (u8 *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    guest_stack_page = (u8 *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!guest_code_page || !guest_stack_page) {
        pr_err("Guest bellek tahsis hatası.\n");
        ret = -ENOMEM;
        goto err_guest;
    }

    /* Kod sayfasını çalıştırılabilir yap (NX kaldır) */
    ret = my_set_memory_x((unsigned long)guest_code_page, 1);
    if (ret) {
        pr_err("set_memory_x başarısız: %d\n", ret);
        goto err_guest;
    }

    memcpy(guest_code_page, guest_code_bin, sizeof(guest_code_bin));

    /*
     * T1/T2 FIX: Guest RIP and RSP MUST be physical addresses when
     * NPT is not enabled, because VMRUN loads them directly.
     * With host CR3 in guest, virtual == identity only if the kernel
     * direct map covers these pages (it does for __get_free_page).
     * For safety we use the VA since we share host CR3 + paging.
     * If NPT is enabled, these become GVAs translated through guest CR3.
     */
    guest_rsp = (u64)guest_stack_page + PAGE_SIZE - 16; /* T3: 16-byte aligned */

    /* 9) Host durumunu oku */
    cr0 = native_read_cr0();
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr4 = native_read_cr4();
    asm volatile("pushf; pop %0" : "=r"(rflags));

    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%ss, %0" : "=r"(ss));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    asm volatile("str  %0"      : "=r"(tr));
    asm volatile("sgdt %0"      : "=m"(gdtr));
    asm volatile("sidt %0"      : "=m"(idtr));
    rdmsrl(MSR_FS_BASE, fs_base);
    rdmsrl(MSR_GS_BASE, gs_base);

    pr_info("Host CR0=0x%llx CR3=0x%lx CR4=0x%llx EFER=0x%llx\n",
            cr0, cr3, cr4, efer_val);

    /* ============================================ */
    /*           VMCB CONTROL ALANI                 */
    /* ============================================ */

    /*
     * ÖNEMLİ: VMRUN intercept ZORUNLU!
     * AMD APM: "VMRUN intercept must be set, otherwise VMEXIT_INVALID"
     */
    guest_vmcb->control.intercepts[INTERCEPT_VMRUN >> 5] |=
        (1U << (INTERCEPT_VMRUN & 31));

    /* HLT intercept - konuk HLT çalıştırınca VMEXIT olsun */
    guest_vmcb->control.intercepts[INTERCEPT_HLT >> 5] |=
        (1U << (INTERCEPT_HLT & 31));

    /* MSRPM ve IOPM fiziksel adreslerini ayarla */
    guest_vmcb->control.msrpm_base_pa = msrpm_pa;
    guest_vmcb->control.iopm_base_pa  = iopm_pa;

    /* ASID - sıfır olamaz! */
    guest_vmcb->control.asid = 1;

    /* Clean bits = 0 (ilk VMRUN, her şey yeniden yüklensin) */
    guest_vmcb->control.clean = 0;

    /* ============================================ */
    /*           VMCB SAVE ALANI (Guest State)      */
    /* ============================================ */

    /* CS - 64-bit kod segmenti */
    guest_vmcb->save.cs.selector = cs;
    guest_vmcb->save.cs.attrib   = 0x029B;  /* P=1 DPL=0 S=1 Type=B L=1 D=0 */
    guest_vmcb->save.cs.limit    = 0xFFFFFFFF;
    guest_vmcb->save.cs.base     = 0;

    /* DS */
    guest_vmcb->save.ds.selector = ds;
    guest_vmcb->save.ds.attrib   = 0x0093;  /* P=1 DPL=0 S=1 Type=3 */
    guest_vmcb->save.ds.limit    = 0xFFFFFFFF;
    guest_vmcb->save.ds.base     = 0;

    /* ES */
    guest_vmcb->save.es.selector = es;
    guest_vmcb->save.es.attrib   = 0x0093;
    guest_vmcb->save.es.limit    = 0xFFFFFFFF;
    guest_vmcb->save.es.base     = 0;

    /* SS */
    guest_vmcb->save.ss.selector = ss;
    guest_vmcb->save.ss.attrib   = 0x0093;
    guest_vmcb->save.ss.limit    = 0xFFFFFFFF;
    guest_vmcb->save.ss.base     = 0;

    /* FS */
    guest_vmcb->save.fs.selector = fs;
    guest_vmcb->save.fs.attrib   = 0x0093;
    guest_vmcb->save.fs.limit    = 0xFFFFFFFF;
    guest_vmcb->save.fs.base     = fs_base;

    /* GS */
    guest_vmcb->save.gs.selector = gs;
    guest_vmcb->save.gs.attrib   = 0x0093;
    guest_vmcb->save.gs.limit    = 0xFFFFFFFF;
    guest_vmcb->save.gs.base     = gs_base;

    /* GDTR */
    guest_vmcb->save.gdtr.limit = gdtr.size;
    guest_vmcb->save.gdtr.base  = gdtr.address;

    /* IDTR */
    guest_vmcb->save.idtr.limit = idtr.size;
    guest_vmcb->save.idtr.base  = idtr.address;

    /* TR - 64-bit TSS Busy */
    guest_vmcb->save.tr.selector = tr;
    guest_vmcb->save.tr.limit    = 0xFFFF;
    guest_vmcb->save.tr.base     = 0;
    guest_vmcb->save.tr.attrib   = 0x008B;

    /* Kontrol register'ları */
    guest_vmcb->save.cr0  = cr0;
    guest_vmcb->save.cr3  = cr3 & 0xFFFFFFFFFFFFF000ULL;  /* PCID/KPTI maskele */
    guest_vmcb->save.cr4  = cr4;
    guest_vmcb->save.efer = efer_val;  /* SVME dahil */

    /* Debug register'ları */
    guest_vmcb->save.dr6 = 0xFFFF0FF0;
    guest_vmcb->save.dr7 = 0x00000400;

    /* Çalışma durumu */
    guest_vmcb->save.rip    = (u64)guest_code_page;
    guest_vmcb->save.rsp    = guest_rsp;
    guest_vmcb->save.rax    = 0;
    guest_vmcb->save.rflags = rflags & ~X86_EFLAGS_IF;

    pr_info("Guest RIP: 0x%llx  RSP: 0x%llx\n",
            guest_vmcb->save.rip, guest_vmcb->save.rsp);
    pr_info("Guest EFER: 0x%llx  CR0: 0x%llx  CR3: 0x%llx  CR4: 0x%llx\n",
            guest_vmcb->save.efer, guest_vmcb->save.cr0,
            guest_vmcb->save.cr3, guest_vmcb->save.cr4);
    pr_info("Intercepts[3]: 0x%08x  [4]: 0x%08x\n",
            guest_vmcb->control.intercepts[3],
            guest_vmcb->control.intercepts[4]);
    pr_info("=== Konuk başlatılıyor... ===\n");

    /* ===== VMRUN ===== */
    vmrun_safe(guest_vmcb_pa);

    /* ===== VMEXIT sonrası ===== */
    exit_code = ((u64)guest_vmcb->control.exit_code_hi << 32) |
                 guest_vmcb->control.exit_code;

    pr_info("=== VMEXIT ===\n");
    pr_info("  Exit Code : 0x%llx\n", exit_code);
    pr_info("  Exit Info1: 0x%llx\n", guest_vmcb->control.exit_info_1);
    pr_info("  Exit Info2: 0x%llx\n", guest_vmcb->control.exit_info_2);
    pr_info("  Guest RIP : 0x%llx\n", guest_vmcb->save.rip);

    if (exit_code == SVM_EXIT_HLT)
        pr_info(">>> BAŞARILI! Guest HLT yakalandı (0x78) <<<\n");
    else if (exit_code == 0xFFFFFFFFFFFFFFFFULL)
        pr_err(">>> HATA: VMEXIT_INVALID - VMCB hala geçersiz <<<\n");
    else
        pr_info(">>> Beklenmeyen çıkış kodu: 0x%llx <<<\n", exit_code);

    raw_cr3_flush();

    /*
     * T5 FIX: If VMEXIT_INVALID, the VMCB was rejected.
     * Return error to prevent module from loading in broken state.
     */
    if (exit_code == 0xFFFFFFFFFFFFFFFFULL) {
        ret = -EIO;
        goto err_guest;
    }

    return 0;

/* ===== Hata Temizliği ===== */
err_guest:
    if (guest_code_page)
        free_page((unsigned long)guest_code_page);
    if (guest_stack_page)
        free_page((unsigned long)guest_stack_page);
    free_pages((unsigned long)iopm_va, 2);
err_msrpm:
    free_pages((unsigned long)msrpm_va, 1);
err_vmcb:
    free_page((unsigned long)guest_vmcb);
err_hsave:
    free_page((unsigned long)hsave_va);
    return ret;
}

/* ========== VMCB Preparation (Reusable) ========== */

/*
 * vmcb_prepare_npt - Configure a VMCB for NPT-based execution
 * @vmcb: VMCB to configure (caller allocates)
 * @g_rip: initial guest instruction pointer
 * @g_rsp: initial guest stack pointer
 * @g_cr3: guest CR3 (0 = use current host CR3)
 * @npt: NPT context (NULL to disable NPT)
 *
 * Sets up control and save areas for a minimal guest
 * with optional NPT and TSC offset support.
 */
int vmcb_prepare_npt(struct vmcb *vmcb, u64 g_rip, u64 g_rsp,
                     u64 g_cr3, struct npt_context *npt)
{
    u64 efer_val, cr0, cr4, rflags;
    unsigned long cr3;
    u16 cs, ds, ss, es, fs, gs, tr;
    struct desc_ptr gdtr, idtr;
    u64 fs_base, gs_base;

    memset(vmcb, 0, sizeof(*vmcb));

    /* Read host state */
    rdmsrl(MSR_EFER, efer_val);
    cr0 = native_read_cr0();
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr4 = native_read_cr4();
    asm volatile("pushf; pop %0" : "=r"(rflags));

    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%ss, %0" : "=r"(ss));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    asm volatile("str  %0"      : "=r"(tr));
    asm volatile("sgdt %0"      : "=m"(gdtr));
    asm volatile("sidt %0"      : "=m"(idtr));
    rdmsrl(MSR_FS_BASE, fs_base);
    rdmsrl(MSR_GS_BASE, gs_base);

    /* VMRUN intercept (mandatory), HLT and RDTSCP intercepts */
    vmcb->control.intercepts[INTERCEPT_VMRUN >> 5] |=
        (1U << (INTERCEPT_VMRUN & 31));
    vmcb->control.intercepts[INTERCEPT_HLT >> 5] |=
        (1U << (INTERCEPT_HLT & 31));

    /*
     * RDTSCP intercept: AMD APM Vol.2 Table 15-7
     */
#ifdef INTERCEPT_RDTSCP
    vmcb->control.intercepts[INTERCEPT_RDTSCP >> 5] |=
        (1U << (INTERCEPT_RDTSCP & 31));
#endif

    /*
     * MSRPM: Intercept critical MSRs.
     *
     * Layout: MSR 0x0000_0000-0x0000_1FFF at MSRPM offset 0.
     *         MSR 0xC000_0000-0xC000_1FFF at MSRPM offset 0x800.
     * Each MSR uses 2 bits: bit 0 = rdmsr, bit 1 = wrmsr.
     */
    if (msrpm_va) {
        u8 *msrpm = (u8 *)msrpm_va;

        /* MSR 0x10 (IA32_TSC): intercept rdmsr */
        /* byte = (0x10 * 2) / 8 = 4, bit = (0x10 * 2) % 8 = 0 */
        msrpm[4] |= (1 << 0);

        /*
         * MSR 0xC0000082 (LSTAR): intercept both rdmsr and wrmsr.
         * Offset from C000_0000 region base (0x800):
         *   relative MSR = 0x82
         *   byte = 0x800 + (0x82 * 2) / 8 = 0x800 + 0x20 = 0x820
         *   bit  = (0x82 * 2) % 8 = 4
         *   rdmsr = bit 4, wrmsr = bit 5
         */
        msrpm[0x820] |= (1 << 4) | (1 << 5);
    }

    /*
     * IOPM: Intercept timer I/O ports to prevent timer skew detection.
     *
     * ACPI PM Timer: port 0x808 (or system-specific pmtmr_ioport).
     * IOPM is a bitmap: 1 bit per port, 1 = intercept.
     * byte = port / 8, bit = port % 8
     */
    if (iopm_va) {
        u8 *iopm = (u8 *)iopm_va;

        /* ACPI PM Timer - port 0x808 */
        iopm[0x808 / 8] |= (1 << (0x808 % 8));
        /* Also block 0x809-0x80B (32-bit read spans 4 ports) */
        iopm[0x809 / 8] |= (1 << (0x809 % 8));
        iopm[0x80A / 8] |= (1 << (0x80A % 8));
        iopm[0x80B / 8] |= (1 << (0x80B % 8));
    }

    /* MSRPM and IOPM */
    vmcb->control.msrpm_base_pa = msrpm_pa;
    vmcb->control.iopm_base_pa  = iopm_pa;

    /*
     * ASID and TLB isolation:
     *   - Use ASID=2 for guest (ASID=1 is host convention).
     *   - TLB_CONTROL=1: Flush entire TLB on VMRUN.
     *   This prevents TLB side-channel timing attacks where the guest
     *   measures TLB hit/miss latency to detect VMEXIT transitions.
     *   Performance cost is accepted for stealth.
     */
    vmcb->control.asid = 2;
    vmcb->control.tlb_ctl = 1;  /* TLB_CONTROL_FLUSH_ALL */
    vmcb->control.clean = 0;

    /* NPT configuration */
    if (npt && npt->pml4_pa) {
        vmcb->control.nested_ctl = 1;
        vmcb->control.nested_cr3 = npt->pml4_pa;
        pr_info("[VMCB] NPT enabled, nested_cr3=0x%llx\n",
                (u64)npt->pml4_pa);
    }

    /* TSC offset */
    vmcb->control.tsc_offset = cumulative_tsc_offset;

    /* Segment registers - 64-bit long mode */
    vmcb->save.cs.selector = cs;
    vmcb->save.cs.attrib   = 0x029B;
    vmcb->save.cs.limit    = 0xFFFFFFFF;
    vmcb->save.cs.base     = 0;

    vmcb->save.ds.selector = ds;
    vmcb->save.ds.attrib   = 0x0093;
    vmcb->save.ds.limit    = 0xFFFFFFFF;
    vmcb->save.ds.base     = 0;

    vmcb->save.es.selector = es;
    vmcb->save.es.attrib   = 0x0093;
    vmcb->save.es.limit    = 0xFFFFFFFF;
    vmcb->save.es.base     = 0;

    vmcb->save.ss.selector = ss;
    vmcb->save.ss.attrib   = 0x0093;
    vmcb->save.ss.limit    = 0xFFFFFFFF;
    vmcb->save.ss.base     = 0;

    vmcb->save.fs.selector = fs;
    vmcb->save.fs.attrib   = 0x0093;
    vmcb->save.fs.limit    = 0xFFFFFFFF;
    vmcb->save.fs.base     = fs_base;

    vmcb->save.gs.selector = gs;
    vmcb->save.gs.attrib   = 0x0093;
    vmcb->save.gs.limit    = 0xFFFFFFFF;
    vmcb->save.gs.base     = gs_base;

    vmcb->save.gdtr.limit = gdtr.size;
    vmcb->save.gdtr.base  = gdtr.address;
    vmcb->save.idtr.limit = idtr.size;
    vmcb->save.idtr.base  = idtr.address;

    vmcb->save.tr.selector = tr;
    vmcb->save.tr.limit    = 0xFFFF;
    vmcb->save.tr.base     = 0;
    vmcb->save.tr.attrib   = 0x008B;

    /* Control registers */
    vmcb->save.cr0  = cr0;
    vmcb->save.cr3  = g_cr3 ? g_cr3 : (cr3 & 0xFFFFFFFFFFFFF000ULL);
    vmcb->save.cr4  = cr4;
    vmcb->save.efer = efer_val;

    /* Debug registers */
    vmcb->save.dr6 = 0xFFFF0FF0;
    vmcb->save.dr7 = 0x00000400;

    /* Execution state */
    vmcb->save.rip    = g_rip;
    vmcb->save.rsp    = g_rsp;
    vmcb->save.rax    = 0;
    vmcb->save.rflags = rflags & ~X86_EFLAGS_IF;

    return 0;
}
EXPORT_SYMBOL_GPL(vmcb_prepare_npt);

/* ========== Modül Exit ========== */

static void __exit svm_module_exit(void)
{
    u64 efer_val;

    rdmsrl(MSR_EFER, efer_val);
    efer_val &= ~EFER_SVME;
    wrmsrl(MSR_EFER, efer_val);

    if (guest_code_page) {
        if (my_set_memory_nx)
            my_set_memory_nx((unsigned long)guest_code_page, 1);
        free_page((unsigned long)guest_code_page);
    }
    if (guest_stack_page)
        free_page((unsigned long)guest_stack_page);
    if (iopm_va)
        free_pages((unsigned long)iopm_va, 2);
    if (msrpm_va)
        free_pages((unsigned long)msrpm_va, 1);
    if (guest_vmcb)
        free_page((unsigned long)guest_vmcb);
    if (hsave_va)
        free_page((unsigned long)hsave_va);

    pr_info("SVM modülü temizlendi.\n");
}

module_init(svm_module_init);
module_exit(svm_module_exit);
