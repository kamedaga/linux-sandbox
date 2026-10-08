/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_TASK_STACK_H
#define KOBOX_TASK_STACK_H

/* Linux retains its bookkeeping stack, but hosted tasks execute on their
 * native thread stacks. DMA callers must bounce either kind of stack. */
#define object_is_on_stack kobox_linux_object_is_on_stack
#include_next <linux/sched/task_stack.h>
#undef object_is_on_stack

int kobox_task_object_is_on_stack(const void *object);
static inline int object_is_on_stack(const void *object)
{
	return kobox_linux_object_is_on_stack(object) ||
		kobox_task_object_is_on_stack(object);
}
#endif
