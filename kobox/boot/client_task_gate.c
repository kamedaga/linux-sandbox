// SPDX-License-Identifier: GPL-2.0-only

#include "client_task_gate.h"
#include "diagnostic.h"
#include "exception.h"

#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>

#include <asm/mmu_context.h>

int kobox_linux_uaccess_probe(unsigned int *line);

#define CHECK(condition) do { \
	if (!(condition)) { \
		if (!state->report->line) \
			state->report->line = __LINE__; \
		return -EINVAL; \
	} \
} while (0)

struct client_task_case {
	struct kobox_client_task_report *report;
	struct mm_struct *mm;
	struct files_struct *files;
	struct file *file;
	const struct cred *cred;
	struct task_struct *child;
	struct completion done;
	unsigned long address;
	unsigned int flags;
	int fd, result;
};

static int inspect_child(struct client_task_case *state)
{
	struct kobox_client_task_report *report = state->report;
	struct cred *credential;
	struct file *file;
	struct page *page;
	unsigned int cpu;
	long count;
	int duplicate;
	bool same;

	CHECK(!(current->flags & PF_KTHREAD));
	CHECK(current->mm && current->active_mm == current->mm);
	CHECK((current->mm == state->mm) == !!(state->flags & CLONE_VM));
	CHECK((current->files == state->files) == !!(state->flags & CLONE_FILES));
	CHECK(uid_eq(current_euid(), state->cred->euid));
	CHECK(current_cred() == current_real_cred());
	CHECK(current_cred() != state->cred);
	CHECK(!current_pt_regs()->ip && !current_pt_regs()->sp);
	report->tasks++;
	for_each_online_cpu(cpu) {
		CHECK(!set_cpus_allowed_ptr(current, cpumask_of(cpu)));
		preempt_disable();
		same = raw_smp_processor_id() == cpu &&
			current == this_cpu_read(current_task) &&
			current_top_of_stack() == task_top_of_stack(current) &&
			current->mm == this_cpu_read(cpu_tlbstate.loaded_mm);
		preempt_enable();
		CHECK(same);
		report->switches++;
	}
	/* This is Linux's page lookup/fault path in the child's actual mm,
	 * including the VMA duplicated by copy_mm in the non-CLONE_VM case.
	 */
	mmap_read_lock(current->mm);
	count = get_user_pages(state->address, 1, 0, &page);
	mmap_read_unlock(current->mm);
	CHECK(count == 1);
	same = *(u64 *)page_address(page) == 0x123456789abcdef0ULL;
	put_page(page);
	CHECK(same);
	report->mappings++;
	count = kobox_linux_uaccess_probe(&report->line);
	CHECK(!count);
	file = fget(state->fd);
	CHECK(file);
	same = file == state->file && close_on_exec(state->fd, current->files);
	duplicate = f_dupfd(0, file, 0);
	fput(file);
	CHECK(same && duplicate >= 0 && duplicate != state->fd);
	CHECK(!close_on_exec(duplicate, current->files));
	CHECK(!close_fd(state->fd));
	file = fget(duplicate);
	CHECK(file);
	same = file == state->file;
	fput(file);
	CHECK(same);
	CHECK(!close_fd(duplicate));
	CHECK(close_fd(duplicate) == -EBADF);
	report->files++;
	credential = prepare_creds();
	CHECK(credential);
	/* Exercise upstream credential copy-on-write, not a host identity
	 * policy. This changes only this test child's effective identity.
	 */
	credential->euid = make_kuid(current_user_ns(), 1000);
	credential->fsuid = credential->euid;
	commit_creds(credential);
	CHECK(!uid_eq(current_euid(), state->cred->euid));
	report->credentials++;
	return 0;
}

static int child_entry(void *argument)
{
	struct client_task_case *state = argument;

	get_task_struct(current);
	state->child = current;
	state->result = inspect_child(state);
	/* A kernel-start user task must explicitly exit or enter its native
	 * user context; returning through the kthread trampoline is invalid.
	 */
	do_exit(state->result ? 1 << 8 : 0);
}

static int clone_case(struct client_task_case *state)
{
	struct kernel_clone_args arguments = {
		.flags = state->flags,
		.exit_signal = SIGCHLD,
		.fn = child_entry,
		.fn_arg = state,
	};
	struct file *file;
	unsigned long deadline;
	pid_t pid, waited;
	int status = 0, inactive = 0;
	bool same;

	state->files = current->files;
	state->cred = current_cred();
	state->child = NULL;
	state->fd = get_unused_fd_flags(O_CLOEXEC);
	CHECK(state->fd >= 0);
	fd_install(state->fd, get_file(state->file));
	pid = kernel_clone(&arguments);
	if (pid < 0) {
		close_fd(state->fd);
		return pid;
	}
	waited = kernel_wait(pid, &status);
	CHECK(waited == pid && state->child);
	/* wait4 reaps after exit_notify, before the child's final schedule.
	 * Keep our task reference until the real switch-out has completed.
	 */
	deadline = jiffies + 5 * HZ;
	while (!wait_task_inactive(state->child, TASK_DEAD)) {
		if (time_after_eq(jiffies, deadline)) {
			inactive = -ETIMEDOUT;
			break;
		}
		schedule_timeout_uninterruptible(1);
	}
	put_task_struct(state->child);
	state->child = NULL;
	file = fget(state->fd);
	same = (file == NULL) == !!(state->flags & CLONE_FILES);
	if (file) {
		fput(file);
		close_fd(state->fd);
	}
	CHECK(!inactive && !status && !state->result && same);
	CHECK(current_cred() == state->cred);
	CHECK(uid_eq(current_euid(), GLOBAL_ROOT_UID));
	CHECK(atomic_read(&state->mm->mm_users) == 1);
	/* Only the manager's file reference and its original VMA remain. */
	CHECK(file_count(state->file) == 2);
	state->report->reaped++;
	return 0;
}

static int manager(void *argument)
{
	struct client_task_case *state = argument;
	struct page *page = NULL;
	unsigned int index;
	int result, unmapped;

	result = unshare_files();
	if (result)
		goto done;
	/* kthreadd ignores SIGCHLD. Use normal waitable-child semantics in
	 * this manager's own sighand, through the upstream signal API.
	 */
	kernel_sigaction(SIGCHLD, SIG_DFL);
	state->mm = mm_alloc();
	if (!state->mm) {
		result = -ENOMEM;
		goto done;
	}
	state->mm->task_size = TASK_SIZE;
	arch_pick_mmap_layout(state->mm, &current->signal->rlim[RLIMIT_STACK]);
	state->file = shmem_file_setup("client-task-pages", PAGE_SIZE, 0);
	if (IS_ERR(state->file)) {
		result = PTR_ERR(state->file);
		goto drop_mm;
	}
	page = shmem_read_mapping_page(state->file->f_mapping, 0);
	if (IS_ERR(page)) {
		result = PTR_ERR(page);
		page = NULL;
		goto drop_file;
	}
	*(u64 *)page_address(page) = 0x123456789abcdef0ULL;
	set_page_dirty(page);
	/* Retain one observer reference through all child exits and teardown. */
	kthread_use_mm(state->mm);
	state->address = vm_mmap(state->file, 0, PAGE_SIZE,
				 PROT_READ | PROT_WRITE, MAP_SHARED, 0);
	if (IS_ERR_VALUE(state->address)) {
		result = (long)state->address;
		goto unuse;
	}
	for (index = 0; index < 16; index++) {
		state->report->phase = index + 1;
		state->flags = (index & 1 ? CLONE_VM : 0) |
			(index & 2 ? CLONE_FILES : 0);
		result = clone_case(state);
		if (result)
			break;
	}
	unmapped = vm_munmap(state->address, PAGE_SIZE);
	if (unmapped && !result) {
		kobox_linux_boot_diagnostic(
			"kobox client task: munmap result=%d\n", unmapped);
		result = -EINVAL;
	}
unuse:
	kthread_unuse_mm(state->mm);
drop_file:
	__fput_sync(state->file);
drop_mm:
	mmput(state->mm);
	if (page) {
		int before = page_ref_count(page);
		int references;
		struct address_space *mapping;

		/* Reaped tasks and an evicted mapping do not drain per-CPU LRU
		 * batches. Their temporary references must retire before testing
		 * for the sole observer, as in the shmem and VM lifetime Gates.
		 */
		lru_add_drain_all();
		references = page_ref_count(page);
		mapping = folio_mapping(page_folio(page));
		if (before != references)
			kobox_linux_boot_diagnostic(
				"kobox client task: LRU drain references=%d -> %d\n",
				before, references);

		if ((references != 1 || mapping) && !result) {
			kobox_linux_boot_diagnostic(
				"kobox client task: observer references=%d mapped=%u\n",
				references, !!mapping);
			result = -EINVAL;
		}
		put_page(page);
	}
done:
	state->result = result;
	complete(&state->done);
	/* kthread_stop synchronizes the manager's final release as well. */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return result;
}

__attribute__((visibility("default")))
int kobox_linux_client_task_verify(struct kobox_client_task_report *report)
{
	struct client_task_case state = {.report = report};
	struct task_struct *task;
	int result;

	if (!IS_ENABLED(CONFIG_MULTIUSER) || !report || report->size != sizeof(*report) ||
	    num_online_cpus() != 2 || current->mm)
		return -EINVAL;
	init_completion(&state.done);
	task = kthread_run(manager, &state, "client-task-gate");
	if (IS_ERR(task))
		return PTR_ERR(task);
	wait_for_completion(&state.done);
	result = kthread_stop(task);
	rcu_barrier();
	flush_delayed_fput();
	report->warnings = kobox_linux_exception_warnings();
	if (!result && (report->warnings || report->tasks != 16 ||
	    report->switches != 32 || report->mappings != 16 ||
	    report->credentials != 16 || report->files != 16 || report->reaped != 16)) {
		kobox_linux_boot_diagnostic(
			"kobox client task: totals warnings=%llu tasks=%u switches=%u mappings=%u credentials=%u files=%u reaped=%u\n",
			report->warnings, report->tasks, report->switches,
			report->mappings, report->credentials, report->files, report->reaped);
		result = -EINVAL;
	}
	report->result = result;
	return result;
}
