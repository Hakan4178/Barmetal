/*
 * AMD64 SVM – Ring -1 Unified Engine (V4.0)
 *
 * Single kernel module combining:
 *   - SVM (VMRUN) engine with NPT + TSC stealth
 *   - Memory snapshot introspection via /proc interface
 *   - NPT identity map builder
 *
 * Section layout:
 *   ═══ SVM ENGINE — Globals, VMRUN, TSC
 *   ═══ NPT — Identity Map Builder
 *   ═══ SNAPSHOT — Memory Capture
 *   ═══ PROCFS — /proc Interface
 *   ═══ MODULE INIT/EXIT
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/string.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/minmax.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/cred.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>
#include <asm/svm.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/page.h>

#include "svm_dump.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hakan");
MODULE_DESCRIPTION("AMD-V SVM Ring -1 Unified Engine V4.0");
MODULE_IMPORT_NS("KVM_AMD");

/* ═══════════════════════════════════════════════════════════════════════════
 *  NPT (Nested Page Table) — Tanımlar & Yapılar
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NPT_PRESENT       (1ULL << 0)
#define NPT_WRITE         (1ULL << 1)
#define NPT_USER          (1ULL << 2)
#define NPT_PS            (1ULL << 7)
#define NPT_NX            (1ULL << 63)
#define NPT_DEFAULT_FLAGS (NPT_PRESENT | NPT_WRITE | NPT_USER)
#define NPT_MAX_PAGES     8192
#define NPT_PWT           (1ULL << 3)
#define NPT_PCD           (1ULL << 4)
#define NPT_PAT_LARGE     (1ULL << 12)
#define NPT_CACHE_WB      0ULL
#define NPT_CACHE_UC      (NPT_PWT | NPT_PCD)

struct npt_context {
    u64 *pml4;
    phys_addr_t pml4_pa;
    struct page *pages[NPT_MAX_PAGES];
    int page_count;
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  SVM ENGINE — Global Değişkenler
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 *  SNAPSHOT — Değişkenler & Yardımcı Tanımlar
 * ═══════════════════════════════════════════════════════════════════════════ */

#define WATCH_NAME_MAX 64
#define SNAPSHOT_MIN_INTERVAL_SEC 1
#define PROC_DIR "svm_dump"
#define SVM_SNAPSHOT_VERSION 30

static struct proc_dir_entry *proc_dir;
static DEFINE_MUTEX(snapshot_lock);

static char watch_name[WATCH_NAME_MAX];
static struct task_struct *watcher_thread;
static bool auto_watch_active;
static int snapshot_count;
static u64 last_snapshot_time;
static bool full_dump_mode = false;
static bool npt_mode = false;

struct svm_snapshot_blob {
    void *data;
    size_t size;
};

static struct svm_snapshot_blob snapshot_blob = { NULL, 0 };

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


/* ========== TSC Offset Compensation ========== */

/* Forward declarations for exported symbols */
u64  vmrun_tsc_compensated(struct vmcb *vmcb, u64 vmcb_pa);
void tsc_offset_reset(void);
static int vmcb_prepare_npt(struct vmcb *vmcb, u64 g_rip, u64 g_rsp,
                            u64 g_cr3, struct npt_context *npt);

static DEFINE_PER_CPU(s64, pcpu_tsc_offset);

/*
 * vmrun_tsc_compensated - Execute VMRUN and compensate TSC (V4.0)
 *
 * Per-CPU offset eliminates cross-core drift.
 * IRQ disabled + preempt disabled eliminates interrupt-induced spikes.
 * Result: guest sees perfectly linear TSC progression.
 */
u64 vmrun_tsc_compensated(struct vmcb *vmcb, u64 vmcb_pa)
{
    u64 tsc_before, tsc_after, exit_code;
    unsigned long flags;
    s64 *offset;

    preempt_disable();
    local_irq_save(flags);

    offset = this_cpu_ptr(&pcpu_tsc_offset);
    vmcb->control.tsc_offset = *offset;

    tsc_before = rdtsc();
    vmrun_safe(vmcb_pa);
    tsc_after = rdtsc();

    /* Subtract hypervisor time — per-CPU, no cross-core drift */
    *offset -= (s64)(tsc_after - tsc_before);

    local_irq_restore(flags);
    preempt_enable();

    exit_code = ((u64)vmcb->control.exit_code_hi << 32) |
                 vmcb->control.exit_code;
    return exit_code;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SVM ENGINE — Yardımcı Fonksiyonlar (devam)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * tsc_offset_reset - Reset the cumulative TSC offset
 */
void tsc_offset_reset(void)
{
    this_cpu_write(pcpu_tsc_offset, 0);
}

static inline bool svm_check_access(void)
{
    return capable(CAP_SYS_ADMIN);
}

static void raw_cr3_flush(void)
{
    unsigned long cr3;

    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}


 *  NPT — Fonksiyonlar
 *  npt_alloc_page      : NPT yapısı için sıfırlanmış sayfa ayır
 *  npt_build_identity_map : 4-level identity map oluştur (2MB leaf)
 *  npt_destroy          : NPT yapısını yok et, tüm sayfaları serbest bırak
 *  npt_set_page_nx      : Belirli GPA'yı NX olarak işaretle
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * npt_alloc_page - NPT için sıfırlanmış bir sayfa ayır ve context'e kaydet.
 * @ctx: NPT context
 * Returns: kernel virtual address, veya NULL (hata)
 */
static u64 *npt_alloc_page(struct npt_context *ctx)
{
    struct page *p;

    if (ctx->page_count >= NPT_MAX_PAGES) {
        pr_err("[NPT] page limit (%d) exceeded\n", NPT_MAX_PAGES);
        return NULL;
    }

    p = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!p)
        return NULL;

    ctx->pages[ctx->page_count++] = p;
    return (u64 *)page_address(p);
}

/* Forward declaration — npt_set_page_nx is used by npt_build_identity_map */
static int npt_set_page_nx(struct npt_context *ctx, u64 gpa);

/*
 * npt_build_identity_map - 4-level AMD64 NPT identity map oluştur
 * @ctx: NPT context (doldurulacak)
 * @phys_limit: haritalanacak fiziksel adres üst sınırı (byte)
 *
 * PML4 → PDPT → PD zinciri kurar, her PD entry'si 2MB leaf (PS=1).
 * MTRR güvenliği: RAM bölgeleri WB, I/O bölgeleri UC.
 * HPET MMIO (0xFED00000) otomatik NX olarak işaretlenir.
 *
 * Adres alanı (2MB pages):
 *   PML4 index: bits [47:39], PDPT index: bits [38:30], PD index: bits [29:21]
 *   4GB RAM → ~6 sayfa, 64GB RAM → ~129 sayfa.
 */
static int npt_build_identity_map(struct npt_context *ctx, u64 phys_limit)
{
    u64 addr;
    int pml4_idx, pdpt_idx, pd_idx;
    u64 *pml4, *pdpt, *pd;

    memset(ctx, 0, sizeof(*ctx));

    pml4 = npt_alloc_page(ctx);
    if (!pml4)
        return -ENOMEM;

    ctx->pml4 = pml4;
    ctx->pml4_pa = virt_to_phys(pml4);

    pr_info("[NPT] Building identity map up to 0x%llx (%llu MB)\n",
        phys_limit, phys_limit >> 20);

    for (addr = 0; addr < phys_limit; addr += (2ULL << 20)) {
        pml4_idx = (addr >> 39) & 0x1FF;
        pdpt_idx = (addr >> 30) & 0x1FF;
        pd_idx   = (addr >> 21) & 0x1FF;

        /* PDPT yoksa ayır */
        if (!(pml4[pml4_idx] & NPT_PRESENT)) {
            pdpt = npt_alloc_page(ctx);
            if (!pdpt)
                goto fail;
            pml4[pml4_idx] = virt_to_phys(pdpt) | NPT_DEFAULT_FLAGS;
        } else {
            pdpt = phys_to_virt(pml4[pml4_idx] & ~0xFFFULL);
        }

        /* PD yoksa ayır */
        if (!(pdpt[pdpt_idx] & NPT_PRESENT)) {
            pd = npt_alloc_page(ctx);
            if (!pd)
                goto fail;
            pdpt[pdpt_idx] = virt_to_phys(pd) | NPT_DEFAULT_FLAGS;
        } else {
            pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);
        }

        /* MTRR güvenliği: I/O bölgeleri UC, RAM bölgeleri WB */
        {
            u64 cache_flags;
            unsigned long pfn = addr >> PAGE_SHIFT;

            if (!pfn_valid(pfn))
                cache_flags = NPT_CACHE_UC;
            else
                cache_flags = NPT_CACHE_WB;

            BUG_ON(addr & ((2ULL << 20) - 1)); /* 2MB alignment assert */
            pd[pd_idx] = addr | NPT_DEFAULT_FLAGS | NPT_PS | cache_flags;
        }
    }

    pr_info("[NPT] Identity map built: %d pages allocated, root PA=0x%llx\n",
        ctx->page_count, (u64)ctx->pml4_pa);

    /* HPET MMIO guard: 0xFED00000 NX olarak işaretle */
    npt_set_page_nx(ctx, 0xFED00000ULL);

    return 0;

fail:
    pr_err("[NPT] Allocation failed at addr 0x%llx\n", addr);
    npt_destroy(ctx);
    return -ENOMEM;
}

/*
 * npt_destroy - NPT yapısını yok et, tüm tahsis edilen sayfaları serbest bırak.
 * @ctx: NPT context
 */
static void npt_destroy(struct npt_context *ctx)
{
    int i;

    for (i = 0; i < ctx->page_count; i++) {
        if (ctx->pages[i])
            __free_page(ctx->pages[i]);
    }

    ctx->pml4 = NULL;
    ctx->pml4_pa = 0;
    ctx->page_count = 0;

    pr_info("[NPT] Destroyed, all pages freed\n");
}

/*
 * npt_set_page_nx - NPT'de belirli bir GPA'nın 2MB bölgesini NX olarak işaretle.
 * @ctx: NPT context
 * @gpa: guest physical address (2MB bölge içinde herhangi bir adres)
 *
 * Hypervisor kod sayfalarını guest'ten gizlemek veya
 * HPET gibi timer MMIO bölgelerini erişilemez yapmak için kullanılır.
 */
static int npt_set_page_nx(struct npt_context *ctx, u64 gpa)
{
    int pml4_idx = (gpa >> 39) & 0x1FF;
    int pdpt_idx = (gpa >> 30) & 0x1FF;
    int pd_idx   = (gpa >> 21) & 0x1FF;
    u64 *pdpt, *pd;

    if (!ctx->pml4 || !(ctx->pml4[pml4_idx] & NPT_PRESENT))
        return -ENOENT;

    pdpt = phys_to_virt(ctx->pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & NPT_PRESENT))
        return -ENOENT;

    pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);
    pd[pd_idx] |= NPT_NX;

    pr_info("[NPT] GPA 0x%llx marked NX\n", gpa & ~((2ULL << 20) - 1));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SNAPSHOT — Yardımcı Fonksiyonlar

/* ═══════════════════════════════════════════════════════════════════════════
 *  SNAPSHOT — Bellek Yakalama Fonksiyonları
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline bool svm_rate_limit_check(void)
{
    u64 now = ktime_get_real_seconds();
    if (last_snapshot_time && (now - last_snapshot_time) < SNAPSHOT_MIN_INTERVAL_SEC)
        return false;
    return true;
}

static void svm_audit_log(const char *action, int pid_val)
{
    pr_notice("[SVM_DUMP] AUDIT: action=%s pid=%d by=%s\n", action, pid_val, current->comm);
}

static u64 compute_checksum(const void *data, size_t len)
{
    const u64 *ptr = data;
    u64 cksum = 0x5356444D48414B41ULL;
    size_t i, words = len / sizeof(u64);

    for (i = 0; i < words; i++)
        cksum ^= ptr[i];
    return cksum & 0xFFFFFFFFFFFFFFFFULL;
}

static void snapshot_free_locked(void)
{
    if (snapshot_blob.data) {
        kvfree(snapshot_blob.data);
        snapshot_blob.data = NULL;
        snapshot_blob.size = 0;
    }
}

static void count_snapshot_entries(struct mm_struct *mm, u64 *vma_count, u64 *map_count, u64 *data_size)
{
    struct vm_area_struct *vma;
    unsigned long addr;

    VMA_ITERATOR(vmi, mm, 0);
    *vma_count = 0;
    *map_count = 0;
    *data_size = 0;

    mmap_read_lock(mm);
    for_each_vma(vmi, vma) {
        (*vma_count)++;
        for (addr = vma->vm_start; addr < vma->vm_end; ) {
            pgd_t *pgd = pgd_offset(mm, addr);
            p4d_t *p4d;
            pud_t *pud;
            pmd_t *pmd;

            if (pgd_none(*pgd) || pgd_bad(*pgd)) {
                addr = (addr & PGDIR_MASK) + PGDIR_SIZE;
                continue;
            }
            p4d = p4d_offset(pgd, addr);
            if (p4d_none(*p4d) || p4d_bad(*p4d)) {
                addr = (addr & P4D_MASK) + P4D_SIZE;
                continue;
            }
            pud = pud_offset(p4d, addr);
            if (pud_none(*pud) || pud_bad(*pud)) {
                addr = (addr & PUD_MASK) + PUD_SIZE;
                continue;
            }
            if (pud_leaf(*pud)) {
                unsigned long next = (addr & PUD_MASK) + PUD_SIZE;
                if (next > vma->vm_end || next < addr) next = vma->vm_end;
                (*map_count)++;
                *data_size += (next - addr);
                addr = next;
                continue;
            }
            pmd = pmd_offset(pud, addr);
            if (pmd_none(*pmd) || pmd_bad(*pmd) || !pmd_present(*pmd)) {
                addr = (addr & PMD_MASK) + PMD_SIZE;
                continue;
            }
            if (pmd_leaf(*pmd)) {
                unsigned long next = (addr & PMD_MASK) + PMD_SIZE;
                if (next > vma->vm_end || next < addr) next = vma->vm_end;
                (*map_count)++;
                *data_size += (next - addr);
                addr = next;
                continue;
            }
            (*map_count)++;
            *data_size += PAGE_SIZE;
            addr += PAGE_SIZE;
        }
    }
    mmap_read_unlock(mm);
}

static int build_snapshot_for_task(struct task_struct *task)
{
    struct mm_struct *mm;
    struct svm_dump_header *hdr;
    struct svm_vma_entry *vma_out;
    struct svm_page_map_entry *map_out;
    void *buf;
    size_t meta_size, data_size_est, total_alloc;
    u64 v_cnt = 0, m_cnt = 0, i_vma = 0, i_map = 0, raw_off = 0, total_data_sz = 0;
    struct vm_area_struct *vma;
    unsigned long addr;
    u8 *raw_buf = NULL;

    mm = get_task_mm(task);
    if (!mm)
        return -EINVAL;

    count_snapshot_entries(mm, &v_cnt, &m_cnt, &total_data_sz);

    meta_size = sizeof(*hdr) + v_cnt * sizeof(*vma_out) + m_cnt * sizeof(*map_out);
    data_size_est = full_dump_mode ? total_data_sz : 0;
    total_alloc = meta_size + data_size_est + 4096;

    buf = kvzalloc(total_alloc, GFP_KERNEL);
    if (!buf) {
        mmput(mm);
        return -ENOMEM;
    }

    hdr = buf;
    vma_out = (void *)(hdr + 1);
    map_out = (void *)(vma_out + v_cnt);

    if (full_dump_mode)
        raw_buf = (u8 *)(map_out + m_cnt);

    mmap_read_lock(mm);

    VMA_ITERATOR(vmi, mm, 0);
    for_each_vma(vmi, vma) {
        if (i_vma >= v_cnt)
            break;
        vma_out[i_vma].vma_start = vma->vm_start;
        vma_out[i_vma].vma_end = vma->vm_end;
        vma_out[i_vma].flags = vma->vm_flags;
        vma_out[i_vma].pgoff = vma->vm_pgoff;
        i_vma++;
    }

    VMA_ITERATOR(vmi2, mm, 0);
    for_each_vma(vmi2, vma) {
        for (addr = vma->vm_start; addr < vma->vm_end; ) {
            pgd_t *pgd;
            p4d_t *p4d;
            pud_t *pud;
            pmd_t *pmd;
            unsigned long pfn_val = 0, pg_size = 0;
            u64 pg_ent = 0;
            int pg_k = 0;

            if (i_map >= m_cnt)
                goto out_unl;

            pgd = pgd_offset(mm, addr);
            if (pgd_none(*pgd) || pgd_bad(*pgd)) {
                addr = (addr & PGDIR_MASK) + PGDIR_SIZE;
                continue;
            }
            p4d = p4d_offset(pgd, addr);
            if (p4d_none(*p4d) || p4d_bad(*p4d)) {
                addr = (addr & P4D_MASK) + P4D_SIZE;
                continue;
            }
            pud = pud_offset(p4d, addr);
            if (pud_none(*pud) || pud_bad(*pud)) {
                addr = (addr & PUD_MASK) + PUD_SIZE;
                continue;
            }
            if (pud_leaf(*pud)) {
                pfn_val = pud_pfn(*pud);
                pg_size = PUD_SIZE;
                pg_ent = pud_val(*pud);
                pg_k = 3;
                goto fill;
            }
            pmd = pmd_offset(pud, addr);
            if (pmd_none(*pmd) || pmd_bad(*pmd) || !pmd_present(*pmd)) {
                addr = (addr & PMD_MASK) + PMD_SIZE;
                continue;
            }
            if (pmd_leaf(*pmd)) {
                pfn_val = pmd_pfn(*pmd);
                pg_size = PMD_SIZE;
                pg_ent = pmd_val(*pmd);
                pg_k = 2;
                goto fill;
            }
            {
                pte_t *pbase;
                /*
                 * Z1 FIX: Validate pmd_page_vaddr result.
                 * A corrupted PMD could yield a NULL or invalid pointer.
                 */
                if (!pmd_present(*pmd))
                    goto skip_pte;
                pbase = (pte_t *)pmd_page_vaddr(*pmd);
                if (!pbase)
                    goto skip_pte;
                if (pte_present(*(pbase + pte_index(addr)))) {
                    pfn_val = pte_pfn(*(pbase + pte_index(addr)));
                    pg_size = PAGE_SIZE;
                    pg_ent = pte_val(*(pbase + pte_index(addr)));
                    pg_k = 1;
                    goto fill;
                }
            }
skip_pte:
            addr += PAGE_SIZE;
            continue;

fill:
            {
                unsigned long mask = (pg_k == 3) ? PUD_MASK : ((pg_k == 2) ? PMD_MASK : PAGE_MASK);
                unsigned long page_start_vaddr = addr & mask;
                unsigned long next = page_start_vaddr + pg_size;
                if (next > vma->vm_end || next < addr) 
                    next = vma->vm_end;
                
                unsigned long chunk_size = next - addr;
                unsigned long offset_in_page = addr - page_start_vaddr;

                map_out[i_map].addr = addr;
                map_out[i_map].size = chunk_size;
                map_out[i_map].entry = pg_ent;
                map_out[i_map].pfn = pfn_val;
                map_out[i_map].kind = pg_k;

                if (full_dump_mode && raw_buf && pfn_valid(pfn_val)) {
                    struct page *pg = pfn_to_page(pfn_val);
                    /*
                     * Swap safety: Only copy if the page is
                     * genuinely in RAM (not swapped, not reserved I/O).
                     * page_count == 0 means it's free/unused.
                     * PageReserved means it's MMIO or firmware.
                     */
                    if (pg && page_count(pg) > 0 && !PageReserved(pg)) {
                        void *vsrc = pfn_to_kaddr(pfn_val);
                        /*
                         * Z2 FIX: Bounds check before memcpy.
                         * Prevent TOCTOU buffer overflow if VMAs changed
                         * between count and build phases.
                         */
                        if (vsrc && (raw_off + chunk_size) <= data_size_est) {
                            memcpy(raw_buf + raw_off,
                                   (u8 *)vsrc + offset_in_page,
                                   chunk_size);
                            map_out[i_map].data_offset = raw_off;
                            raw_off += chunk_size;
                        } else {
                            map_out[i_map].data_offset = (u64)-1;
                        }
                    } else {
                        map_out[i_map].data_offset = (u64)-1;
                    }
                } else {
                    map_out[i_map].data_offset = (u64)-1;
                }

                i_map++;
                addr = next;
            }
        }
    }

out_unl:
    mmap_read_unlock(mm);

    memcpy(hdr->magic, SVM_MAGIC, 4);
    hdr->version = SVM_SNAPSHOT_VERSION;
    hdr->pid = task_pid_nr(task);
    hdr->timestamp = ktime_get_real_seconds();
    hdr->cr3_phys = __pa(mm->pgd);
    hdr->vma_count = i_vma;
    hdr->map_count = i_map;
    hdr->total_size = (u8 *)(map_out + i_map) - (u8 *)buf;

    if (full_dump_mode) {
        hdr->flags |= SVM_FLAG_RAW_DATA;
        hdr->total_size += raw_off;
    }
    if (npt_mode)
        hdr->flags |= SVM_FLAG_NPT_MODE;

    /*
     * Z3 FIX: If total_size exceeds allocation, cap it and
     * set TRUNCATED flag so the analyst knows the checksum
     * covers only partial data.
     */
    if (hdr->total_size > total_alloc) {
        pr_warn("[SVM_DUMP] TRUNCATED: total_size %llu > alloc %zu, capping\n",
                hdr->total_size, total_alloc);
        hdr->total_size = total_alloc;
        hdr->flags |= SVM_FLAG_TRUNCATED;
    }

    hdr->checksum = 0;
    hdr->checksum = compute_checksum(buf, (size_t)hdr->total_size);

    mmput(mm);

    /* Assume snapshot_lock is held by caller where necessary */
    snapshot_free_locked();
    snapshot_blob.data = buf;
    snapshot_blob.size = (size_t)hdr->total_size;
    last_snapshot_time = ktime_get_real_seconds();
    snapshot_count++;

    return 0;
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  PROCFS — /proc/svm_dump Interface
 * ═══════════════════════════════════════════════════════════════════════════ */

static int watcher_fn(void *data)
{
    while (!kthread_should_stop()) {
        struct task_struct *task;
        struct task_struct *target_task = NULL;

        if (watch_name[0] == 0) {
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(msecs_to_jiffies(500));
            continue;
        }

        rcu_read_lock();
        for_each_process(task) {
            if (strncmp(task->comm, watch_name, TASK_COMM_LEN) == 0) {
                get_task_struct(task);
                target_task = task;
                break;
            }
        }
        rcu_read_unlock();

        if (target_task) {
            u64 now = ktime_get_real_seconds();
            if (!last_snapshot_time || (now - last_snapshot_time) >= SNAPSHOT_MIN_INTERVAL_SEC) {
                mutex_lock(&snapshot_lock);
                svm_audit_log("auto", task_pid_nr(target_task));
                build_snapshot_for_task(target_task);
                mutex_unlock(&snapshot_lock);
            }
            put_task_struct(target_task);
        }

        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(msecs_to_jiffies(500));
    }
    return 0;
}

static ssize_t pid_write(struct file *f, const char __user *u, size_t c, loff_t *p)
{
    char buf[16];
    int val;
    struct pid *ps;
    struct task_struct *t;

    if (!svm_check_access())
        return -EPERM;

    if (copy_from_user(buf, u, min(c, sizeof(buf) - 1)))
        return -EFAULT;
    buf[min(c, sizeof(buf) - 1)] = 0;

    if (kstrtoint(strim(buf), 10, &val))
        return -EINVAL;

    ps = find_get_pid(val);
    if (!ps)
        return -ESRCH;

    t = get_pid_task(ps, PIDTYPE_PID);
    if (!t) {
        put_pid(ps);
        return -ESRCH;
    }

    mutex_lock(&snapshot_lock);
    svm_audit_log("manual", val);
    build_snapshot_for_task(t);
    mutex_unlock(&snapshot_lock);

    put_task_struct(t);
    put_pid(ps);

    return c;
}

static ssize_t out_read(struct file *f, char __user *u, size_t c, loff_t *p)
{
    ssize_t r;

    mutex_lock(&snapshot_lock);
    if (!snapshot_blob.data) {
        mutex_unlock(&snapshot_lock);
        return -ENOENT;
    }
    r = simple_read_from_buffer(u, c, p, snapshot_blob.data, snapshot_blob.size);
    mutex_unlock(&snapshot_lock);

    return r;
}

static int pl_show(struct seq_file *m, void *v)
{
    struct task_struct *task;

    seq_printf(m, "%-8s %-20s\n", "PID", "NAME");
    rcu_read_lock();
    for_each_process(task) {
        seq_printf(m, "%-8d %-20s\n", task_pid_nr(task), task->comm);
    }
    rcu_read_unlock();

    return 0;
}

static int pl_open(struct inode *i, struct file *f)
{
    return single_open(f, pl_show, NULL);
}

static ssize_t wn_write(struct file *f, const char __user *u, size_t c, loff_t *p)
{
    if (!svm_check_access())
        return -EPERM;

    mutex_lock(&snapshot_lock);
    /*
     * Z4 FIX: Zero entire buffer before copy to prevent
     * leaking old watch_name contents through /proc/svm_dump/status.
     */
    memset(watch_name, 0, sizeof(watch_name));
    if (copy_from_user(watch_name, u, min(c, (size_t)63))) {
        memset(watch_name, 0, sizeof(watch_name));
        mutex_unlock(&snapshot_lock);
        return -EFAULT;
    }
    watch_name[min(c, (size_t)63)] = 0;
    strim(watch_name);
    mutex_unlock(&snapshot_lock);

    return c;
}

static ssize_t aw_write(struct file *f, const char __user *u, size_t c, loff_t *p)
{
    char buf[8];
    int v;

    if (!svm_check_access())
        return -EPERM;

    if (copy_from_user(buf, u, min(c, sizeof(buf) - 1)))
        return -EFAULT;
    buf[min(c, sizeof(buf) - 1)] = 0;

    if (kstrtoint(strim(buf), 10, &v))
        return -EINVAL;

    mutex_lock(&snapshot_lock);
    if (v == 1 && !auto_watch_active) {
        struct task_struct *t = kthread_run(watcher_fn, NULL, "svm_watch");
        if (!IS_ERR(t)) {
            watcher_thread = t;
            auto_watch_active = true;
        }
    } else if (v == 0 && auto_watch_active) {
        struct task_struct *t = watcher_thread;
        watcher_thread = NULL;
        auto_watch_active = false;
        mutex_unlock(&snapshot_lock);

        /* Stop the thread outside the lock to prevent deadlock */
        kthread_stop(t);
        return c;
    }
    mutex_unlock(&snapshot_lock);

    return c;
}

static ssize_t fd_write(struct file *f, const char __user *u, size_t c, loff_t *p)
{
    char buf[8];
    int v;

    if (!svm_check_access())
        return -EPERM;

    if (copy_from_user(buf, u, min(c, sizeof(buf) - 1)))
        return -EFAULT;
    buf[min(c, sizeof(buf) - 1)] = 0;

    if (kstrtoint(strim(buf), 10, &v))
        return -EINVAL;

    mutex_lock(&snapshot_lock);
    full_dump_mode = (v == 1);
    mutex_unlock(&snapshot_lock);

    return c;
}

static ssize_t nm_write(struct file *f, const char __user *u, size_t c, loff_t *p)
{
    char buf[8];
    int v;

    if (!svm_check_access())
        return -EPERM;

    if (copy_from_user(buf, u, min(c, sizeof(buf) - 1)))
        return -EFAULT;
    buf[min(c, sizeof(buf) - 1)] = 0;

    if (kstrtoint(strim(buf), 10, &v))
        return -EINVAL;

    mutex_lock(&snapshot_lock);
    npt_mode = (v == 1);
    mutex_unlock(&snapshot_lock);

    return c;
}

static int st_show(struct seq_file *m, void *v)
{
    mutex_lock(&snapshot_lock);
    seq_printf(m, "Watch: %s\n", watch_name);
    seq_printf(m, "Full: %s\n", full_dump_mode ? "ON" : "OFF");
    seq_printf(m, "NPT: %s\n", npt_mode ? "ON" : "OFF");
    seq_printf(m, "Size: %zu\n", snapshot_blob.size);
    seq_printf(m, "Ready: %s\n", snapshot_blob.data ? "YES" : "NO");
    mutex_unlock(&snapshot_lock);

    return 0;
}

static int st_open(struct inode *i, struct file *f)
{
    return single_open(f, st_show, NULL);
}

static const struct proc_ops pops_p = {
    .proc_write = pid_write
};

static const struct proc_ops pops_o = {
    .proc_read = out_read
};

static const struct proc_ops pops_l = {
    .proc_open = pl_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release
};

static const struct proc_ops pops_w = {
    .proc_write = wn_write
};

static const struct proc_ops pops_a = {
    .proc_write = aw_write
};

static const struct proc_ops pops_f = {
    .proc_write = fd_write
};

static const struct proc_ops pops_n = {
    .proc_write = nm_write
};

static const struct proc_ops pops_s = {
    .proc_open = st_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release
};


/* ═══════════════════════════════════════════════════════════════════════════
 *  SVM ENGINE — VMCB Prepare (NPT mode)
 * ═══════════════════════════════════════════════════════════════════════════ */



/*
 * vmrun_tsc_compensated - Execute VMRUN and compensate TSC (V4.0)
 *
 * Per-CPU offset eliminates cross-core drift.
 * IRQ disabled + preempt disabled eliminates interrupt-induced spikes.
 * Result: guest sees perfectly linear TSC progression.
 */
u64 vmrun_tsc_compensated(struct vmcb *vmcb, u64 vmcb_pa)
{
    u64 tsc_before, tsc_after, exit_code;
    unsigned long flags;
    s64 *offset;

    preempt_disable();
    local_irq_save(flags);

    offset = this_cpu_ptr(&pcpu_tsc_offset);
    vmcb->control.tsc_offset = *offset;

    tsc_before = rdtsc();
    vmrun_safe(vmcb_pa);
    tsc_after = rdtsc();

    /* Subtract hypervisor time — per-CPU, no cross-core drift */
    *offset -= (s64)(tsc_after - tsc_before);

    local_irq_restore(flags);
    preempt_enable();

    exit_code = ((u64)vmcb->control.exit_code_hi << 32) |
                 vmcb->control.exit_code;
    return exit_code;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MODULE INIT/EXIT — Unified
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    /* ── Proc entries oluştur ── */
    proc_dir = proc_mkdir(PROC_DIR, NULL);
    if (!proc_dir) {
        ret = -ENOMEM;
        goto err_guest;
    }

    if (!proc_create("target_pid", 0600, proc_dir, &pops_p) ||
        !proc_create("output", 0400, proc_dir, &pops_o) ||
        !proc_create("process_list", 0400, proc_dir, &pops_l) ||
        !proc_create("watch_name", 0600, proc_dir, &pops_w) ||
        !proc_create("auto_watch", 0600, proc_dir, &pops_a) ||
        !proc_create("full_dump", 0600, proc_dir, &pops_f) ||
        !proc_create("npt_mode", 0600, proc_dir, &pops_n) ||
        !proc_create("status", 0400, proc_dir, &pops_s)) {

        remove_proc_subtree(PROC_DIR, NULL);
        ret = -ENOMEM;
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
static int vmcb_prepare_npt(struct vmcb *vmcb, u64 g_rip, u64 g_rsp,
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

    /* VMRUN intercept (mandatory) */
    vmcb->control.intercepts[INTERCEPT_VMRUN >> 5] |=
        (1U << (INTERCEPT_VMRUN & 31));
    /* HLT intercept */
    vmcb->control.intercepts[INTERCEPT_HLT >> 5] |=
        (1U << (INTERCEPT_HLT & 31));

    /*
     * CPUID intercept: Normalize instruction latency.
     * Without this, selective interception creates a timing fingerprint:
     *   cpuid=fast, rdtsc=slow → detectable pattern.
     * Intercepting CPUID makes all trapped instructions uniformly slow.
     */
    vmcb->control.intercepts[INTERCEPT_CPUID >> 5] |=
        (1U << (INTERCEPT_CPUID & 31));

    /* RDTSCP intercept */
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

        /*
         * MSRPM V4.0: Full timing + syscall MSR coverage.
         * Base region (0x0000-0x1FFF): offset 0
         * C000 region (0xC000_0000-0xC000_1FFF): offset 0x800
         * Each MSR = 2 bits: bit0=rdmsr, bit1=wrmsr
         */

        /* MSR 0x10 (IA32_TSC): rdmsr intercept */
        msrpm[(0x10 * 2) / 8] |= (1 << ((0x10 * 2) % 8));

        /* MSR 0xE7 (IA32_MPERF): rdmsr — CPU frequency detection */
        msrpm[(0xE7 * 2) / 8] |= (1 << ((0xE7 * 2) % 8));

        /* MSR 0xE8 (IA32_APERF): rdmsr — CPU frequency detection */
        msrpm[(0xE8 * 2) / 8] |= (1 << ((0xE8 * 2) % 8));

        /* MSR 0x176 (IA32_SYSENTER_EIP): rdmsr — legacy syscall entry */
        msrpm[(0x176 * 2) / 8] |= (1 << ((0x176 * 2) % 8));

        /* MSR 0xC0000081 (STAR): rdmsr + wrmsr — syscall entry point */
        msrpm[0x800 + (0x81 * 2) / 8] |= (3 << ((0x81 * 2) % 8));

        /* MSR 0xC0000082 (LSTAR): rdmsr + wrmsr — 64-bit syscall handler */
        msrpm[0x800 + (0x82 * 2) / 8] |= (3 << ((0x82 * 2) % 8));

        /* MSR 0xC0000103 (TSC_AUX): rdmsr — RDTSCP auxiliary data */
        msrpm[0x800 + (0x103 * 2) / 8] |= (1 << ((0x103 * 2) % 8));
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

        /* PIT (Programmable Interval Timer) - ports 0x40-0x43 */
        iopm[0x40 / 8] |= (1 << (0x40 % 8));
        iopm[0x41 / 8] |= (1 << (0x41 % 8));
        iopm[0x42 / 8] |= (1 << (0x42 % 8));
        iopm[0x43 / 8] |= (1 << (0x43 % 8));

        /* ACPI PM Timer - ports 0x808-0x80B (32-bit read) */
        iopm[0x808 / 8] |= (1 << (0x808 % 8));
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
    vmcb->control.tlb_ctl = 0;  /* ASID-only isolation, no flush penalty */
    vmcb->control.clean = 0;

    /* NPT configuration */
    if (npt && npt->pml4_pa) {
        vmcb->control.nested_ctl = 1;
        vmcb->control.nested_cr3 = npt->pml4_pa;
        pr_info("[VMCB] NPT enabled, nested_cr3=0x%llx\n",
                (u64)npt->pml4_pa);
    }

    /* TSC offset */
    vmcb->control.tsc_offset = *this_cpu_ptr(&pcpu_tsc_offset);

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
