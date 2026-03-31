/*
 * svm_decode.h — Lightweight x86-64 Instruction Decoder Interface
 *
 * Phase 22: Hypervisor-internal micro-emulator for NPT Split-View.
 * Decodes and emulates register-only instructions in-place.
 * Memory-access instructions return DECODE_ACTION_MEMOP for
 * fallback to NX-remove + TF single-step.
 */
#ifndef SVM_DECODE_H
#define SVM_DECODE_H

#include <linux/types.h>

/* Action flags returned in upper bits of svm_decode_insn result */
#define DECODE_ACTION_EMULATED  (1U << 8)   /* Komut emüle edildi */
#define DECODE_ACTION_BRANCH    (1U << 9)   /* RIP değiştirildi (dallanma) */
#define DECODE_ACTION_MEMOP     (1U << 10)  /* Bellek erişimi — single-step fallback */
#define DECODE_LEN_MASK         0xFF        /* Alt 8 bit = komut uzunluğu */

/* Decode cache entry — JIT hash verification for SMC resistance */
#define DECODE_CACHE_BITS   6
#define DECODE_CACHE_SIZE   (1 << DECODE_CACHE_BITS)  /* 64 entry */
#define DECODE_CACHE_MASK   (DECODE_CACHE_SIZE - 1)

struct decode_cache_entry {
	u64 gva;           /* Guest Virtual Address */
	u32 opcode_hash;   /* CRC32 of first 4 bytes */
	u16 result;        /* Cached decode result (len + flags) */
	u16 pad;
};

/*
 * svm_decode_insn — Decode and optionally emulate one x86-64 instruction
 *
 * @insn_buf:   Pointer to 15-byte instruction buffer (pre-fetched from guest)
 * @gregs:      Pointer to guest_regs (software-saved GPRs)
 * @vmcb_save:  Pointer to vmcb->save (hardware-saved RAX/RSP/RIP/RFLAGS)
 *
 * Returns: packed u32
 *   bits [7:0]  = instruction length (0 = decode failed)
 *   bit  8      = DECODE_ACTION_EMULATED  (register state modified)
 *   bit  9      = DECODE_ACTION_BRANCH    (RIP was patched)
 *   bit  10     = DECODE_ACTION_MEMOP     (needs single-step fallback)
 *
 * Defined in svm_decode.S (pure assembly, no C compiler interference).
 */
extern u32 svm_decode_insn(const u8 *insn_buf,
			   struct guest_regs *gregs,
			   void *vmcb_save);

#endif /* SVM_DECODE_H */
