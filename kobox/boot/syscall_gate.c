// SPDX-License-Identifier: GPL-2.0-only

#include "syscall_gate.h"
#include "exception.h"
#include "../mm/port.h"
#include "../task/user.h"

#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/futex.h>
#include <linux/fs_struct.h>
#include <linux/kthread.h>
#include <linux/memfd.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/net.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>
#include <linux/socket.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <uapi/linux/memfd.h>
#include <uapi/linux/un.h>
#include <uapi/drm/drm.h>
#include <uapi/drm/drm_mode.h>
#include <asm/unistd.h>
#include <asm/prctl.h>

struct transfer_case {
	struct completion bound, sent;
	struct completion captured[2];
	bool race;
	atomic_t failed;
	struct files_struct *receiver_files;
	struct mm_struct *receiver_mm;
	struct file *held;
	struct page *page;
	u32 gem_handle;
	u64 gem_offset;
};

struct syscall_case {
	const struct kobox_syscall_test *host;
	struct kobox_syscall_report *report;
	struct kobox_vm_space *space;
	struct vfsmount *drm_mount;
	struct task_struct *child;
	struct task_struct *peer;
	struct file *held;
	struct page *file_page, *buffer_page;
	struct page *peer_pages[2];
	struct transfer_case *transfer;
	bool inheritance;
	bool native_fork;
	bool shared_clone;
	bool thread;
	bool autonomous, adopted;
	unsigned int group_exit;
	struct completion done;
	u64 sequence, syscall_sequence;
	unsigned int cpu;
	pid_t pid;
	int result;
};

struct inheritance_case {
	struct syscall_case *parent;
	struct syscall_case child;
	struct kobox_syscall_report report;
	struct completion child_closed, parent_closed;
	atomic_t failed;
};

static int join_case(struct syscall_case *state);
static int drm_rights_create(struct syscall_case *state);
static int drm_rights_receive(struct syscall_case *state, int fd);

#define CHECK(condition) do { \
	if (!(condition)) { \
		if (!state->report->line) \
			state->report->line = __LINE__; \
		return -EINVAL; \
	} \
} while (0)

static int next_event(struct syscall_case *state, struct kobox_linux_vm_event *event)
{
	struct kobox_vm_space *space = state->space;
	int result;
	long waited;

	for (;;) {
		waited = wait_event_timeout(space->events,
			(result = space->operations->event(space->host_space, event)) != -EAGAIN,
			5 * HZ);
		if (!waited)
			return -ETIMEDOUT;
		if (result || event->error)
			return result ?: event->error;
		if (event->kind != KOBOX_VM_EVENT_FAULT)
			return 0;
		result = kobox_vm_resolve_fault(space, &event->fault);
		if (result)
			return result;
		state->report->faults++;
		result = space->operations->resume(space->host_space, event->sequence);
		if (result)
			return result;
	}
}

/* This diagnostic connects real captured instructions to the upstream table.
 * It is not certification of full user-register/FPU/signal return.
 * The trusted client executes outside the Linux CPU domain; do not pretend
 * that the waiting service task is executing userspace on that logical CPU.
 */
static long invoke(struct syscall_case *state, u64 number,
		   u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct kobox_vm_space *space = state->space;
	const u64 arguments[6] = {a0, a1, a2, a3, a4, a5};
	struct kobox_linux_vm_event event;
	struct kobox_x86_user_regs returned;
	long value;
	int result;

	state->report->number = number;
	CHECK(current->mm == space->mm && !(current->flags & PF_KTHREAD));
	if (state->shared_clone && number == __NR_clone)
		result = state->host->vm->probe(space->host_space, state->host->vm->start,
					      state->thread ? 10 : 9, a0, state->sequence);
	else if (state->native_fork && number == __NR_fork)
		result = state->host->vm->probe(space->host_space, state->host->vm->start,
					      7, 0, state->sequence);
	else
		result = state->host->issue(space->host_space, number, arguments, state->sequence);
	CHECK(!result);
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	/* The x86 syscall number is a signed int; ptrace may sign extend it. */
	CHECK(event.kind == KOBOX_VM_EVENT_SYSCALL && (int)event.syscall.number == (int)number);
	CHECK(!memcmp(event.syscall.arguments, arguments, sizeof(arguments)));
	state->syscall_sequence = event.syscall.sequence;
	if (state->transfer && state->transfer->race &&
	    ((!state->cpu && number == __NR_sendmsg) ||
	     (state->cpu && number == __NR_exit_group))) {
		/* Both native instructions are stopped before either Linux syscall
		 * starts. Release the two CPU domains to race send against exit.
		 */
		complete(&state->transfer->captured[state->cpu]);
		CHECK(wait_for_completion_timeout(
			&state->transfer->captured[state->cpu ^ 1], 5 * HZ));
		CHECK(!atomic_read(&state->transfer->failed));
		state->report->rendezvous++;
	}
	CHECK(!kobox_user_syscall(space, space->host_space, &event, &returned));
	value = returned.ax;
	state->report->returned = value;
	result = space->operations->syscall_return(space->host_space,
			state->sequence, event.syscall.sequence, &returned);
	CHECK(!result);
	CHECK(!space->operations->resume(space->host_space, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && (long)event.value == value);
	state->report->calls++;
	return value;
}

#define CALL(number, a, b, c) invoke(state, __NR_##number, a, b, c, 0, 0, 0)

static int tls_access(struct syscall_case *state, bool gs)
{
	const struct kobox_linux_vm_test *host = state->host->vm;
	unsigned long buffer = host->start + 512;
	unsigned long base = host->start + PAGE_SIZE + (gs ? 8 : 0);
	u64 sentinel = 0xfeedface00000000ULL | (state->cpu << 4) | gs;
	struct kobox_linux_vm_event event;
	u64 previous, observed;
	unsigned int get = gs ? ARCH_GET_GS : ARCH_GET_FS;
	unsigned int set = gs ? ARCH_SET_GS : ARCH_SET_FS;

	CHECK(!CALL(arch_prctl, get, buffer, 0));
	CHECK(!get_user(previous, (u64 __user *)buffer));
	CHECK(!put_user(sentinel, (u64 __user *)base));
	CHECK(!CALL(arch_prctl, set, base, 0));
	CHECK(!CALL(arch_prctl, get, buffer, 0));
	CHECK(!get_user(observed, (u64 __user *)buffer) && observed == base);
	CHECK(current == this_cpu_read(current_task));
	/* Execute a real segment-relative access in the external process. */
	CHECK(!host->probe(state->space->host_space, base, gs ? 6 : 5, 0, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && event.value == sentinel);
	CHECK(!CALL(arch_prctl, set, previous, 0));
	state->report->tls++;
	return 0;
}

static int exercise(struct syscall_case *state)
{
	unsigned long base = state->host->vm->start;
	struct iovec vectors[2] = {
		{.iov_base = (void __user *)(base + 128), .iov_len = 8},
		{.iov_base = (void __user *)(base + 136), .iov_len = 8},
	};
	static const char payload[16] = "external syscall";
	char output[16];
	struct cred *cred;
	struct kobox_linux_vm_event event;
	u64 native_value;
	long fd, duplicate, count;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	state->report->cpu_mask |= BIT(raw_smp_processor_id());
	CHECK(current == this_cpu_read(current_task));
	cred = prepare_creds();
	CHECK(cred);
	cred->euid = make_kuid(current_user_ns(), 1000 + state->cpu);
	commit_creds(cred);
	CHECK(vm_mmap(NULL, base, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, 0) == base);
	CHECK(!copy_to_user((void __user *)base, "syscall-buffer", 15));
	CHECK(!tls_access(state, false));
	CHECK(!tls_access(state, true));
	CHECK(CALL(getpid, 0, 0, 0) == task_pid_nr(current));
	CHECK(CALL(getpid, 0, 0, 0) != state->host->vm->pids[state->cpu]);
	CHECK(CALL(geteuid, 0, 0, 0) == 1000 + state->cpu);
	CHECK(invoke(state, UINT_MAX, 0, 0, 0, 0, 0, 0) == -ENOSYS);
	CHECK(CALL(memfd_create, (unsigned long)output, 0, 0) == -EFAULT);
	state->report->invalid++;
	fd = CALL(memfd_create, base, MFD_CLOEXEC, 0);
	CHECK(fd >= 0 && fd != 3);
	CHECK(CALL(fcntl, fd, F_GETFD, 0) == FD_CLOEXEC);
	CHECK(CALL(dup3, fd, 3, 0) == 3);
	CHECK(!CALL(close, fd, 0, 0));
	duplicate = CALL(dup, 3, 0, 0);
	CHECK(duplicate >= 0 && duplicate != 3);
	CHECK(!CALL(fcntl, duplicate, F_GETFD, 0));
	CHECK(!CALL(close, 3, 0, 0));
	CHECK(CALL(close, 3, 0, 0) == -EBADF);
	state->report->descriptors++;
	fd = duplicate;
	CHECK(!CALL(ftruncate, fd, PAGE_SIZE, 0));
	CHECK(!copy_to_user((void __user *)(base + 128), payload, sizeof(payload)));
	vectors[0].iov_base = (void __user *)(base + 8 * PAGE_SIZE);
	CHECK(!copy_to_user((void __user *)(base + 256), vectors, sizeof(vectors)));
	CHECK(CALL(writev, fd, base + 256, 2) == -EFAULT);
	CHECK(CALL(writev, fd, (unsigned long)vectors, 2) == -EFAULT);
	state->report->invalid += 2;
	vectors[0].iov_base = (void __user *)(base + 128);
	CHECK(!copy_to_user((void __user *)(base + 256), vectors, sizeof(vectors)));
	CHECK(CALL(writev, fd, base + 256, 2) == sizeof(payload));
	/* The real client access below must fault and publish this destination
	 * through the host RAM FD after Linux FD 3 was closed. The two number
	 * spaces must not alias, including on a newly populated Linux page.
	 */
	CHECK(invoke(state, __NR_pread64, fd, base + 2 * PAGE_SIZE,
		     sizeof(output), 0, 0, 0) == sizeof(output));
	CHECK(!copy_from_user(output, (void __user *)(base + 2 * PAGE_SIZE), sizeof(output)));
	CHECK(!memcmp(output, payload, sizeof(output)));
	memcpy(&native_value, payload, sizeof(native_value));
	CHECK(!state->host->vm->probe(state->space->host_space,
		base + 2 * PAGE_SIZE, 0, 0, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && event.value == native_value);
	state->report->nested++;
	state->held = fget(fd);
	CHECK(state->held && file_count(state->held) == 2);
	state->file_page = shmem_read_mapping_page(state->held->f_mapping, 0);
	if (IS_ERR(state->file_page)) {
		state->file_page = NULL;
		CHECK(false);
	}
	mmap_read_lock(current->mm);
	count = get_user_pages(base + 2 * PAGE_SIZE, 1, 0, &state->buffer_page);
	mmap_read_unlock(current->mm);
	CHECK(count == 1);
	state->result = 0;
	/* This instruction must exit the Linux task via upstream do_group_exit;
	 * the manager reaps it and terminates the still-stopped native context.
	 */
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

/* User buffers use pinned Linux ABI types only inside the GPL diagnostic.
 * None of these layouts or FD numbers are part of the controller protocol.
 */
static int rights_message(struct syscall_case *state, int fd, bool receive,
			  bool truncate)
{
	unsigned long base = state->host->vm->start;
	struct iovec vector = {
		.iov_base = (void __user *)(base + 128),
		.iov_len = state->drm_mount ? sizeof(u32) : 1,
	};
	struct user_msghdr message = {
		.msg_iov = (void __user *)(base + 256), .msg_iovlen = 1,
		.msg_control = (void __user *)(base + 512),
		.msg_controllen = truncate ? 0 : CMSG_SPACE(sizeof(fd)),
	};
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(fd))];
	} control = {0};

	if (!receive) {
		control.header.cmsg_len = CMSG_LEN(sizeof(fd));
		control.header.cmsg_level = SOL_SOCKET;
		control.header.cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(&control.header), &fd, sizeof(fd));
	}
	CHECK(!copy_to_user((void __user *)(base + 256), &vector, sizeof(vector)));
	CHECK(!copy_to_user((void __user *)(base + 512), &control, sizeof(control)));
	CHECK(!copy_to_user((void __user *)(base + 768), &message, sizeof(message)));
	return 0;
}

static int rights_sender(struct syscall_case *state, int socket)
{
	struct transfer_case *transfer = state->transfer;
	unsigned long base = state->host->vm->start;
	struct user_msghdr message;
	unsigned int index;
	long bytes = state->drm_mount ? sizeof(u32) : 1;
	long fd;

	CHECK(wait_for_completion_timeout(&transfer->bound, 5 * HZ));
	CHECK(!atomic_read(&transfer->failed));
	CHECK(current->files != transfer->receiver_files &&
	      current->mm != transfer->receiver_mm);
	CHECK(!CALL(connect, socket, base, sizeof(struct sockaddr_un)));
	CHECK(!copy_to_user((void __user *)(base + 128), "rights-payload", 15));
	fd = state->drm_mount ? drm_rights_create(state) :
		CALL(memfd_create, base + 128, MFD_CLOEXEC, 0);
	CHECK(fd >= 0 && fd != 17);
	CHECK(CALL(dup3, fd, 17, O_CLOEXEC) == 17);
	CHECK(!CALL(close, fd, 0, 0));
	transfer->held = fget(17);
	CHECK(transfer->held && file_count(transfer->held) == 2);
	if (!state->drm_mount) {
		CHECK(!CALL(ftruncate, 17, PAGE_SIZE, 0));
		CHECK(CALL(write, 17, base + 128, 15) == 15);
		transfer->page = shmem_read_mapping_page(transfer->held->f_mapping, 0);
		if (IS_ERR(transfer->page)) {
			transfer->page = NULL;
			CHECK(false);
		}
	}
	CHECK(!rights_message(state, 17, false, false));
	if (transfer->race) {
		long result;

		complete(&transfer->sent);
		result = CALL(sendmsg, socket, base + 768, MSG_DONTWAIT);
		CHECK(result == bytes || result == -ECONNREFUSED);
		if (result == bytes)
			state->report->race_sent++;
		else
			state->report->race_rejected++;
		CHECK(!CALL(close, 17, 0, 0));
		CHECK(!CALL(close, socket, 0, 0));
		return 0;
	}
	CHECK(CALL(sendmsg, socket, base + 8 * PAGE_SIZE, MSG_DONTWAIT) == -EFAULT);
	CHECK(!copy_from_user(&message, (void __user *)(base + 768), sizeof(message)));
	message.msg_control = (void __user *)(base + 8 * PAGE_SIZE);
	CHECK(!copy_to_user((void __user *)(base + 768), &message, sizeof(message)));
	CHECK(CALL(sendmsg, socket, base + 768, MSG_DONTWAIT) == -EFAULT);
	CHECK(!rights_message(state, -1, false, false));
	CHECK(CALL(sendmsg, socket, base + 768, MSG_DONTWAIT) == -EBADF);
	CHECK(file_count(transfer->held) == 2);
	state->report->invalid += 3;
	CHECK(!rights_message(state, 17, false, false));
	for (index = 0; index < 3; index++)
		CHECK(CALL(sendmsg, socket, base + 768, MSG_DONTWAIT) == bytes);
	CHECK(!CALL(close, 17, 0, 0));
	CHECK(!CALL(close, socket, 0, 0));
	CHECK(file_count(transfer->held) == 4); /* Observer and three queued rights. */
	state->report->nested++;
	complete(&transfer->sent);
	return 0;
}

static int rights_receiver(struct syscall_case *state, int socket)
{
	struct transfer_case *transfer = state->transfer;
	unsigned long base = state->host->vm->start;
	struct user_msghdr message;
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct kobox_linux_vm_event event;
	struct file *received;
	char payload[15];
	u64 observed;
	long bytes = state->drm_mount ? sizeof(u32) : 1;
	int fd;

	CHECK(!CALL(bind, socket, base, sizeof(struct sockaddr_un)));
	transfer->receiver_files = current->files;
	transfer->receiver_mm = current->mm;
	complete(&transfer->bound);
	CHECK(wait_for_completion_timeout(&transfer->sent, 5 * HZ));
	CHECK(!atomic_read(&transfer->failed));
	if (transfer->race)
		return 0;
	CHECK(!rights_message(state, -1, true, false));
	CHECK(CALL(recvmsg, socket, base + 768, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) == bytes);
	CHECK(!copy_from_user(&message, (void __user *)(base + 768), sizeof(message)));
	CHECK(!copy_from_user(&control, (void __user *)(base + 512), sizeof(control)));
	CHECK(!(message.msg_flags & MSG_CTRUNC) &&
	      message.msg_controllen == sizeof(control));
	CHECK(control.header.cmsg_len == CMSG_LEN(sizeof(fd)) &&
	      control.header.cmsg_level == SOL_SOCKET && control.header.cmsg_type == SCM_RIGHTS);
	memcpy(&fd, CMSG_DATA(&control.header), sizeof(fd));
	CHECK(fd >= 0 && fd != socket && fd != 17);
	CHECK(CALL(fcntl, fd, F_GETFD, 0) == FD_CLOEXEC);
	received = fget(fd);
	if (received != transfer->held) {
		if (received)
			fput(received);
		CHECK(false);
	}
	fput(received);
	if (state->drm_mount) {
		CHECK(!drm_rights_receive(state, fd));
		goto received_closed;
	}
	CHECK(CALL(lseek, fd, 0, SEEK_CUR) == 15); /* Same open file description. */
	CHECK(invoke(state, __NR_pread64, fd, base + PAGE_SIZE, sizeof(payload), 0, 0, 0) ==
	      sizeof(payload));
	CHECK(!copy_from_user(payload, (void __user *)(base + PAGE_SIZE), sizeof(payload)));
	CHECK(!memcmp(payload, "rights-payload", sizeof(payload)));
	memcpy(&observed, payload, sizeof(observed));
	CHECK(!state->host->vm->probe(state->space->host_space,
		base + PAGE_SIZE, 0, 0, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && event.value == observed);
	CHECK(!CALL(close, fd, 0, 0));
received_closed:
	CHECK(file_count(transfer->held) == 3);
	state->report->transfers++;
	state->report->nested++;
	/* A zero-sized control buffer must not install or leak a descriptor. */
	CHECK(!rights_message(state, -1, true, true));
	CHECK(CALL(recvmsg, socket, base + 768, MSG_DONTWAIT) == bytes);
	CHECK(!copy_from_user(&message, (void __user *)(base + 768), sizeof(message)));
	CHECK((message.msg_flags & MSG_CTRUNC) && !message.msg_controllen);
	CHECK(CALL(fcntl, fd, F_GETFD, 0) == -EBADF);
	CHECK(file_count(transfer->held) == 2);
	state->report->truncated++;
	/* Leave the last message queued. Upstream exit_files must release the
	 * socket, queued SCM reference and backing after the task exits.
	 */
	return 0;
}

static int exercise_rights(struct syscall_case *state)
{
	unsigned long base = state->host->vm->start;
	const struct sockaddr_un address = {
		.sun_family = AF_UNIX, .sun_path = "\0kobox-external-rights",
	};
	long socket;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	CHECK(current == this_cpu_read(current_task));
	state->report->cpu_mask = BIT(raw_smp_processor_id());
	CHECK(vm_mmap(NULL, base, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, 0) == base);
	CHECK(!copy_to_user((void __user *)base, &address, sizeof(address)));
	if (state->drm_mount) {
		struct path root = {
			.mnt = state->drm_mount,
			.dentry = state->drm_mount->mnt_root,
		};

		CHECK(!unshare_fs_struct());
		set_fs_root(current->fs, &root);
		set_fs_pwd(current->fs, &root);
		CHECK(!CALL(setresuid, 1000, 1000, 1000));
	}
	socket = CALL(socket, AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(socket >= 0);
	CHECK(!(state->cpu ? rights_receiver(state, socket) : rights_sender(state, socket)));
	state->result = 0;
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

static int probe_value(struct syscall_case *state, bool write, u64 value)
{
	struct kobox_linux_vm_event event;

	CHECK(!state->host->vm->probe(state->space->host_space,
		state->host->vm->start + PAGE_SIZE, write, value, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && event.value == value);
	return 0;
}

static int hold_private_page(struct syscall_case *state)
{
	long count;

	mmap_read_lock(current->mm);
	count = get_user_pages(state->host->vm->start + PAGE_SIZE, 1, 0, &state->buffer_page);
	mmap_read_unlock(current->mm);
	CHECK(count == 1);
	return 0;
}

static int reject_binding_reset(void *space, u64 address, size_t size)
{
	return -EIO;
}

static int inherited_child_steps(struct inheritance_case *inheritance)
{
	struct syscall_case *state = &inheritance->child;
	const struct kobox_linux_vm_test *host = state->host->vm;
	struct kobox_linux_vm_host_operations failed_operations = *host->operations;
	struct kobox_vm_space *attempt;
	struct file *inherited;
	unsigned long base = host->start;
	char payload[15];
	int users = atomic_read(&current->mm->mm_users);
	long fd;

	CHECK(current->mm != inheritance->parent->space->mm);
	CHECK(current->files != inheritance->parent->child->files);
	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	state->report->cpu_mask = BIT(raw_smp_processor_id());
	/* Neither duplicate hardware ownership nor a failed reset may consume
	 * the caller's mm reference or replace its inherited Linux mappings.
	 */
	attempt = kobox_vm_space_bind(current->mm, host->spaces[0], host->operations,
				      base, host->length);
	CHECK(PTR_ERR_OR_ZERO(attempt) == -EEXIST);
	CHECK(atomic_read(&current->mm->mm_users) == users);
	failed_operations.reset = reject_binding_reset;
	attempt = kobox_vm_space_bind(current->mm, host->spaces[1], &failed_operations,
				      base, host->length);
	CHECK(PTR_ERR_OR_ZERO(attempt) == -EIO);
	CHECK(atomic_read(&current->mm->mm_users) == users);
	state->report->binding_rollbacks += 2;
	state->space = kobox_vm_space_bind(current->mm, host->spaces[1], host->operations,
					  base, host->length);
	if (IS_ERR(state->space)) {
		state->space = NULL;
		CHECK(false);
	}
	CHECK(!host->operations->enable_syscalls(state->space->host_space));
	CHECK(CALL(geteuid, 0, 0, 0) == 1000);
	CHECK(CALL(fcntl, 17, F_GETFD, 0) == FD_CLOEXEC);
	inherited = fget(17);
	if (inherited != inheritance->parent->held) {
		if (inherited)
			fput(inherited);
		CHECK(false);
	}
	fput(inherited);
	CHECK(!probe_value(state, false, 0x1122334455667788ULL));
	CHECK(!probe_value(state, true, 0xaabbccddeeff0011ULL));
	CHECK(!hold_private_page(state));
	fd = CALL(dup, 17, 0, 0);
	CHECK(fd >= 0 && fd != 17);
	CHECK(CALL(lseek, fd, 2, SEEK_SET) == 2);
	CHECK(!CALL(close, 17, 0, 0));
	complete(&inheritance->child_closed);
	CHECK(wait_for_completion_timeout(&inheritance->parent_closed, 5 * HZ));
	CHECK(!atomic_read(&inheritance->failed));
	CHECK(invoke(state, __NR_pread64, fd, base + 128, sizeof(payload), 0, 0, 0) ==
	      sizeof(payload));
	CHECK(!copy_from_user(payload, (void __user *)(base + 128), sizeof(payload)));
	CHECK(!memcmp(payload, "inherited-data", sizeof(payload)));
	CHECK(!CALL(close, fd, 0, 0));
	state->report->inherited++;
	state->result = 0;
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

static int inherited_child_entry(void *argument)
{
	struct inheritance_case *inheritance = argument;
	struct syscall_case *state = &inheritance->child;

	get_task_struct(current);
	state->child = current;
	state->result = inherited_child_steps(inheritance);
	atomic_set(&inheritance->failed, 1);
	complete_all(&inheritance->child_closed);
	complete_all(&inheritance->parent_closed);
	do_exit(1 << 8);
}

static int inherited_parent_steps(struct syscall_case *state,
				  struct inheritance_case *inheritance)
{
	CHECK(wait_for_completion_timeout(&inheritance->child_closed, 5 * HZ));
	CHECK(!atomic_read(&inheritance->failed));
	CHECK(CALL(fcntl, 17, F_GETFD, 0) == FD_CLOEXEC);
	CHECK(CALL(lseek, 17, 0, SEEK_CUR) == 2);
	CHECK(!probe_value(state, false, 0x1122334455667788ULL));
	CHECK(!hold_private_page(state));
	CHECK(state->buffer_page != inheritance->child.buffer_page);
	state->report->cow++;
	CHECK(!CALL(close, 17, 0, 0));
	complete(&inheritance->parent_closed);
	return 0;
}

static int exercise_inheritance(struct syscall_case *state)
{
	struct inheritance_case inheritance = {
		.parent = state,
		.child = {.host = state->host, .report = &inheritance.report,
			  .cpu = 1, .result = -EINVAL},
	};
	struct kernel_clone_args arguments = {
		.exit_signal = SIGCHLD, .fn = inherited_child_entry, .fn_arg = &inheritance,
	};
	unsigned long base = state->host->vm->start;
	struct cred *cred;
	long fd;
	int result, status;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(0)));
	state->report->cpu_mask = BIT(raw_smp_processor_id());
	cred = prepare_creds();
	CHECK(cred);
	cred->euid = make_kuid(current_user_ns(), 1000);
	commit_creds(cred);
	CHECK(vm_mmap(NULL, base, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, 0) == base);
	CHECK(!copy_to_user((void __user *)base, "inherited-data", 15));
	fd = CALL(memfd_create, base, MFD_CLOEXEC, 0);
	CHECK(fd >= 0 && fd != 17);
	CHECK(CALL(dup3, fd, 17, O_CLOEXEC) == 17);
	CHECK(!CALL(close, fd, 0, 0));
	CHECK(CALL(write, 17, base, 15) == 15);
	state->held = fget(17);
	CHECK(state->held);
	state->file_page = shmem_read_mapping_page(state->held->f_mapping, 0);
	if (IS_ERR(state->file_page)) {
		state->file_page = NULL;
		CHECK(false);
	}
	CHECK(!probe_value(state, true, 0x1122334455667788ULL));
	init_completion(&inheritance.child_closed);
	init_completion(&inheritance.parent_closed);
	atomic_set(&inheritance.failed, 0);
	/* This tests binding of upstream copy_mm/copy_files output to a second
	 * native client. It does not certify native fork/register return: that
	 * needs a separate architecture entry instead of this kernel start.
	 */
	inheritance.child.pid = kernel_clone(&arguments);
	CHECK(inheritance.child.pid > 0);
	result = inherited_parent_steps(state, &inheritance);
	if (result) {
		atomic_set(&inheritance.failed, 1);
		complete_all(&inheritance.parent_closed);
	}
	status = join_case(&inheritance.child);
	if (!result)
		result = status;
	if (inheritance.child.space) {
		status = kobox_vm_space_destroy(inheritance.child.space);
		if (!result)
			result = status;
	}
	lru_add_drain_all();
	if (inheritance.child.buffer_page) {
		if (page_ref_count(inheritance.child.buffer_page) != 1)
			result = -EINVAL;
		else
			state->report->reclaimed++;
		put_page(inheritance.child.buffer_page);
	}
	state->report->calls += inheritance.report.calls;
	state->report->faults += inheritance.report.faults;
	state->report->exited += inheritance.report.exited;
	state->report->inherited += inheritance.report.inherited;
	state->report->binding_rollbacks += inheritance.report.binding_rollbacks;
	state->report->cpu_mask |= inheritance.report.cpu_mask;
	if (!state->report->line && inheritance.report.line) {
		state->report->line = inheritance.report.line;
		state->report->number = inheritance.report.number;
		state->report->returned = inheritance.report.returned;
	}
	CHECK(!result);
	state->result = 0;
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

static int exercise_fork(struct syscall_case *state)
{
	unsigned long base = state->host->vm->start;
	struct syscall_case child = {.report = state->report};
	struct cred *cred;
	u64 value;
	u32 cpu;
	long fd, result;
	int retained_fd = state->shared_clone ? 19 : 17;
	u32 tid, status;
	unsigned long deadline;
	long count;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(0)));
	state->report->cpu_mask = BIT(raw_smp_processor_id());
	cred = prepare_creds();
	CHECK(cred);
	cred->euid = make_kuid(current_user_ns(), 1000);
	commit_creds(cred);
	CHECK(vm_mmap(NULL, base, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, 0) == base);
	CHECK(!copy_to_user((void __user *)base, "inherited-data", 15));
	CHECK(!put_user((u64)task_pid_nr(current), (u64 __user *)(base + 256)));
	CHECK(!put_user((u64)BIT(1), (u64 __user *)(base + 264)));
	CHECK(!put_user((u32)state->thread, (u32 __user *)(base + 288)));
	CHECK(!put_user((u32)-1, (u32 __user *)(base + 304)));
	CHECK(!put_user((u32)state->group_exit, (u32 __user *)(base + 320)));
	fd = CALL(memfd_create, base, MFD_CLOEXEC, 0);
	CHECK(fd >= 0 && fd != 17);
	CHECK(CALL(dup3, fd, 17, O_CLOEXEC) == 17);
	if (state->shared_clone)
		CHECK(CALL(dup3, 17, retained_fd, O_CLOEXEC) == retained_fd);
	CHECK(!CALL(close, fd, 0, 0));
	CHECK(CALL(write, 17, base, 15) == 15);
	state->held = fget(17);
	CHECK(state->held);
	state->file_page = shmem_read_mapping_page(state->held->f_mapping, 0);
	if (IS_ERR(state->file_page)) {
		state->file_page = NULL;
		CHECK(false);
	}
	CHECK(!probe_value(state, true, 0x1122334455667788ULL));
	result = state->thread ?
		invoke(state, __NR_clone, CLONE_VM | CLONE_FILES | CLONE_SIGHAND |
		       CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
		       base + 4 * PAGE_SIZE, base + 296, base + 296, 0, 0) : state->shared_clone ?
		CALL(clone, CLONE_VM | CLONE_FILES | CLONE_SIGHAND | SIGCHLD,
		     base + 4 * PAGE_SIZE, 0) : CALL(fork, 0, 0, 0);
	CHECK(result > 0);
	child.pid = result;
	child.child = find_get_task_by_vpid(child.pid);
	CHECK(child.child);
	if (state->thread)
		state->peer = child.child;
	/* The autonomous native child executes its inherited FD/COW program,
	 * then issues a real exit_group. The common user machine loop handles
	 * its syscalls; there is no child kernel-start fixture function.
	 */
	if (state->thread) {
		/* The external thread waits on a real futex until its parent has
		 * checked Linux ownership and the parent-TID publication.
		 */
		CHECK(child.child->mm == current->mm && child.child->files == current->files &&
		      child.child->signal == current->signal && child.child->sighand == current->sighand);
		CHECK(!get_user(tid, (u32 __user *)(base + 296)) && tid == child.pid);
		if (state->group_exit) {
			deadline = jiffies + 5 * HZ;
			while (!wait_task_inactive(child.child, TASK_INTERRUPTIBLE) ||
			       task_pt_regs(child.child)->orig_ax != __NR_futex ||
			       task_pt_regs(child.child)->di != base + 312) {
				CHECK(time_before(jiffies, deadline));
				schedule_timeout_uninterruptible(1);
			}
			CHECK(!get_user(cpu, (u32 __user *)(base + 280)) && cpu == 1);
			state->report->cpu_mask |= BIT(cpu);
			CHECK(!hold_private_page(state));
			mmap_read_lock(current->mm);
			count = get_user_pages(base, 1, 0, &state->peer_pages[0]);
			if (count == 1)
				count = get_user_pages(base + 3 * PAGE_SIZE, 1, 0, &state->peer_pages[1]);
			mmap_read_unlock(current->mm);
			CHECK(count == 1);
			if (state->group_exit == 2) {
				CHECK(!put_user((u32)1, (u32 __user *)(base + 312)));
				CHECK(CALL(futex, base + 312, FUTEX_WAKE_PRIVATE, 1) >= 0);
				do {
					CHECK(!get_user(value, (u64 __user *)(base + 328)));
					CHECK(time_before(jiffies, deadline));
					if (!value)
						schedule_timeout_uninterruptible(1);
				} while (!value);
			}
			/* Leave both shared descriptors open. Only Linux exit_files
			 * may drain them; neither side performs a diagnostic close.
			 */
			CHECK(file_count(state->held) == 3);
			state->report->shared_clones++;
			state->report->threads++;
			state->report->group_exits++;
			state->result = 0;
			if (state->group_exit == 3) {
				/* Here only the peer issues exit_group. Its parent waits
				 * on a different futex which nobody normally wakes.
				 */
				CHECK(!put_user((u32)1, (u32 __user *)(base + 312)));
				CHECK(CALL(futex, base + 312, FUTEX_WAKE_PRIVATE, 1) >= 0);
				CALL(futex, base + 336, FUTEX_WAIT_PRIVATE, 0);
				CHECK(false);
			}
			CALL(exit_group, 0, 0, 0);
			CHECK(false);
		}
		CHECK(!put_user((u32)1, (u32 __user *)(base + 312)));
		CHECK(CALL(futex, base + 312, FUTEX_WAKE_PRIVATE, 1) >= 0);
		for (;;) {
			CHECK(!get_user(tid, (u32 __user *)(base + 296)));
			if (!tid)
				break;
			result = CALL(futex, base + 296, FUTEX_WAIT, tid);
			CHECK(!result || result == -EAGAIN);
		}
		CHECK(!get_user(status, (u32 __user *)(base + 304)) && !status);
		/* clear_child_tid precedes final task/machine teardown. */
		while (!wait_task_inactive(child.child, TASK_DEAD))
			schedule_timeout_uninterruptible(1);
		put_task_struct(child.child);
		child.child = NULL;
		state->peer = NULL;
		state->report->exited++;
		state->report->threads++;
	} else {
		CHECK(!join_case(&child));
	}
	CHECK(CALL(fcntl, 17, F_GETFD, 0) == (state->shared_clone ? -EBADF : FD_CLOEXEC));
	CHECK(CALL(lseek, retained_fd, 0, SEEK_CUR) == 7);
	CHECK(invoke(state, __NR_pread64, retained_fd, base + 128, sizeof(value), 32, 0, 0) == sizeof(value));
	CHECK(!get_user(value, (u64 __user *)(base + 128)) && value == 0xaabbccddeeff0011ULL);
	CHECK(invoke(state, __NR_pread64, retained_fd, base + 128, sizeof(cpu), 40, 0, 0) == sizeof(cpu));
	CHECK(!get_user(cpu, (u32 __user *)(base + 128)) && cpu == 1);
	state->report->cpu_mask |= BIT(cpu);
	CHECK(!probe_value(state, false, state->shared_clone ?
			   0xaabbccddeeff0011ULL : 0x1122334455667788ULL));
	CHECK(!hold_private_page(state));
	CHECK(!CALL(close, retained_fd, 0, 0));
	if (state->shared_clone)
		state->report->shared_clones++;
	else
		state->report->native_forks++;
	state->report->inherited++;
	if (!state->shared_clone)
		state->report->cow++;
	state->result = 0;
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

static long drm_request(struct syscall_case *state, int fd, unsigned int command,
			void *data, size_t size)
{
	unsigned long address = state->host->vm->start + 256;
	long result;

	CHECK(!copy_to_user((void __user *)address, data, size));
	result = CALL(ioctl, fd, command, address);
	if (!result)
		CHECK(!copy_from_user(data, (void __user *)address, size));
	return result;
}

static int drm_buffer_access(struct syscall_case *state, unsigned long address,
			     bool write, u64 expected)
{
	struct kobox_linux_vm_event event;

	CHECK(!state->host->vm->probe(state->space->host_space, address, write,
				     expected, state->sequence));
	CHECK(!next_event(state, &event));
	state->sequence = event.sequence;
	CHECK(event.kind == KOBOX_VM_EVENT_STOP && event.value == expected);
	return 0;
}

static int drm_observe_page(struct syscall_case *state, unsigned long address,
			    struct page **page)
{
	struct follow_pfnmap_args follow = {.address = address};
	int result;

	mmap_read_lock(current->mm);
	follow.vma = vma_lookup(current->mm, address);
	result = follow.vma ? follow_pfnmap_start(&follow) : -ENOENT;
	if (!result) {
		if (!pfn_valid(follow.pfn)) {
			result = -EINVAL;
		} else {
			*page = pfn_to_page(follow.pfn);
			get_page(*page);
		}
		follow_pfnmap_end(&follow);
	}
	mmap_read_unlock(current->mm);
	CHECK(!result);
	return 0;
}

static int drm_rights_create(struct syscall_case *state)
{
	unsigned long base = state->host->vm->start;
	unsigned long mapping = base + 4 * PAGE_SIZE;
	struct drm_mode_create_dumb create = {.width = 32, .height = 32, .bpp = 32};
	struct drm_mode_map_dumb map = {0};
	u32 faults = state->report->faults;
	long fd;

	CHECK(!copy_to_user((void __user *)(base + 1536), "card", 5));
	fd = CALL(openat, AT_FDCWD, base + 1536, O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	CHECK(!drm_request(state, fd, DRM_IOCTL_MODE_CREATE_DUMB, &create, sizeof(create)));
	CHECK(create.handle && create.size == PAGE_SIZE);
	map.handle = create.handle;
	CHECK(!drm_request(state, fd, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)));
	CHECK(invoke(state, __NR_mmap, mapping, PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, fd, map.offset) == mapping);
	CHECK(!drm_buffer_access(state, mapping, true, 0x1122334455667788ULL));
	/* PFN insertion publishes a clean PTE. The store must fault again to
	 * let upstream set its dirty bit before the host grants write access.
	 */
	CHECK(state->report->faults == faults + 2);
	CHECK(!drm_observe_page(state, mapping, &state->transfer->page));
	CHECK(!CALL(munmap, mapping, PAGE_SIZE, 0));
	state->transfer->gem_handle = create.handle;
	state->transfer->gem_offset = map.offset;
	/* The datagram payload carries the handle alongside SCM_RIGHTS;
	 * the receiver must get it through recvmsg, not the observer state.
	 */
	CHECK(!put_user(create.handle, (u32 __user *)(base + 128)));
	state->report->drm_files++;
	state->report->gem_handles++;
	return fd;
}

static int drm_rights_receive(struct syscall_case *state, int fd)
{
	unsigned long mapping = state->host->vm->start + 4 * PAGE_SIZE;
	struct drm_mode_map_dumb map = {0};
	u32 faults = state->report->faults;
	u32 handle;

	CHECK(!get_user(handle, (u32 __user *)(state->host->vm->start + 128)));
	CHECK(handle == state->transfer->gem_handle);
	map.handle = handle;

	/* SCM_RIGHTS transfers the open file description, including its GEM
	 * handle table; it does not recreate a DRM open or copy a handle table.
	 */
	CHECK(!drm_request(state, fd, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)));
	CHECK(map.offset == state->transfer->gem_offset);
	CHECK(invoke(state, __NR_mmap, mapping, PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, fd, map.offset) == mapping);
	CHECK(!drm_buffer_access(state, mapping, false, 0x1122334455667788ULL));
	CHECK(state->report->faults == faults + 1);
	CHECK(!CALL(close, fd, 0, 0));
	CHECK(!drm_buffer_access(state, mapping, true, 0x8877665544332211ULL));
	CHECK(state->report->faults == faults + 2);
	CHECK(!drm_buffer_access(state, mapping, false, 0x8877665544332211ULL));
	CHECK(!CALL(munmap, mapping, PAGE_SIZE, 0));
	/* Leave the handle live: the final queued/file reference must drive
	 * upstream drm_release, GEM handle removal and backing destruction.
	 */
	return 0;
}

static int exercise_drm(struct syscall_case *state)
{
	static const char paths[] = "card\0private";
	static const char driver[] = "kobox-gem-lifetime";
	unsigned long base = state->host->vm->start, mapping = base + 4 * PAGE_SIZE;
	unsigned long bad = base + state->host->vm->length + PAGE_SIZE;
	struct path root = {.mnt = state->drm_mount, .dentry = state->drm_mount->mnt_root};
	struct drm_version version = {.name_len = 63, .name = (void __user *)bad};
	struct drm_mode_create_dumb create = {.width = 32, .height = 32, .bpp = 32};
	struct drm_mode_map_dumb map = {0};
	struct drm_gem_close close = {0};
	char name[sizeof(driver)] = {0};
	long fd, other;
	u32 initial_faults;
	u64 offset, sentinel = 0xaabbaabb00000000ULL | state->cpu;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	state->report->cpu_mask |= BIT(raw_smp_processor_id());
	CHECK(!unshare_fs_struct());
	set_fs_root(current->fs, &root);
	set_fs_pwd(current->fs, &root);
	CHECK(vm_mmap(NULL, base, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) == base);
	CHECK(!copy_to_user((void __user *)base, paths, sizeof(paths)));
	/* Real credential and DAC checks include fsuid, not just an euid cache. */
	CHECK(!CALL(setresuid, 1000, 1000, 1000));
	CHECK(CALL(geteuid, 0, 0, 0) == 1000);
	CHECK(CALL(openat, AT_FDCWD, base + 5, O_RDWR) == -EACCES);
	state->report->invalid++;
	fd = CALL(openat, AT_FDCWD, base, O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0 && fd != 17);
	CHECK(CALL(dup3, fd, 17, O_CLOEXEC) == 17);
	CHECK(!CALL(close, fd, 0, 0));
	state->held = fget(17);
	CHECK(state->held);
	CHECK(CALL(ioctl, 17, DRM_IOCTL_VERSION, bad) == -EFAULT);
	CHECK(drm_request(state, 17, DRM_IOCTL_VERSION, &version, sizeof(version)) == -EFAULT);
	state->report->invalid += 2;
	version.name = (void __user *)(base + 512);
	CHECK(!drm_request(state, 17, DRM_IOCTL_VERSION, &version, sizeof(version)));
	CHECK(version.name_len == sizeof(driver) - 1);
	CHECK(!copy_from_user(name, version.name, sizeof(driver) - 1));
	CHECK(!memcmp(name, driver, sizeof(driver)));
	state->report->nested++;
	CHECK(!drm_request(state, 17, DRM_IOCTL_MODE_CREATE_DUMB, &create, sizeof(create)));
	CHECK(create.handle && create.handle < 17 && create.pitch == 128 && create.size == PAGE_SIZE);
	map.handle = create.handle;
	/* Give an unrelated Linux FD exactly the GEM handle's integer value.
	 * Closing it must not remove a GEM handle or touch a host descriptor.
	 */
	fd = CALL(memfd_create, base, MFD_CLOEXEC, 0);
	CHECK(fd >= 0 && fd != create.handle);
	CHECK(CALL(dup3, fd, create.handle, O_CLOEXEC) == create.handle);
	CHECK(!CALL(close, fd, 0, 0));
	CHECK(!CALL(close, create.handle, 0, 0));
	CHECK(!drm_request(state, 17, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)));
	offset = map.offset;
	other = CALL(openat, AT_FDCWD, base, O_RDWR | O_CLOEXEC);
	CHECK(other >= 0);
	CHECK(drm_request(state, other, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)) == -ENOENT);
	CHECK(!CALL(close, other, 0, 0));
	state->report->invalid++;
	state->report->drm_files += 2;
	CHECK(CALL(dup3, 17, 19, O_CLOEXEC) == 19);
	CHECK(!CALL(close, 17, 0, 0));
	CHECK(!drm_request(state, 19, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)));
	CHECK(map.offset == offset);
	CHECK(invoke(state, __NR_mmap, mapping, PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, 19, offset) == mapping);
	CHECK(invoke(state, __NR_mmap, mapping + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_FIXED_NOREPLACE, 19, offset) == mapping + PAGE_SIZE);
	initial_faults = state->report->faults;
	CHECK(!drm_buffer_access(state, mapping, true, sentinel));
	CHECK(!drm_buffer_access(state, mapping + PAGE_SIZE, false, sentinel));
	CHECK(state->report->faults >= initial_faults + 2);
	/* Observe the real Linux PFNMAP page, not a synthetic struct page or a
	 * direct call of the driver's fault method. Hold only a test reference.
	 */
	CHECK(!drm_observe_page(state, mapping, &state->peer_pages[0]));
	close.handle = create.handle;
	CHECK(!drm_request(state, 19, DRM_IOCTL_GEM_CLOSE, &close, sizeof(close)));
	CHECK(drm_request(state, 19, DRM_IOCTL_GEM_CLOSE, &close, sizeof(close)) == -EINVAL);
	CHECK(drm_request(state, 19, DRM_IOCTL_MODE_MAP_DUMB, &map, sizeof(map)) == -ENOENT);
	state->report->invalid += 2;
	CHECK(!CALL(close, 19, 0, 0));
	CHECK(CALL(ioctl, 19, DRM_IOCTL_VERSION, base + 256) == -EBADF);
	state->report->invalid++;
	CHECK(!drm_buffer_access(state, mapping + PAGE_SIZE, false, sentinel));
	CHECK(!CALL(munmap, mapping, PAGE_SIZE, 0));
	CHECK(!drm_buffer_access(state, mapping + PAGE_SIZE, false, sentinel));
	state->report->descriptors++;
	state->report->gem_handles++;
	state->result = 0;
	/* The last VMA survives both handle and FD closure until task/MM exit. */
	CALL(exit_group, 0, 0, 0);
	CHECK(false);
}

static int exercise_autonomous(struct syscall_case *state)
{
	unsigned long base = state->host->vm->start;
	struct kobox_linux_vm_event event;
	long count;
	int result;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	state->report->cpu_mask |= BIT(raw_smp_processor_id());
	/* Shared metadata lets the observer inspect the result after both MMs
	 * exit; the separate private payload still exercises real fork COW.
	 */
	CHECK(vm_mmap(NULL, base, PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) == base);
	CHECK(vm_mmap(NULL, base + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) == base + PAGE_SIZE);
	CHECK(vm_mmap(NULL, base + 16 * PAGE_SIZE, 16 * PAGE_SIZE,
		      PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) == base + 16 * PAGE_SIZE);
	mmap_read_lock(current->mm);
	count = get_user_pages(base, 2, FOLL_WRITE, state->peer_pages);
	mmap_read_unlock(current->mm);
	CHECK(count == 2);
	CHECK(!state->host->vm->probe(state->space->host_space, base, 11,
				     state->cpu, state->sequence));
	CHECK(!next_event(state, &event));
	CHECK(event.kind == KOBOX_VM_EVENT_SYSCALL && event.syscall.number == __NR_getpid);
	CHECK(event.user.sp >= base + 16 * PAGE_SIZE && event.user.sp < base + 32 * PAGE_SIZE);
	{
		struct kobox_linux_vm_event bad = event;

		/* A rejected handoff must neither consume the pending instruction
		 * nor take ownership of the native execution context.
		 */
		bad.kind = KOBOX_VM_EVENT_STOP;
		CHECK(kobox_user_adopt(state->space, state->space->host_space, &bad) == -EINVAL);
	}
	state->sequence = event.sequence;
	state->syscall_sequence = event.syscall.sequence;
	state->result = 0;
	state->adopted = true;
	result = kobox_user_adopt(state->space, state->space->host_space, &event);
	/* Only pre-handoff failures return; the fixture still owns the binding. */
	state->adopted = false;
	return result;
}

static int child_entry(void *argument)
{
	struct syscall_case *state = argument;

	get_task_struct(current);
	state->child = current;
	state->result = state->autonomous ? exercise_autonomous(state) :
		state->transfer ? exercise_rights(state) :
		state->drm_mount ? exercise_drm(state) :
		state->native_fork || state->shared_clone ? exercise_fork(state) :
		state->inheritance ? exercise_inheritance(state) :
		exercise(state);
	if (state->transfer) {
		atomic_set(&state->transfer->failed, 1);
		complete_all(&state->transfer->bound);
		complete_all(&state->transfer->sent);
		complete_all(&state->transfer->captured[0]);
		complete_all(&state->transfer->captured[1]);
	}
	if (state->thread)
		do_group_exit(1 << 8);
	do_exit(1 << 8);
}

static int start_case(struct syscall_case *state)
{
	const struct kobox_linux_vm_test *host = state->host->vm;
	struct kernel_clone_args args = {
		.flags = CLONE_VM, .exit_signal = SIGCHLD,
		.fn = child_entry, .fn_arg = state,
	};
	int result;

	state->space = kobox_vm_space_create(host->spaces[state->cpu], host->operations,
					   host->start, host->length);
	if (IS_ERR(state->space)) {
		result = PTR_ERR(state->space);
		state->space = NULL;
		return result;
	}
	result = host->operations->enable_syscalls(state->space->host_space);
	if (result)
		return result;
	kthread_use_mm(state->space->mm);
	state->sequence = 0;
	state->adopted = false;
	state->child = NULL;
	state->result = -EINVAL;
	state->pid = kernel_clone(&args);
	kthread_unuse_mm(state->space->mm);
	return state->pid < 0 ? state->pid : 0;
}

static int join_case(struct syscall_case *state)
{
	unsigned long deadline;
	int status = 0;
	pid_t waited;

	waited = kernel_wait(state->pid, &status);
	if (waited != state->pid || !state->child)
		panic("syscall test could not reap its child");
	deadline = jiffies + 5 * HZ;
	while (!wait_task_inactive(state->child, TASK_DEAD)) {
		if (time_after_eq(jiffies, deadline))
			panic("syscall test task did not finish exit");
		schedule_timeout_uninterruptible(1);
	}
	put_task_struct(state->child);
	state->child = NULL;
	if (!status && !state->result) {
		state->report->exited++;
		return 0;
	} else if (!state->report->line) {
		state->report->line = __LINE__;
		state->report->returned = status;
	}
	return -EINVAL;
}

static int run_case(struct syscall_case *state)
{
	const struct kobox_linux_vm_test *host = state->host->vm;
	int result, cleanup;
	unsigned long deadline;
	unsigned int index;

	result = start_case(state);
	if (result)
		goto destroy;
	result = join_case(state);
	if (state->adopted) {
		void *metadata = page_address(state->peer_pages[0]);

		/* exit_thread, not this fixture, must have reaped the native root
		 * and released its binding. Never dereference the old space here.
		 */
		state->space = NULL;
		cleanup = 0;
		if (!result && (READ_ONCE(*(u32 *)(metadata + 336)) != 0xc11e17 ||
				READ_ONCE(*(u32 *)(metadata + 344)) != state->cpu))
			result = -EINVAL;
		if (!result) {
			state->report->autonomous++;
			state->report->native_forks++;
			state->report->exited++; /* Child was reaped by actual wait4. */
		}
		goto released;
	}
	if (state->peer) {
		deadline = jiffies + 5 * HZ;
		while (!wait_task_inactive(state->peer, TASK_DEAD)) {
			if (time_after_eq(jiffies, deadline))
				panic("syscall test peer did not finish group exit");
			schedule_timeout_uninterruptible(1);
		}
		if (state->peer->mm || state->peer->files)
			result = -EINVAL;
		put_task_struct(state->peer);
		state->peer = NULL;
		if (!result)
			state->report->exited++;
	}
	if (state->group_exit && state->peer_pages[0]) {
		void *metadata = page_address(state->peer_pages[0]);

		if (READ_ONCE(*(u32 *)(metadata + 296)) ||
		    READ_ONCE(*(u32 *)(metadata + 304)) != (state->group_exit == 3 ? 0 : (u32)-1) ||
		    (!!READ_ONCE(*(u64 *)(metadata + 328)) != (state->group_exit == 2)))
			result = -EINVAL;
	}
	if (!result && (!state->held || file_count(state->held) != (state->drm_mount ? 2 : 1))) {
		state->report->line = __LINE__;
		state->report->returned = state->held ? file_count(state->held) : -1;
		result = -EINVAL;
	}
destroy:
	if (!state->space)
		return result;
	cleanup = kobox_vm_space_destroy(state->space);
	if (!result)
		result = cleanup;
released:
	if (!cleanup && host->operations->syscall_return(host->spaces[state->cpu],
			state->sequence, state->syscall_sequence,
			&(struct kobox_x86_user_regs){0}) != -ESRCH)
		result = -EINVAL;
	if (state->held) {
		if (!cleanup && file_count(state->held) != 1) {
			state->report->line = __LINE__;
			state->report->returned = file_count(state->held);
			result = -EINVAL;
		}
		__fput_sync(state->held);
		state->held = NULL;
	}
	/* Newly faulted/read pages may still have upstream per-CPU LRU batch
	 * references. Drain those real batches before checking final ownership.
	 */
	flush_delayed_fput();
	lru_add_drain_all();
	if (state->file_page) {
		if (page_ref_count(state->file_page) != 1 ||
		    folio_mapping(page_folio(state->file_page)))
			result = -EINVAL;
		else
			state->report->reclaimed++;
		put_page(state->file_page);
		state->file_page = NULL;
	}
	if (state->buffer_page) {
		if (page_ref_count(state->buffer_page) != 1)
			result = -EINVAL;
		else
			state->report->reclaimed++;
		put_page(state->buffer_page);
		state->buffer_page = NULL;
	}
	for (index = 0; index < ARRAY_SIZE(state->peer_pages); index++) {
		if (!state->peer_pages[index])
			continue;
		if (page_ref_count(state->peer_pages[index]) != 1) {
			state->report->line = __LINE__;
			state->report->returned = page_ref_count(state->peer_pages[index]);
			result = -EINVAL;
		} else {
			state->report->reclaimed++;
		}
		put_page(state->peer_pages[index]);
		state->peer_pages[index] = NULL;
	}
	if (result && !state->report->line)
		state->report->line = __LINE__;
	return result;
}

static int run_transfer(struct syscall_case *root)
{
	struct kobox_syscall_report reports[2] = {0};
	struct syscall_case cases[2] = {0};
	struct transfer_case *transfer = root->transfer;
	unsigned int cpu;
	int result = 0, status;

	init_completion(&transfer->bound);
	init_completion(&transfer->sent);
	init_completion(&transfer->captured[0]);
	init_completion(&transfer->captured[1]);
	atomic_set(&transfer->failed, 0);
	for (cpu = 0; cpu < 2; cpu++) {
		cases[cpu].host = root->host;
		cases[cpu].report = &reports[cpu];
		cases[cpu].cpu = cpu;
		cases[cpu].transfer = transfer;
		cases[cpu].drm_mount = root->drm_mount;
		result = start_case(&cases[cpu]);
		if (result) {
			atomic_set(&transfer->failed, 1);
			complete_all(&transfer->bound);
			complete_all(&transfer->sent);
			complete_all(&transfer->captured[0]);
			complete_all(&transfer->captured[1]);
			break;
		}
	}
	for (cpu = 0; cpu < 2; cpu++) {
		if (cases[cpu].pid > 0) {
			status = join_case(&cases[cpu]);
			if (!result)
				result = status;
		}
		if (cases[cpu].space) {
			status = kobox_vm_space_destroy(cases[cpu].space);
			if (!result)
				result = status;
		}
		root->report->calls += reports[cpu].calls;
		root->report->faults += reports[cpu].faults;
		root->report->nested += reports[cpu].nested;
		root->report->invalid += reports[cpu].invalid;
		root->report->transfers += reports[cpu].transfers;
		root->report->drm_files += reports[cpu].drm_files;
		root->report->gem_handles += reports[cpu].gem_handles;
		root->report->truncated += reports[cpu].truncated;
		root->report->rendezvous += reports[cpu].rendezvous;
		root->report->race_sent += reports[cpu].race_sent;
		root->report->race_rejected += reports[cpu].race_rejected;
		root->report->exited += reports[cpu].exited;
		root->report->cpu_mask |= reports[cpu].cpu_mask;
		if (!root->report->line && reports[cpu].line) {
			root->report->line = reports[cpu].line;
			root->report->number = reports[cpu].number;
			root->report->returned = reports[cpu].returned;
		}
	}
	flush_delayed_fput();
	if (!transfer->held || file_count(transfer->held) != 1)
		result = -EINVAL;
	else if (!result)
		root->report->queued_exit += transfer->race ? root->report->race_sent : 1;
	if (transfer->held)
		__fput_sync(transfer->held);
	flush_delayed_fput();
	lru_add_drain_all();
	if (transfer->page) {
		if (page_ref_count(transfer->page) != 1 ||
		    folio_mapping(page_folio(transfer->page)))
			result = -EINVAL;
		else
			root->report->reclaimed++;
		put_page(transfer->page);
	} else {
		result = -EINVAL;
	}
	return result;
}

static int manager(void *argument)
{
	struct syscall_case *state = argument;
	int result;

	result = unshare_files();
	if (result)
		goto done;
	kernel_sigaction(SIGCHLD, SIG_DFL);
	if (state->transfer) {
		result = run_transfer(state);
		goto done;
	}
	if (state->inheritance || state->native_fork || state->shared_clone) {
		state->cpu = 0;
		result = run_case(state);
		goto done;
	}
	for (state->cpu = 0; state->cpu < 2; state->cpu++) {
		result = run_case(state);
		if (result)
			break;
	}
done:
	state->result = result;
	complete(&state->done);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return result;
}

enum syscall_gate_kind {
	SYSCALL_BASE, SYSCALL_RIGHTS, SYSCALL_RIGHTS_RACE, SYSCALL_INHERITANCE, SYSCALL_FORK,
	SYSCALL_CLONE,
	SYSCALL_THREAD,
	SYSCALL_GROUP_EXIT_WAIT, SYSCALL_GROUP_EXIT_RUNNING,
	SYSCALL_GROUP_EXIT_PEER,
	SYSCALL_DRM,
	SYSCALL_DRM_RIGHTS,
	SYSCALL_AUTONOMOUS,
};

static int verify_mounted(const struct kobox_syscall_test *host,
	struct kobox_syscall_report *report, enum syscall_gate_kind kind, struct vfsmount *mount)
{
	struct syscall_case state = {.host = host, .report = report, .drm_mount = mount};
	bool rights = kind == SYSCALL_RIGHTS || kind == SYSCALL_RIGHTS_RACE ||
		kind == SYSCALL_DRM_RIGHTS;
	bool race = kind == SYSCALL_RIGHTS_RACE;
	struct transfer_case transfer = {.race = race};
	struct task_struct *task;
	int result;

	if (!host || host->size != sizeof(*host) || !host->vm ||
	    (kind == SYSCALL_AUTONOMOUS ? !!host->issue : !host->issue) ||
	    !host->vm->operations->enable_syscalls || !host->vm->operations->syscall_return ||
	    !report || report->size != sizeof(*report) || current->mm || num_online_cpus() != 2)
		return -EINVAL;
	if (rights)
		state.transfer = &transfer;
	state.inheritance = kind == SYSCALL_INHERITANCE;
	state.autonomous = kind == SYSCALL_AUTONOMOUS;
	state.native_fork = kind == SYSCALL_FORK;
	state.group_exit = kind == SYSCALL_GROUP_EXIT_WAIT ? 1 :
		kind == SYSCALL_GROUP_EXIT_RUNNING ? 2 : kind == SYSCALL_GROUP_EXIT_PEER ? 3 : 0;
	state.thread = kind == SYSCALL_THREAD || state.group_exit;
	state.shared_clone = kind == SYSCALL_CLONE || state.thread;
	init_completion(&state.done);
	task = kthread_run(manager, &state, "syscall-gate");
	if (IS_ERR(task))
		return PTR_ERR(task);
	wait_for_completion(&state.done);
	result = kthread_stop(task);
	rcu_barrier();
	flush_delayed_fput();
	report->warnings = kobox_linux_exception_warnings();
	if (!result && (report->warnings || report->cpu_mask != 3 ||
		       report->exited != (state.autonomous ? 4 : 2)))
		result = -EINVAL;
	if (!result && state.autonomous && (report->autonomous != 2 ||
					    report->native_forks != 2 || report->reclaimed != 4))
		result = -EINVAL;
	if (!result && race && (report->rendezvous != 2 ||
			       report->race_sent + report->race_rejected != 1 ||
			       report->queued_exit != report->race_sent || report->reclaimed != 1))
		result = -EINVAL;
	if (!result && rights && !race && (report->transfers != 1 || report->truncated != 1 ||
				  report->queued_exit != 1 || report->reclaimed != 1 ||
				  report->invalid != 3 || report->nested != 2 ||
				  report->faults != (mount ? 4 : 1)))
		result = -EINVAL;
	if (!result && kind == SYSCALL_DRM_RIGHTS &&
	    (report->drm_files != 1 || report->gem_handles != 1))
		result = -EINVAL;
	if (!result && state.inheritance && (report->inherited != 1 || report->cow != 1 ||
					    report->binding_rollbacks != 2 ||
					    report->reclaimed != 3 || report->faults != 4))
		result = -EINVAL;
	if (!result && state.native_fork && (report->native_forks != 1 || report->inherited != 1 ||
					    report->cow != 1 || report->reclaimed != 2 ||
					    report->faults != 2))
		result = -EINVAL;
	if (!result && state.shared_clone && !state.group_exit &&
					    (report->shared_clones != 1 || report->inherited != 1 ||
					     report->cow || report->reclaimed != 2 || report->faults != 1))
		result = -EINVAL;
	if (!result && state.group_exit && (report->group_exits != 1 || report->shared_clones != 1 ||
					   report->reclaimed != 4 || report->faults != 1))
		result = -EINVAL;
	if (!result && report->threads != state.thread)
		result = -EINVAL;
	if (!result && kind == SYSCALL_DRM && (report->drm_files != 4 || report->gem_handles != 2 ||
					     report->nested != 2 || report->invalid != 14 ||
					     report->descriptors != 2 || report->faults < 4 ||
					     report->reclaimed != 2))
		result = -EINVAL;
	if (!result && kind == SYSCALL_BASE && (
		       report->faults != 4 || report->tls != 4 ||
		       report->nested != 2 || report->invalid != 6 ||
		       report->descriptors != 2 || report->reclaimed != 4))
		result = -EINVAL;
	report->result = result;
	return result;
}

static int verify(const struct kobox_syscall_test *host,
		  struct kobox_syscall_report *report, enum syscall_gate_kind kind)
{
	return verify_mounted(host, report, kind, NULL);
}

__attribute__((visibility("default")))
int kobox_linux_autonomous_client_verify(const struct kobox_syscall_test *host,
					struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_AUTONOMOUS);
}

int kobox_linux_drm_syscall_verify(const struct kobox_syscall_test *host,
	struct kobox_syscall_report *report, struct vfsmount *mount)
{
	if (!mount)
		return -EINVAL;
	return verify_mounted(host, report, SYSCALL_DRM, mount);
}

int kobox_linux_drm_rights_verify(const struct kobox_syscall_test *host,
	struct kobox_syscall_report *report, struct vfsmount *mount)
{
	if (!mount)
		return -EINVAL;
	return verify_mounted(host, report, SYSCALL_DRM_RIGHTS, mount);
}

__attribute__((visibility("default")))
int kobox_linux_syscall_verify(const struct kobox_syscall_test *host,
			       struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_BASE);
}

__attribute__((visibility("default")))
int kobox_linux_clone_verify(const struct kobox_syscall_test *host,
			    struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_CLONE);
}

__attribute__((visibility("default")))
int kobox_linux_thread_verify(const struct kobox_syscall_test *host,
			     struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_THREAD);
}

__attribute__((visibility("default")))
int kobox_linux_group_exit_wait_verify(const struct kobox_syscall_test *host,
				      struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_GROUP_EXIT_WAIT);
}

__attribute__((visibility("default")))
int kobox_linux_group_exit_running_verify(const struct kobox_syscall_test *host,
					 struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_GROUP_EXIT_RUNNING);
}

__attribute__((visibility("default")))
int kobox_linux_group_exit_peer_verify(const struct kobox_syscall_test *host,
				      struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_GROUP_EXIT_PEER);
}

__attribute__((visibility("default")))
int kobox_linux_fd_transfer_verify(const struct kobox_syscall_test *host,
				   struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_RIGHTS);
}

__attribute__((visibility("default")))
int kobox_linux_fd_exit_race_verify(const struct kobox_syscall_test *host,
				    struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_RIGHTS_RACE);
}

__attribute__((visibility("default")))
int kobox_linux_fd_inheritance_verify(const struct kobox_syscall_test *host,
				      struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_INHERITANCE);
}

__attribute__((visibility("default")))
int kobox_linux_fork_verify(const struct kobox_syscall_test *host,
			    struct kobox_syscall_report *report)
{
	return verify(host, report, SYSCALL_FORK);
}
