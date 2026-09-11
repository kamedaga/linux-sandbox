// SPDX-License-Identifier: GPL-2.0-only

#include "fpu.h"

#include <linux/sched.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/regset.h>
#include "../../../arch/x86/kernel/fpu/internal.h"
#include "../../../arch/x86/kernel/fpu/legacy.h"
#include "../../../arch/x86/kernel/fpu/context.h"

/* External user execution is a separate physical FP register bank. Import
 * it through the real instruction validator, then leave Linux's lazy-load
 * state authoritative while the runtime calls its native host services.
 */
int kobox_user_fp_clone_import(struct task_struct *task,
			       const struct kobox_x86_fp_state *state)
{
	BUILD_BUG_ON(sizeof(*state) != sizeof(struct fxregs_state));
	return xfpregs_set(task, NULL, 0, sizeof(*state), state, NULL);
}

int kobox_user_fp_import(const struct kobox_x86_fp_state *state)
{
	struct fxregs_state native __aligned(16);
	struct fpu *fpu = x86_task_fpu(current);
	int result;

	BUILD_BUG_ON(sizeof(native) != sizeof(*state));
	memcpy(&native, state, sizeof(native));
	fpregs_lock();
	result = fxrstor_safe(&native);
	if (!result) {
		fpregs_mark_activate();
		save_fpregs_to_fpstate(fpu);
	}
	set_thread_flag(TIF_NEED_FPU_LOAD);
	__cpu_invalidate_fpregs_state();
	__fpu_invalidate_fpregs_state(fpu);
	fpregs_unlock();
	return result;
}

void kobox_user_fp_export(struct kobox_x86_fp_state *state)
{
	struct fpu *fpu = x86_task_fpu(current);

	fpregs_lock();
	if (!test_thread_flag(TIF_NEED_FPU_LOAD))
		save_fpregs_to_fpstate(fpu);
	memcpy(state, &fpu->fpstate->regs.fxsave, sizeof(*state));
	set_thread_flag(TIF_NEED_FPU_LOAD);
	__cpu_invalidate_fpregs_state();
	__fpu_invalidate_fpregs_state(fpu);
	fpregs_unlock();
}
