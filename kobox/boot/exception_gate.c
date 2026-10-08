// SPDX-License-Identifier: GPL-2.0-only
#include "exception.h"
#include <linux/bug.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <asm/asm.h>

static noinline int fixup_probe(unsigned long address)
{
	unsigned long value = 0x123456789abcdef0UL;
	long result = 0;

	/* Unlike a synthetic frame, this instruction must fault through the native
	 * fiber stub and upstream extable handler, including its GPR/IP updates.
	 */
	asm volatile("1: movq (%[address]), %[value]\n"
		     "2:\n"
		     _ASM_EXTABLE_TYPE_REG(1b, 2b, EX_TYPE_EFAULT_REG, %[result])
		     : [result] "+&r" (result), [value] "+&r" (value)
		     : [address] "r" (address) : "memory");
	return result == -EFAULT && value == 0x123456789abcdef0UL ? 0 : -EINVAL;
}

static void pending_reschedule(void)
{
	/* Inject the scheduler's pending state without invoking a host callback
	 * between publication and the faulting instruction.
	 */
	set_tsk_need_resched(current);
	set_preempt_need_resched();
}

__attribute__((visibility("default")))
int kobox_linux_exception_verify(void)
{
	cpumask_t saved;
	struct task_struct *owner = current;
	unsigned long initial = kobox_linux_exception_warnings();
	unsigned int cpu, mode, completed = 0;
	int result = 0;

	if (initial || !preemptible() || in_interrupt())
		return -EINVAL;
	cpumask_copy(&saved, current->cpus_ptr);
	for_each_online_cpu(cpu) {
		result = set_cpus_allowed_ptr(current, cpumask_of(cpu));
		if (result)
			break;
		for (mode = 0; mode < 4; ++mode) {
			unsigned long warnings = kobox_linux_exception_warnings();
			unsigned int count = preempt_count();
			volatile bool trigger = true;

			if (!preemptible() || current != owner)
				goto invalid;
			pr_info("KOBOX_EXCEPTION_CASE cpu=%u mode=%u BEGIN\n", cpu, mode);
			if (mode == 1)
				preempt_disable();
			if (mode == 2)
				local_irq_disable();
			if (mode == 3)
				pending_reschedule();
			WARN_ON(trigger);
			if (mode == 3)
				pending_reschedule();
			/* Zero is unmapped; bit 63 without sign extension is noncanonical.
			 * Both are reads only, so a broken recovery cannot write memory.
			 */
			result = fixup_probe(0);
			if (mode == 3)
				pending_reschedule();
			result = fixup_probe(1UL << 63) ?: result;
			if (mode == 2) {
				if (!irqs_disabled())
					result = -EINVAL;
				local_irq_enable();
			}
			if (mode == 1) {
				if (preempt_count() != count + PREEMPT_DISABLE_OFFSET)
					result = -EINVAL;
				preempt_enable();
			}
			cond_resched();
			if (result || current != owner || !preemptible() ||
			    raw_smp_processor_id() != cpu || preempt_count() != count ||
			    kobox_linux_exception_warnings() != warnings + 1)
				goto invalid;
			++completed;
			pr_info("KOBOX_EXCEPTION_CASE cpu=%u mode=%u warn=1 pf=1 gp=1 PASS\n",
				cpu, mode);
		}
	}
	if (!result && completed != num_online_cpus() * 4)
		result = -EINVAL;
	goto out;
invalid:
	result = -EINVAL;
out:
	return set_cpus_allowed_ptr(current, &saved) ?: result;
}
