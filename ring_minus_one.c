/*

AMD64 SVM – Güvenli, minimal, çalışan örnek (Revize Edilmiş)

Düzeltmeler:


1. Host State Save (GPR'ler stack'e push/pop edildi)



2. GIF Yönetimi (clgi ve stgi eklendi)



3. CR3 PCID/KPTI maskelemesi eklendi



4. Guest kod sayfası için set_memory_x eklendi (NX bitini kaldırmak için)
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>
#include <asm/svm.h>
#include <asm/io.h>
#include <linux/mm.h>
#include <asm/set_memory.h>   /* set_memory_x için eklendi */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hakan");
MODULE_DESCRIPTION("AMD-V SVM – Stabil ve Güvenli");

/* Global değişkenler */
static struct vmcb *guest_vmcb;
static phys_addr_t guest_vmcb_pa;
static void *hsave_va;
static phys_addr_t hsave_pa;
static u8 *guest_code_page;
static u8 *guest_stack_page;

/* Saf makine kodu: hlt + sonsuz döngü /
static const u8 guest_code_bin[] = {
0xf4,        / hlt /
0xeb, 0xfc,  / jmp -2  (sonsuz döngü) */
};

/* SVM desteği var mı? */
static bool svm_supported(void)
{
u32 eax, ebx, ecx, edx;
cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
return !!(ecx & (1 << 2));
}

/* * GÜVENLİ VMRUN ÇAĞRISI (Fix 1 & Fix 2)

Host GPR'lerini korur, Kesmeleri (GIF) kapatır ve açar.
/
static void vmrun_safe(u64 vmcb_pa)
{
asm volatile (
/ Host register'larını güvenle stack'e sakla */
"push %%rbx \n\t"
"push %%rcx \n\t"
"push %%rdx \n\t"
"push %%rsi \n\t"
"push %%rdi \n\t"
"push %%rbp \n\t"
"push %%r8  \n\t"
"push %%r9  \n\t"
"push %%r10 \n\t"
"push %%r11 \n\t"
"push %%r12 \n\t"
"push %%r13 \n\t"
"push %%r14 \n\t"
"push %%r15 \n\t"

"clgi \n\t"            /* Kesmeleri durdur (Fix 2) */  
 "vmrun %%rax \n\t"     /* Konuğu Başlat (RAX = vmcb_pa) */  
 "stgi \n\t"            /* Kesmeleri tekrar aç (Fix 2) */  

 /* Host register'larını geri yükle */  
 "pop %%r15 \n\t"  
 "pop %%r14 \n\t"  
 "pop %%r13 \n\t"  
 "pop %%r12 \n\t"  
 "pop %%r11 \n\t"  
 "pop %%r10 \n\t"  
 "pop %%r9  \n\t"  
 "pop %%r8  \n\t"  
 "pop %%rbp \n\t"  
 "pop %%rdi \n\t"  
 "pop %%rsi \n\t"  
 "pop %%rdx \n\t"  
 "pop %%rcx \n\t"  
 "pop %%rbx \n\t"  
 :  
 : "a"(vmcb_pa)  
 : "memory", "cc"

);
}


static void raw_cr3_flush(void)
{
unsigned long cr3;
asm volatile("mov %%cr3, %0" : "=r"(cr3));
asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static int __init svm_module_init(void)
{
u64 efer_val, cr0, cr3, cr4, rflags;
u16 cs, ds, ss, es, fs, gs;
struct desc_ptr gdtr, idtr;
u16 tr;
u64 fs_base, gs_base;
int ret;

if (!svm_supported()) {  
	pr_err("SVM bu CPU'da desteklenmiyor.\n");  
	return -ENODEV;  
}  

rdmsrl(MSR_EFER, efer_val);  
if (!(efer_val & EFER_SVME)) {  
	efer_val |= EFER_SVME;  
	wrmsrl(MSR_EFER, efer_val);  
	pr_info("SVME biti etkinleştirildi.\n");  
}  

hsave_va = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);  
if (!hsave_va) {  
	pr_err("HSAVE sayfası ayrılamadı.\n");  
	return -ENOMEM;  
}  
hsave_pa = virt_to_phys(hsave_va);  
wrmsrl(MSR_VM_HSAVE_PA, hsave_pa);  

guest_vmcb = (struct vmcb *)__get_free_page(GFP_KERNEL | __GFP_ZERO);  
if (!guest_vmcb) {  
	ret = -ENOMEM;  
	goto err_free_hsave;  
}  
guest_vmcb_pa = virt_to_phys(guest_vmcb);  

guest_code_page = (u8 *)__get_free_page(GFP_KERNEL);  
guest_stack_page = (u8 *)__get_free_page(GFP_KERNEL);  
if (!guest_code_page || !guest_stack_page) {  
	pr_err("Guest bellek sayfaları ayrılamadı.\n");  
	ret = -ENOMEM;  
	goto err_free_all;  
}  

/* Konuk kodu sayfasını çalıştırılabilir (Executable) yap - NX Bit bypass */  
set_memory_x((unsigned long)guest_code_page, 1);  

memcpy(guest_code_page, guest_code_bin, sizeof(guest_code_bin));  
u64 guest_rsp = (u64)guest_stack_page + PAGE_SIZE - 8;  

cr0 = native_read_cr0();  
cr3 = __read_cr3();  
cr4 = native_read_cr4();  
asm volatile("pushf; pop %0" : "=r"(rflags));  
asm volatile("mov %%cs, %0" : "=r"(cs));  
asm volatile("mov %%ds, %0" : "=r"(ds));  
asm volatile("mov %%ss, %0" : "=r"(ss));  
asm volatile("mov %%es, %0" : "=r"(es));  
asm volatile("mov %%fs, %0" : "=r"(fs));  
asm volatile("mov %%gs, %0" : "=r"(gs));  

asm volatile("sgdt %0" : "=m"(gdtr));  
asm volatile("sidt %0" : "=m"(idtr));  
asm volatile("str %0" : "=r"(tr));  

rdmsrl(MSR_FS_BASE, fs_base);  
rdmsrl(MSR_GS_BASE, gs_base);  

guest_vmcb->control.intercepts[INTERCEPT_HLT >> 5] |=  
	(1ULL << (INTERCEPT_HLT & 31));  
guest_vmcb->control.asid = 1;  
guest_vmcb->control.exit_code = 0;  
guest_vmcb->control.exit_code_hi = 0;  

guest_vmcb->save.cs.selector = cs;  
guest_vmcb->save.cs.attrib   = 0x0A9B;   
guest_vmcb->save.cs.limit    = 0xFFFFFFFF;  
guest_vmcb->save.cs.base     = 0;  

guest_vmcb->save.ds.selector = ds;  
guest_vmcb->save.ds.attrib   = 0x293;  
guest_vmcb->save.ds.limit    = 0xFFFFFFFF;  
guest_vmcb->save.ds.base     = 0;  

guest_vmcb->save.ss.selector = ss;  
guest_vmcb->save.ss.attrib   = 0x293;  
guest_vmcb->save.ss.limit    = 0xFFFFFFFF;  
guest_vmcb->save.ss.base     = 0;  

guest_vmcb->save.es.selector = es;  
guest_vmcb->save.es.attrib   = 0x293;  
guest_vmcb->save.es.limit    = 0xFFFFFFFF;  
guest_vmcb->save.es.base     = 0;  

guest_vmcb->save.fs.selector = fs;  
guest_vmcb->save.fs.attrib   = 0x293;  
guest_vmcb->save.fs.limit    = 0xFFFFFFFF;  
guest_vmcb->save.fs.base     = fs_base;  

guest_vmcb->save.gs.selector = gs;  
guest_vmcb->save.gs.attrib   = 0x293;  
guest_vmcb->save.gs.limit    = 0xFFFFFFFF;  
guest_vmcb->save.gs.base     = gs_base;  

guest_vmcb->save.gdtr.limit  = gdtr.size;  
guest_vmcb->save.gdtr.base   = gdtr.address;  
guest_vmcb->save.gdtr.attrib = 0;  

guest_vmcb->save.idtr.limit  = idtr.size;     
guest_vmcb->save.idtr.base   = idtr.address;  
guest_vmcb->save.idtr.attrib = 0;  

guest_vmcb->save.tr.selector = tr;  
guest_vmcb->save.tr.limit    = 0xFFFF;  
guest_vmcb->save.tr.base     = 0;  
guest_vmcb->save.tr.attrib   = 0x28B;        

guest_vmcb->save.cr0 = cr0;  
  
/* Fix 3: CR3 PCID / KPTI maskelemesi (Sadece fiziksel adresi alıyoruz) */  
guest_vmcb->save.cr3 = cr3 & 0xFFFFFFFFFFFFF000ULL;   
  
guest_vmcb->save.cr4 = cr4;  
guest_vmcb->save.efer = efer_val;           

guest_vmcb->save.dr6 = 0xFFFF0FF0;  
guest_vmcb->save.dr7 = 0x00000400;  

guest_vmcb->save.rip = (u64)guest_code_page;  
guest_vmcb->save.rsp = guest_rsp;  
guest_vmcb->save.rflags = rflags & ~X86_EFLAGS_IF;   

pr_info("Guest RIP: 0x%llx, RSP: 0x%llx\n", guest_vmcb->save.rip, guest_vmcb->save.rsp);  
pr_info("Konuk başlatılıyor...\n");  

/* Güvenli vmrun çağrısı */  
vmrun_safe(guest_vmcb_pa);  

u64 exit_code = ((u64)guest_vmcb->control.exit_code_hi << 32) |  
                 guest_vmcb->control.exit_code;  
pr_info("VMEXIT oluştu, çıkış kodu: 0x%llx\n", exit_code);  

raw_cr3_flush();  

return 0;

err_free_all:
if (guest_code_page)
free_page((unsigned long)guest_code_page);
if (guest_stack_page)
free_page((unsigned long)guest_stack_page);
if (guest_vmcb)
free_page((unsigned long)guest_vmcb);
err_free_hsave:
free_page((unsigned long)hsave_va);
return ret;
}

static void __exit svm_module_exit(void)
{
u64 efer_val;
rdmsrl(MSR_EFER, efer_val);
efer_val &= ~EFER_SVME;
wrmsrl(MSR_EFER, efer_val);

if (guest_code_page) {  
	/* Sayfayı geri vermeden önce NX bitini eski haline getirmek iyi pratiktir */  
	set_memory_nx((unsigned long)guest_code_page, 1);  
	free_page((unsigned long)guest_code_page);  
}  
if (guest_stack_page)  
	free_page((unsigned long)guest_stack_page);  
if (guest_vmcb)  
	free_page((unsigned long)guest_vmcb);  
if (hsave_va)  
	free_page((unsigned long)hsave_va);  

pr_info("SVM modülü temizlendi.\n");

}

module_init(svm_module_init);
module_exit(svm_module_exit);
