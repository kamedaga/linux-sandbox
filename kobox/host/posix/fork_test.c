// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "native fork line %d: %s\n", __LINE__, #expression); \
		return 1; \
	} \
} while (0)

static int begin(struct kobox_posix_vm *space, struct kobox_posix_vm_event *event)
{
	space->control->write = 4;
	space->control->syscall_number = SYS_fork;
	memset(space->control->syscall_arguments, 0, sizeof(space->control->syscall_arguments));
	CHECK(!kobox_posix_vm_resume(space));
	CHECK(!kobox_posix_vm_wait(space, false, event));
	CHECK(event->kind == KOBOX_POSIX_VM_SYSCALL && event->syscall.number == SYS_fork);
	return 0;
}

static int finish(struct kobox_posix_vm *space, uint64_t value)
{
	struct kobox_posix_vm_event event;
	struct kobox_x86_user_regs registers;

	CHECK(!kobox_posix_vm_read_registers(space, &registers));
	registers.ax = value;
	CHECK(!kobox_posix_vm_syscall_return(space, space->syscall_sequence, &registers));
	CHECK(!kobox_posix_vm_resume(space));
	CHECK(!kobox_posix_vm_wait(space, false, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_STOP);
	CHECK(atomic_load(&space->control->event) == KOBOX_VM_CLIENT_DONE &&
	      space->control->result == value);
	return 0;
}

static int fork_context(const char *client)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm parent = {0}, child = {0}, grandchild = {0};
	struct kobox_posix_vm_event event;
	struct kobox_x86_user_regs before, after;
	struct rlimit original, limited;
	int result;

	CHECK(!kobox_posix_memory_backing_init(&ram, 8192));
	CHECK(!kobox_posix_vm_create(&parent, client, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
	CHECK(!kobox_posix_vm_enable_syscalls(&parent));
	CHECK(!begin(&parent, &event));
	CHECK(!kobox_posix_vm_read_registers(&parent, &before));
	CHECK(kobox_posix_vm_clone(&parent, event.syscall.sequence - 1, false, &child) == ESTALE);
	/* Fail resource import after native creation, not just before fork.
	 * The parent must retain its original frame and pending syscall.
	 */
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, NULL, &original));
	limited = original;
	limited.rlim_cur = KOBOX_VM_CONTROL_FD + 1;
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, &limited, NULL));
	CHECK(kobox_posix_vm_clone(&parent, event.syscall.sequence, false, &child) == EMFILE);
	CHECK(!child.pid && !child.control && !child.control_backing.initialized);
	CHECK(!parent.syscall_failed && parent.syscall_pending);
	CHECK(!kobox_posix_vm_read_registers(&parent, &after));
	CHECK(!memcmp(&before, &after, sizeof(before)));
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, &original, NULL));
	memset(&child, 0, sizeof(child));
	result = kobox_posix_vm_clone(&parent, event.syscall.sequence, false, &child);
	if (result)
		fprintf(stderr, "native fork: %s\n", strerror(result));
	CHECK(!result);
	CHECK(parent.pid && child.pid && parent.pid != child.pid);
	CHECK(!child.running && child.syscall_pending && parent.syscall_pending);
	CHECK(child.control != parent.control && child.control_address == parent.control_address);
	CHECK(child.control_backing.descriptor != parent.control_backing.descriptor);
	CHECK(!kobox_posix_vm_read_registers(&parent, &after));
	CHECK(!memcmp(&before, &after, sizeof(before)));
	CHECK(!kobox_posix_vm_read_registers(&child, &after));
	before.ax = 0;
	CHECK(!memcmp(&before, &after, sizeof(before)));
	CHECK(!finish(&child, 0));
	CHECK(atomic_load(&parent.control->event) == KOBOX_VM_CLIENT_RUNNING);
	CHECK(!finish(&parent, 1234));
	CHECK(child.control->result == 0);
	/* A second generation also belongs to the service, not the native
	 * client parent. Reaping the latter cannot orphan the former.
	 */
	CHECK(!begin(&child, &event));
	CHECK(!kobox_posix_vm_clone(&child, event.syscall.sequence, false, &grandchild));
	CHECK(!finish(&grandchild, 0));
	CHECK(!finish(&child, 5678));
	CHECK(!kobox_posix_vm_destroy(&child));
	CHECK(!kobox_posix_vm_read_registers(&grandchild, &after));
	CHECK(!kobox_posix_vm_destroy(&grandchild));
	CHECK(!kobox_posix_vm_destroy(&parent));
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	return 0;
}

static int native_child_count(pid_t owner, pid_t parent, pid_t bystander, bool *extra)
{
	char path[96];
	FILE *stream;
	pid_t pid;
	int count = 0;

	*extra = false;
	snprintf(path, sizeof(path), "/proc/self/task/%d/children", owner);
	stream = fopen(path, "re");
	if (!stream)
		return -1;
	while (fscanf(stream, "%d", &pid) == 1) {
		count++;
		if (pid != parent && pid != bystander)
			*extra = true;
	}
	if (ferror(stream))
		count = -1;
	fclose(stream);
	return count;
}

static int mapped(pid_t pid, uint64_t address)
{
	char path[96], line[512];
	unsigned long start, end;
	FILE *stream;
	int found = 0;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	stream = fopen(path, "re");
	if (!stream)
		return -1;
	while (fgets(line, sizeof(line), stream)) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2) {
			found = -1;
			break;
		}
		if (address >= start && address < end)
			found = 1;
	}
	if (ferror(stream))
		found = -1;
	fclose(stream);
	return found;
}

static int access_start(struct kobox_posix_vm *space, uint64_t address,
			unsigned int write, uint64_t value)
{
	space->control->address = address;
	space->control->write = write;
	space->control->value = value;
	return kobox_posix_vm_resume(space);
}

static int access_done(struct kobox_posix_vm *space, uint64_t value)
{
	struct kobox_posix_vm_event event;

	CHECK(!kobox_posix_vm_wait(space, false, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_STOP);
	CHECK(atomic_load(&space->control->event) == KOBOX_VM_CLIENT_DONE &&
	      space->control->result == value);
	return 0;
}

static int shared_contexts(const char *client)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm parent = {0}, child = {0}, copy = {0}, sibling = {0};
	struct kobox_posix_vm_event event, parent_fault, child_fault;
	struct kobox_x86_user_regs registers;
	struct rlimit original, limited;
	unsigned int rw = KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE;
	uint64_t base = KOBOX_VM_TEST_WINDOW_BASE, child_control, parent_control, sibling_control;
	uint64_t *direct;

	CHECK(!kobox_posix_memory_backing_init(&ram, 4 * 4096));
	direct = mmap(NULL, ram.size, PROT_READ | PROT_WRITE,
		      MAP_SHARED, ram.descriptor, 0);
	CHECK(direct != MAP_FAILED);
	direct[0] = 0x12345678;
	direct[4096 / sizeof(*direct)] = 0xabcdef;
	CHECK(!kobox_posix_vm_create(&parent, client, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
	CHECK(!kobox_posix_vm_enable_syscalls(&parent));
	CHECK(!begin(&parent, &event));
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, NULL, &original));
	limited = original;
	limited.rlim_cur = KOBOX_VM_CONTROL_FD + 1;
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, &limited, NULL));
	CHECK(kobox_posix_vm_clone(&parent, event.syscall.sequence, true, &child) == EMFILE);
	CHECK(!child.pid && !child.group && !parent.syscall_failed);
	CHECK(!prlimit(parent.pid, RLIMIT_NOFILE, &original, NULL));
	memset(&child, 0, sizeof(child));
	CHECK(!kobox_posix_vm_clone(&parent, event.syscall.sequence, true, &child));
	CHECK(child.group == parent.group && child.control != parent.control);
	child_control = child.control_address;
	parent_control = parent.control_address;
	CHECK(child_control && child_control != parent_control);
	CHECK(mapped(parent.pid, child_control) == 1);
	CHECK(!finish(&parent, 1234));
	CHECK(!kobox_posix_vm_map(&parent, base, 0, 4096, rw));
	CHECK(!kobox_posix_vm_map(&parent, base + 3 * 4096, 3 * 4096, 4096, rw));
	CHECK(!kobox_posix_vm_read_registers(&child, &registers));
	/* The machine Gate supplies a separate user stack and entry; production
	 * frames come from upstream copy_thread, not this diagnostic loop.
	 */
	registers.ip = child.control->client_entry;
	registers.sp = base + 4 * 4096 - 8;
	registers.di = child_control;
	CHECK(!kobox_posix_vm_syscall_return(&child, child.syscall_sequence, &registers));
	CHECK(!access_start(&child, base, 0, 0));
	CHECK(!access_done(&child, 0x12345678));
	/* A child-side reset must invalidate the parent's actual translation.
	 * Sharing RAM backing across separate address spaces cannot pass this.
	 */
	CHECK(!kobox_posix_vm_reset(&child, base, 4096));
	CHECK(!access_start(&parent, base, 0, 0));
	CHECK(!access_start(&child, base + 4096, 8, 0x55aa55aa));
	CHECK(!kobox_posix_vm_wait(&parent, false, &parent_fault));
	CHECK(!kobox_posix_vm_wait(&child, false, &child_fault));
	CHECK(parent_fault.kind == KOBOX_POSIX_VM_FAULT && parent_fault.address == base);
	CHECK(child_fault.kind == KOBOX_POSIX_VM_FAULT &&
	      child_fault.address == base + 4096 && child_fault.sp == base + 4096 + 8);
	CHECK(parent.control->fault_address == base &&
	      child.control->fault_address == base + 4096);
	CHECK(!kobox_posix_vm_map(&child, base, 4096, 4096, rw));
	CHECK(!kobox_posix_vm_map(&parent, base + 4096, 0, 4096, rw));
	CHECK(!kobox_posix_vm_resume(&parent));
	CHECK(!access_done(&parent, 0xabcdef));
	CHECK(atomic_load(&child.control->event) == KOBOX_VM_CLIENT_FAULT);
	CHECK(!kobox_posix_vm_resume(&child));
	CHECK(!access_done(&child, 0x55aa55aa));
	CHECK(direct[0] == 0x55aa55aa);
	/* A private fork out of this shared MM must not retain a phantom
	 * machine context for the parent's peer.
	 */
	CHECK(!begin(&child, &event));
	CHECK(!kobox_posix_vm_clone(&child, event.syscall.sequence, false, &copy));
	CHECK(copy.group != child.group && mapped(copy.pid, parent_control) == 0);
	/* The host port never invents COW for mapped Linux RAM. This machine
	 * test supplies a private stack page; the Linux fork Gate exercises
	 * upstream PTE invalidation/fault-driven COW separately.
	 */
	memcpy((char *)direct + 2 * 4096, (char *)direct + 3 * 4096, 4096);
	CHECK(!kobox_posix_vm_map(&copy, base + 3 * 4096, 2 * 4096, 4096, rw));
	CHECK(!finish(&copy, 0));
	CHECK(!finish(&child, 4567));
	CHECK(!kobox_posix_vm_destroy(&copy));
	CHECK(!begin(&child, &event));
	CHECK(!kobox_posix_vm_clone(&child, event.syscall.sequence, true, &sibling));
	sibling_control = sibling.control_address;
	CHECK(mapped(parent.pid, sibling_control) == 1);
	CHECK(!finish(&child, 7890));
	/* The representative native context can die before Linux detaches it.
	 * Reset must still revoke the live MM, not report death as completion.
	 */
	CHECK(!kill(sibling.pid, SIGKILL));
	CHECK(!kobox_posix_vm_reset(&parent, base, 4096));
	CHECK(!sibling.pid);
	CHECK(!access_start(&parent, base, 0, 0));
	CHECK(!kobox_posix_vm_wait(&parent, false, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base);
	CHECK(!kobox_posix_vm_map(&child, base, 4096, 4096, rw));
	CHECK(!kobox_posix_vm_resume(&parent));
	CHECK(!access_done(&parent, 0xabcdef));
	CHECK(!kobox_posix_vm_destroy(&sibling));
	CHECK(mapped(parent.pid, sibling_control) == 0 &&
	      mapped(parent.pid, child_control) == 1);
	CHECK(!kobox_posix_vm_destroy(&parent));
	CHECK(mapped(child.pid, parent_control) == 0 && mapped(child.pid, child_control) == 1);
	CHECK(!kobox_posix_vm_reset(&child, base, 4096));
	CHECK(!access_start(&child, base, 8, 0x76543210));
	CHECK(!kobox_posix_vm_wait(&child, false, &event));
	CHECK(event.kind == KOBOX_POSIX_VM_FAULT && event.address == base &&
	      event.sp == base + 8 && child.control->fault_address == base);
	CHECK(!kobox_posix_vm_map(&child, base, 4096, 4096, rw));
	CHECK(!kobox_posix_vm_resume(&child));
	CHECK(!access_done(&child, 0x76543210));
	CHECK(!kobox_posix_vm_destroy(&child));
	CHECK(!munmap(direct, ram.size));
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	return 0;
}

struct death_race {
	pthread_barrier_t start;
	atomic_bool done;
	pid_t owner, parent, bystander;
	bool after_birth;
	int result;
};

static void *kill_parent(void *argument)
{
	struct death_race *race = argument;
	bool extra = false;
	int result = pthread_barrier_wait(&race->start);

	if (result && result != PTHREAD_BARRIER_SERIAL_THREAD)
		__builtin_trap();
	while (race->after_birth && !atomic_load(&race->done)) {
		if (native_child_count(race->owner, race->parent, race->bystander, &extra) < 0) {
			race->result = EIO;
			return NULL;
		}
		if (extra)
			break;
		sched_yield();
	}
	if (kill(race->parent, SIGKILL) && errno != ESRCH)
		race->result = errno;
	return NULL;
}

static int parent_death(const char *client, bool share_mm)
{
	struct kobox_posix_memory_backing ram = {0};
	struct kobox_posix_vm bystander = {0};
	unsigned int index, failed = 0;
	bool extra;
	int status;

	CHECK(!kobox_posix_memory_backing_init(&ram, 8192));
	CHECK(!kobox_posix_vm_create(&bystander, client, &ram,
		KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
	for (index = 0; index < 32; index++) {
		struct kobox_posix_vm parent = {0}, child = {0};
		struct kobox_posix_vm_event event;
		struct kobox_x86_user_regs registers;
		struct death_race race = {.owner = syscall(SYS_gettid),
			.bystander = bystander.pid, .after_birth = index & 1};
		pthread_t killer;
		int result;

		CHECK(!kobox_posix_vm_create(&parent, client, &ram,
			KOBOX_VM_TEST_WINDOW_BASE, KOBOX_VM_TEST_WINDOW_SIZE));
		CHECK(!kobox_posix_vm_enable_syscalls(&parent));
		CHECK(!begin(&parent, &event));
		race.parent = parent.pid;
		CHECK(!pthread_barrier_init(&race.start, NULL, 2));
		atomic_init(&race.done, false);
		CHECK(!pthread_create(&killer, NULL, kill_parent, &race));
		result = pthread_barrier_wait(&race.start);
		CHECK(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
		result = kobox_posix_vm_clone(&parent, event.syscall.sequence, share_mm, &child);
		atomic_store(&race.done, true);
		CHECK(!pthread_join(killer, NULL) && !race.result);
		CHECK(!pthread_barrier_destroy(&race.start));
		if (result) {
			failed++;
			CHECK(!child.pid && !child.control && !child.control_backing.initialized);
		} else {
			CHECK(!kobox_posix_vm_destroy(&child));
		}
		CHECK(!kobox_posix_vm_destroy(&parent));
		CHECK(native_child_count(race.owner, 0, bystander.pid, &extra) == 1 && !extra);
		CHECK(!kobox_posix_vm_read_registers(&bystander, &registers));
	}
	CHECK(failed);
	CHECK(!kobox_posix_vm_destroy(&bystander));
	CHECK(!kobox_posix_memory_backing_destroy(&ram));
	errno = 0;
	CHECK(waitpid(-1, &status, __WALL | WNOHANG) == -1 && errno == ECHILD);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2 && !fork_context(argv[1]) && !shared_contexts(argv[1]) &&
	      !parent_death(argv[1], false) && !parent_death(argv[1], true));
	puts("POSIX private/shared context clone passed (not the Linux clone/FD Gate)");
	return 0;
}
