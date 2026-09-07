// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/vmalloc.h>

#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/fpu/sched.h>
#include <asm/pgtable.h>
#include <asm/runtime-const.h>
#include <asm/sync_core.h>
#include <asm/text-patching.h>

/* fpu__init_system() remains upstream, including task/FPU allocation sizing. */
void fpu__init_system(void);
DECLARE_PER_CPU(bool, kernel_fpu_allowed);

void fpu__init_cpu(void)
{
	/* pthread execution already has host-managed CR0/CR4 and FPU storage.
	 * This platform exposes legacy FP/SSE, not guest-owned XCR0 or XSAVES.
	 * FNINIT and Linux's per-CPU kernel-FPU permission are still necessary.
	 */
	if (!boot_cpu_has(X86_FEATURE_FPU) ||
	    boot_cpu_has(X86_FEATURE_XSAVE) ||
	    boot_cpu_has(X86_FEATURE_XSAVES))
		panic("unsupported hosted FPU configuration");
	asm volatile("fninit");
	this_cpu_write(kernel_fpu_allowed, true);
}

static void __init hosted_cpu_features(void)
{
	struct cpuinfo_x86 *cpu = &boot_cpu_data;
	const u32 required = BIT(X86_FEATURE_FPU) | BIT(X86_FEATURE_CX8) |
		BIT(X86_FEATURE_CMOV) | BIT(X86_FEATURE_FXSR) |
		BIT(X86_FEATURE_XMM) | BIT(X86_FEATURE_XMM2);
	u32 eax, ebx, ecx, edx;

	/* Use Linux's CPUID family/model decoder; do not run vendor firmware,
	 * mitigation, power-management or privileged feature setup routines.
	 * Logical CPUs expose the common x86-64 instruction baseline only, so
	 * migration among host CPUs cannot enable a boot-CPU-only alternative.
	 */
	cpu_detect(cpu);
	cpuid(1, &eax, &ebx, &ecx, &edx);
	if ((edx & required) != required)
		panic("host lacks required x86-64 FP/SSE instruction baseline");
	cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
	if (!(edx & BIT(X86_FEATURE_NX % 32)))
		panic("host lacks x86 execute-disable page protection");
	cpu->x86_vendor = X86_VENDOR_UNKNOWN;
	strscpy(cpu->x86_vendor_id, "HostedX86CPU");
	strscpy(cpu->x86_model_id, "kobox hosted x86-64 execution domain");
	memset(cpu->x86_capability, 0, sizeof(cpu->x86_capability));
	cpu->x86_capability[CPUID_1_EDX] = required;
	set_cpu_cap(cpu, X86_FEATURE_CPUID);
	set_cpu_cap(cpu, X86_FEATURE_LM);
	set_cpu_cap(cpu, X86_FEATURE_NX);
	set_cpu_cap(cpu, X86_FEATURE_NOPL);
	set_cpu_cap(cpu, X86_FEATURE_ALWAYS);
	cpu->initialized = true;
}

void __init arch_cpu_finalize_init(void)
{
	if (raw_smp_processor_id() || num_online_cpus() != 1)
		panic("hosted CPU finalization outside boot CPU");
	hosted_cpu_features();
	cpu_smt_set_num_threads(1, 1);
	fpu__init_system();
	*this_cpu_ptr(&cpu_info) = boot_cpu_data;
	alternative_instructions();
	USER_PTR_MAX = TASK_SIZE_MAX;
	runtime_const_init(ptr, USER_PTR_MAX);
}

void __init_or_module text_poke_early(void *address, const void *opcode,
				     size_t length)
{
	unsigned long flags;
	void *alias;

	/* Native callers own an unpublished target. Core ELF text has a RAM
	 * alias; newly allocated module text is already writable and NX.
	 */
	if (system_state >= SYSTEM_FREEING_INITMEM) {
		struct vm_struct *area = find_vm_area(address);
		unsigned long start = (unsigned long)address, page;

		if (!area || !(area->flags & VM_ALLOC) || !length ||
		    start < (unsigned long)area->addr ||
		    start - (unsigned long)area->addr >= get_vm_area_size(area) ||
		    length > get_vm_area_size(area) - (start - (unsigned long)area->addr))
			panic("invalid unpublished module text mapping");
		for (page = start & PAGE_MASK; page < start + length; page += PAGE_SIZE) {
			unsigned int level;
			pte_t *pte = lookup_address(page, &level);

			if (!pte || level != PG_LEVEL_4K || !pte_present(*pte) ||
			    !pte_write(*pte) || pte_exec(*pte))
				panic("early module text is not RW/NX");
		}
		alias = address;
	} else {
		alias = kobox_linux_boot_text_alias(address, length);
	}
	local_irq_save(flags);
	memcpy(alias, opcode, length);
	sync_core();
	local_irq_restore(flags);
}
