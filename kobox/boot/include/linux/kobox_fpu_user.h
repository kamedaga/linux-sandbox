/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_FPU_USER_H
#define KOBOX_FPU_USER_H

/* Included after the upstream legacy instruction helpers. Only the operand
 * translation changes: signal-frame layout, retries and rejection stay in
 * upstream fpu/signal.c. The fixed machine exposes legacy FP/SSE only.
 */
static inline int kobox_fxsave_user(struct fxregs_state __user *user)
{
	struct fxregs_state state __aligned(16) = {};

	fxsave(&state);
	return __copy_to_user(user, &state, sizeof(state)) ? X86_TRAP_PF : 0;
}

static inline int kobox_fxrstor_user(struct fxregs_state __user *user)
{
	struct fxregs_state state __aligned(16);

	if (__copy_from_user(&state, user, sizeof(state)))
		return X86_TRAP_PF;
	return fxrstor_safe(&state) ? X86_TRAP_GP : 0;
}

#define fxsave_to_user_sigframe kobox_fxsave_user
#define fxrstor_from_user_sigframe kobox_fxrstor_user

#endif
