// SPDX-License-Identifier: GPL-2.0-only

#include "../../boot/exception.h"

#include <linux/irq-entry-common.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/string.h>

#include <asm/extable.h>
#include <asm/ptrace.h>
#include <asm/trapnr.h>
#include <asm/text-patching.h>

/* Only visibility changes: the decoder, WARN/BUG report and IP adjustment
 * remain the pinned arch/x86/kernel/traps.c implementation.
 */
bool handle_bug(struct pt_regs *regs);
bool do_int3(struct pt_regs *regs);

static const size_t register_offset[KOBOX_EXCEPTION_REGISTERS] = {
	[KOBOX_EXCEPTION_AX] = offsetof(struct pt_regs, ax),
	[KOBOX_EXCEPTION_BX] = offsetof(struct pt_regs, bx),
	[KOBOX_EXCEPTION_CX] = offsetof(struct pt_regs, cx),
	[KOBOX_EXCEPTION_DX] = offsetof(struct pt_regs, dx),
	[KOBOX_EXCEPTION_SI] = offsetof(struct pt_regs, si),
	[KOBOX_EXCEPTION_DI] = offsetof(struct pt_regs, di),
	[KOBOX_EXCEPTION_BP] = offsetof(struct pt_regs, bp),
	[KOBOX_EXCEPTION_R8] = offsetof(struct pt_regs, r8),
	[KOBOX_EXCEPTION_R9] = offsetof(struct pt_regs, r9),
	[KOBOX_EXCEPTION_R10] = offsetof(struct pt_regs, r10),
	[KOBOX_EXCEPTION_R11] = offsetof(struct pt_regs, r11),
	[KOBOX_EXCEPTION_R12] = offsetof(struct pt_regs, r12),
	[KOBOX_EXCEPTION_R13] = offsetof(struct pt_regs, r13),
	[KOBOX_EXCEPTION_R14] = offsetof(struct pt_regs, r14),
	[KOBOX_EXCEPTION_R15] = offsetof(struct pt_regs, r15),
};
static unsigned long exception_warnings;
static struct kobox_linux_warning warnings[16];
static bool warning_ready[ARRAY_SIZE(warnings)];

unsigned long kobox_linux_exception_warnings(void)
{
	return READ_ONCE(exception_warnings);
}

int kobox_linux_exception_warning(unsigned int index, struct kobox_linux_warning *warning)
{
	if (!warning || index >= ARRAY_SIZE(warnings))
		return -EINVAL;
	if (!smp_load_acquire(&warning_ready[index]))
		return -EAGAIN;
	*warning = warnings[index];
	return 0;
}

static void record_warning(unsigned long address, unsigned int cpu)
{
	unsigned long index = __atomic_fetch_add(&exception_warnings, 1, __ATOMIC_RELAXED);
	struct bug_entry *bug = find_bug(address);
	const char *file = "unknown";

	if (index >= ARRAY_SIZE(warnings))
		return;
	warnings[index].cpu = cpu;
#ifdef CONFIG_DEBUG_BUGVERBOSE
	if (bug) {
#ifdef CONFIG_GENERIC_BUG_RELATIVE_POINTERS
		file = (const char *)&bug->file_disp + bug->file_disp;
#else
		file = bug->file;
#endif
		warnings[index].line = bug->line;
	}
#endif
	/* Copy before module removal can release its bug table and strings. */
	strscpy(warnings[index].file, file, sizeof(warnings[index].file));
	smp_store_release(&warning_ready[index], true);
}

enum kobox_linux_exception_result kobox_linux_exception_dispatch(
	struct kobox_linux_exception_frame *frame)
{
	struct pt_regs regs = {.cs = __KERNEL_CS, .ss = __KERNEL_DS,
			       .orig_ax = ~0UL};
	irqentry_state_t state = {0};
	unsigned long flags;
	unsigned int index;
	bool handled;
	bool early;

	if (!frame || frame->size != sizeof(*frame) || !current ||
	    frame->cpu != raw_smp_processor_id() ||
	    current != raw_cpu_read(current_task) ||
	    !kernel_text_address(frame->ip))
		return KOBOX_EXCEPTION_FATAL;
	for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
		*(unsigned long *)((char *)&regs + register_offset[index]) =
			frame->registers[index];
	regs.ip = frame->ip;
	regs.sp = frame->sp;
	regs.flags = frame->flags;
	local_irq_save(flags);
	if (flags)
		regs.flags &= ~X86_EFLAGS_IF;
	else
		regs.flags |= X86_EFLAGS_IF;
	/* Match the native early exception path before RCU/service startup. */
	early = system_state == SYSTEM_BOOTING;
	if (frame->vector == X86_TRAP_UD) {
		handled = handle_bug(&regs);
		if (handled)
			record_warning(frame->ip, frame->cpu);
	} else if (frame->vector == X86_TRAP_BP) {
		handled = smp_text_poke_int3_handler(&regs);
		if (!handled) {
			state = irqentry_nmi_enter(&regs);
			handled = do_int3(&regs);
			irqentry_nmi_exit(&regs, state);
		}
	} else {
		if (!early)
			state = irqentry_enter(&regs);
		handled = fixup_exception(&regs, frame->vector,
					  frame->error_code, frame->fault_address);
		if (!early)
			irqentry_exit(&regs, state);
	}
	if (handled) {
		for (index = 0; index < KOBOX_EXCEPTION_REGISTERS; index++)
			frame->registers[index] = *(unsigned long *)
				((char *)&regs + register_offset[index]);
		frame->ip = regs.ip;
		frame->sp = regs.sp;
		frame->flags = regs.flags;
	}
	local_irq_restore(flags);
	return handled ? KOBOX_EXCEPTION_RESUME : KOBOX_EXCEPTION_FATAL;
}
