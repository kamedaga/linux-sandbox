/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_RUNTIME_HOST_H
#define KOBOX_RUNTIME_HOST_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#endif

/* Core-private state for one host thread. It is explicit so the same linked
 * core can run beside either libc TLS on POSIX or native adapter TLS on
 * PachaOS. Keep this independent of Linux task_struct: a host thread can
 * switch between Linux tasks while retaining its machine-entry state. */
struct kobox_runtime_thread_state {
	unsigned long percpu_offset;
	void *current_task;
	void *active_user_call;
	unsigned long gate_value __attribute__((aligned(64)));
	unsigned int cpu;
	unsigned int irq_disable_depth;
	unsigned char gate_initialized;
};

/* Process-local loader contract. Bind once before core execution; context
 * must outlive every core thread. thread_state is a leaf callback: it may not
 * allocate, block, enter a guest service, or use vector/FPU state. */
struct kobox_runtime_host {
	size_t size;
	void *context;
	struct kobox_runtime_thread_state *(*thread_state)(void *context);
};

int kobox_linux_runtime_bind(const struct kobox_runtime_host *host);
struct kobox_runtime_thread_state *kobox_runtime_thread_state(void);

#endif
