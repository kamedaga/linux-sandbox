/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_FPU_H
#define KOBOX_TASK_FPU_H

#include "user.h"

struct task_struct;
int kobox_user_fp_clone_import(struct task_struct *task,
			       const struct kobox_x86_fp_state *state);
int kobox_user_fp_import(const struct kobox_x86_fp_state *state);
void kobox_user_fp_export(struct kobox_x86_fp_state *state);

#endif
