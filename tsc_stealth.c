/*
 * tsc_stealth.c — Per-CPU TSC Offset Compensation (V4.0)
 *
 * Per-CPU offset eliminates cross-core drift.
 * IRQ disabled + preempt disabled eliminates interrupt-induced spikes.
 * Result: guest sees perfectly linear TSC progression.
 */

#include "ring_minus_one.h"

DEFINE_PER_CPU(s64, pcpu_tsc_offset);

/*
 * vmrun_tsc_compensated - Execute VMRUN and compensate TSC
 * @vmcb: Virtual Machine Control Block
 * @vmcb_pa: Physical address of VMCB
 *
 * Wraps vmrun_safe with per-CPU TSC offset tracking.
 * IRQ ve preemption devre dışı bırakılarak interrupt spike'ları önlenir.
 */
u64 vmrun_tsc_compensated(struct svm_context *ctx) {
  u64 tsc_before, tsc_after, exit_code;
  unsigned long flags;
  s64 *offset;

  preempt_disable();
  local_irq_save(flags);

  offset = this_cpu_ptr(&pcpu_tsc_offset);
  ctx->vmcb->control.tsc_offset = *offset;

  /* Mark stable fields clean — only TSC changes per-VMRUN */
  ctx->vmcb->control.clean = VMCB_CLEAN_STABLE;

  tsc_before = rdtsc();
  vmrun_safe(ctx->vmcb_pa);
  tsc_after = rdtsc();

  /* Subtract hypervisor time — per-CPU, no cross-core drift */
  *offset -= (s64)(tsc_after - tsc_before);

  local_irq_restore(flags);
  preempt_enable();

  exit_code = ((u64)ctx->vmcb->control.exit_code_hi << 32) |
              ctx->vmcb->control.exit_code;
  return exit_code;
}

/*
 * tsc_offset_reset - Bu CPU'nun TSC offset'ini sıfırla
 */
void tsc_offset_reset(void) { this_cpu_write(pcpu_tsc_offset, 0); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  TSC Jitter (PRNG tabanlı gürültü)
 *
 *  Without jitter: every CPUID takes exactly N cycles → detected as emulated.
 *  With jitter: Gaussian-like noise makes timing look like real hardware
 *  (cache miss, pipeline stall, branch misprediction variance).
 *
 *  Uses a fast 64-bit LCG (Linear Congruential Generator).
 * ═══════════════════════════════════════════════════════════════════════════
 */

static DEFINE_PER_CPU(u64, jitter_state);

u64 tsc_jitter(u64 min, u64 max)
{
	u64 *state = this_cpu_ptr(&jitter_state);

	if (!*state)
		*state = 0x5DEECE66DULL ^ rdtsc();

	/* LCG: state = state * 6364136223846793005 + 1442695040888963407 */
	*state = *state * 6364136223846793005ULL + 1442695040888963407ULL;

	/* Defensive: prevent UB if caller passes min > max */
	if (unlikely(min >= max))
		return min;

	return min + ((*state >> 33) % (max - min + 1));
}
