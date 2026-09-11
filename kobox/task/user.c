// SPDX-License-Identifier: GPL-2.0-only

#include "user.h"
#include "../arch/x86_64/host_call.h"
#include "../arch/x86_64/fpu.h"
#include "../arch/x86_64/registers.h"
#include "../mm/port.h"

#include <linux/entry-common.h>
#include <linux/export.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>


struct kobox_user_context {
	struct kobox_vm_space *space;
	void *host;
	u64 sequence, syscall_sequence;
};

struct user_call {
	struct kobox_vm_space *space;
	void *host;
	const struct kobox_linux_vm_event *event;
};

/* Native pthread-local machine entry, not a Linux current/task replacement. */
static __thread struct user_call *active_call;

int kobox_user_syscall(struct kobox_vm_space *space, void *host_context,
		       const struct kobox_linux_vm_event *event,
		       struct kobox_x86_user_regs *output)
{
	struct user_call call = {.space = space, .host = host_context, .event = event};
	struct kobox_x86_user_regs stopped;
	struct kobox_x86_fp_state fp;
	int result;

	if (!space || !host_context || current->mm != space->mm || current->active_mm != space->mm ||
	    current->flags & PF_KTHREAD || irqs_disabled() || in_atomic() ||
	    !event || event->kind != KOBOX_VM_EVENT_SYSCALL || !output || active_call ||
	    !space->operations->snapshot || !space->operations->write_fpregs)
		return -EINVAL;
	if (!kobox_x86_user_mode(&event->user))
		return -EINVAL;
	result = kobox_host_call(space->operations->snapshot(host_context, event->sequence, &stopped, &fp));
	if (result)
		return result;
	kobox_x86_user_import(&event->user);
	kobox_x86_user_syscall_prepare();
	result = kobox_user_fp_import(&fp);
	if (result)
		return result;
	active_call = &call;
	kobox_x86_user_syscall_dispatch();
	/*
	 * Remote user execution is host scheduled. This Linux task is in the
	 * kernel while servicing requests, not in a fabricated CPU user EQS.
	 */
	kobox_x86_user_return_work();
	kobox_user_fp_export(&fp);
	local_irq_enable();
	active_call = NULL;
	kobox_x86_user_export(output);
	return kobox_host_call(space->operations->write_fpregs(host_context, event->sequence, &fp));
}

struct kobox_user_context *kobox_user_clone(struct task_struct *task,
	const struct kernel_clone_args *arguments)
{
	struct kobox_user_context *context;
	struct kobox_x86_fp_state fp;
	struct kobox_vm_space *parent;
	void *host_child = NULL;
	bool share_mm = arguments->flags & CLONE_VM;
	int result;

	if (!active_call || !task->mm || arguments->fn)
		return ERR_PTR(-EINVAL);
	parent = active_call->space;
	if (!parent->operations->clone)
		return ERR_PTR(-EOPNOTSUPP);
	if (share_mm && task->mm != parent->mm)
		return ERR_PTR(-EINVAL);
	if (arguments->flags & CLONE_SETTLS) {
		result = kobox_x86_user_set_tls(task, arguments->tls);
		if (result)
			return ERR_PTR(result);
	}
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return ERR_PTR(-ENOMEM);
	result = kobox_host_call(parent->operations->clone(active_call->host, active_call->event->sequence,
		active_call->event->syscall.sequence, share_mm, &host_child, &fp));
	if (result)
		goto free;
	/* Use the upstream regset validator and FP state ownership. The source
	 * is the actual external child, not the runtime pthread's registers.
	 */
	result = kobox_user_fp_clone_import(task, &fp);
	if (result)
		goto close;
	if (share_mm) {
		result = kobox_vm_space_share(parent, host_child);
		if (result)
			goto close;
		context->space = parent;
	} else {
		context->space = kobox_vm_space_bind(task->mm, host_child, parent->operations,
						     parent->start, parent->end - parent->start);
		if (IS_ERR(context->space)) {
			result = PTR_ERR(context->space);
			goto close;
		}
	}
	context->host = host_child;
	context->syscall_sequence = active_call->event->syscall.sequence;
	return context;
close:
	if (kobox_host_call(parent->operations->close(host_child)))
		panic("native fork rollback could not reap child");
free:
	kfree(context);
	return ERR_PTR(result);
}

void kobox_user_release(struct kobox_user_context *context, struct task_struct *task)
{
	int result;

	if (!context)
		return;
	if (READ_ONCE(task->__state) == TASK_NEW)
		result = kobox_vm_space_cancel(context->space, context->host, task);
	else
		result = kobox_vm_space_exit(context->space, context->host, task);
	if (result)
		panic("native task release could not detach mm: %d", result);
	kfree(context);
}

static int return_user(struct kobox_user_context *context,
		       const struct kobox_linux_vm_event *event,
		       const struct kobox_x86_user_regs *output)
{
	struct kobox_vm_space *old = context->space, *next;
	struct kobox_x86_fp_state fp;
	void *host = NULL;
	int result;

	if (current->mm != old->mm) {
		if (!old->operations->clone || !old->operations->write_fpregs)
			return -EOPNOTSUPP;
		/* Linux has committed exec, including files and credentials. A
		 * fresh native MM keeps old CLONE_VM peers intact; cloning here
		 * creates no Linux task and changes no Linux process identity.
		 */
		result = kobox_host_call(old->operations->clone(context->host, event->sequence,
			event->syscall.sequence, false, &host, &fp));
		if (result)
			return result;
		next = kobox_vm_space_bind(current->mm, host, old->operations,
					  old->start, old->end - old->start);
		if (IS_ERR(next)) {
			result = PTR_ERR(next);
			if (kobox_host_call(old->operations->close(host)))
				panic("native exec rollback could not reap context");
			return result;
		}
		/* No syscall return may run with the inherited pre-exec FP bank.
		 * Its replacement is Linux's flush_thread()/start_thread state.
		 */
		kobox_user_fp_export(&fp);
		result = kobox_host_call(old->operations->write_fpregs(host, 0, &fp));
		/* Even on FP-restore failure, publish ownership of next before
		 * fatal exit so exit_thread releases its binding and native MM.
		 */
		if (kobox_vm_space_replaced(old, context->host))
			panic("native exec could not retire old context");
		context->space = next;
		context->host = host;
		context->sequence = 0;
		if (result)
			return result;
	}
	return kobox_host_call(context->space->operations->syscall_return(context->host,
		context->sequence, event->syscall.sequence, output));
}

static int interrupted_user(struct kobox_user_context *context,
			    const struct kobox_linux_vm_fault *fault)
{
	struct kobox_vm_space *space = context->space;
	struct kobox_x86_user_regs registers;
	struct kobox_x86_fp_state fp;
	int result;

	result = kobox_host_call(space->operations->snapshot(context->host, context->sequence, &registers, &fp));
	if (result)
		return result;
	/* This is an instruction boundary, not a suspended guest syscall. */
	registers.orig_ax = -1UL;
	kobox_x86_user_import(&registers);
	result = kobox_user_fp_import(&fp);
	if (result)
		return result;
	if (fault) {
		kobox_x86_user_fault(fault->address, fault->error);
		force_sig_fault(fault->signal, fault->signal_code,
				(void __user *)fault->address);
	}
	kobox_x86_user_return_work();
	kobox_user_fp_export(&fp);
	local_irq_enable();
	kobox_x86_user_export(&registers);
	return kobox_host_call(space->operations->restore(context->host, context->sequence, &registers, &fp));
}

static void __noreturn user_run(struct kobox_user_context *context, int result)
{
	struct kobox_vm_space *space;
	struct kobox_linux_vm_event event;
	struct kobox_x86_user_regs output;
	int waited;
	for (;;) {
		space = context->space;
		if (!result)
			result = kobox_host_call(space->operations->resume(context->host, context->sequence));
		if (result) {
			pr_err("user machine return: pid=%d result=%d sequence=%llu syscall=%lu ip=%lx\n",
			       current->pid, result, context->sequence,
			       kobox_x86_user_syscall_number(), kobox_x86_user_ip());
			do_group_exit(SIGKILL);
		}
		/* Resume advances the host generation even before an event exists.
		 * An asynchronous stop must address that running generation.
		 */
		context->sequence++;
		waited = wait_event_interruptible(space->events,
			(result = kobox_host_call(space->operations->event(context->host, &event))) != -EAGAIN);
		if (waited) {
			result = interrupted_user(context, NULL);
			if (result != -EBUSY)
				continue;
			/* A real syscall/fault won the stop race. Consume that exact
			 * event before signal delivery; never overwrite its frame.
			 */
			wait_event(space->events,
				(result = kobox_host_call(space->operations->event(context->host, &event))) != -EAGAIN);
		}
		if (result || event.error || event.kind == KOBOX_VM_EVENT_EXIT) {
			pr_err("user machine event: pid=%d result=%d error=%d kind=%d\n",
			       current->pid, result, event.error, event.kind);
			do_group_exit(SIGKILL);
		}
		context->sequence = event.sequence;
		if (event.kind == KOBOX_VM_EVENT_SYSCALL) {
			result = kobox_user_syscall(space, context->host, &event, &output);
			if (!result)
				result = return_user(context, &event, &output);
		} else if (event.kind == KOBOX_VM_EVENT_FAULT) {
			result = kobox_vm_resolve_fault(space, &event.fault);
			if (result == -EAGAIN)
				result = 0;
			if (!result && event.fault.signal) {
				result = interrupted_user(context, &event.fault);
			}
		} else {
			/* An unsolicited native trap is not a fixture completion. */
			do_group_exit(SIGTRAP);
		}
	}
}

void __noreturn kobox_user_enter(struct kobox_user_context *context)
{
	struct kobox_x86_user_regs output;
	struct kobox_x86_fp_state fp;
	int result;

	/* schedule_tail already performed upstream child-TID publication. */
	kobox_x86_user_return_work();
	kobox_user_fp_export(&fp);
	local_irq_enable();
	kobox_x86_user_export(&output);
	result = kobox_host_call(context->space->operations->write_fpregs(
		context->host, context->sequence, &fp));
	if (!result)
		result = kobox_host_call(context->space->operations->syscall_return(context->host,
			context->sequence, context->syscall_sequence, &output));
	user_run(context, result);
}

int kobox_user_adopt(struct kobox_vm_space *space, void *host_context,
		     const struct kobox_linux_vm_event *event)
{
	struct kobox_user_context *context;
	struct kobox_x86_user_regs output;
	int result;

	if (!space || !host_context || current->mm != space->mm ||
	    current->active_mm != space->mm || current->flags & PF_KTHREAD ||
	    irqs_disabled() || in_atomic() || active_call || !event ||
	    event->error || event->kind != KOBOX_VM_EVENT_SYSCALL ||
	    !kobox_x86_user_mode(&event->user) ||
	    !space->operations->syscall_return || !space->operations->resume ||
	    !space->operations->event)
		return -EINVAL;
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	context->space = space;
	context->host = host_context;
	context->sequence = event->sequence;
	context->syscall_sequence = event->syscall.sequence;
	result = kobox_task_user_attach(context);
	if (result) {
		kfree(context);
		return result;
	}
	/* The first instruction may itself exit. Publish task-port ownership
	 * before dispatch so upstream exit_thread always reaps the native task.
	 */
	result = kobox_user_syscall(space, host_context, event, &output);
	if (!result)
		result = return_user(context, event, &output);
	user_run(context, result);
}

int kobox_user_start(struct kobox_vm_space *space, void *host_context)
{
	struct kobox_user_context *context;
	struct kobox_x86_user_regs output;
	struct kobox_x86_fp_state fp;
	int result;

	if (!space || !host_context || current->mm != space->mm ||
	    current->active_mm != space->mm || current->flags & PF_KTHREAD ||
	    irqs_disabled() || in_atomic() || active_call ||
	    !space->operations->start || !space->operations->enable_syscalls)
		return -EINVAL;
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	context->space = space;
	context->host = host_context;
	result = kobox_task_user_attach(context);
	if (result) {
		kfree(context);
		return result;
	}
	kobox_x86_user_return_work();
	kobox_user_fp_export(&fp);
	local_irq_enable();
	kobox_x86_user_export(&output);
	result = kobox_host_call(space->operations->enable_syscalls(host_context));
	if (!result)
		result = kobox_host_call(space->operations->start(host_context, &output, &fp));
	user_run(context, result);
}
