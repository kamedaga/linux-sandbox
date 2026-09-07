// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "posix_machine.h"
#include "../host/posix/host.h"

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define TEST_RAM_SIZE (256UL << 20)
#define TEST_VMEMMAP_SIZE (16UL << 20)
#define TEST_VMALLOC_SIZE (256UL << 20)
#define TEST_KERNEL_PHYSICAL_BASE (16UL << 20)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static kobox_linux_task_notification_fn task_dispatch;
static uintptr_t task_core_base;

static void crash_handler(int signal_number, siginfo_t *info, void *argument)
{
	ucontext_t *context = argument;
	void *frames[48];
	char message[160];
	int count;
	int size = snprintf(message, sizeof(message),
		"task gate signal %d: core+%#llx sp=%#llx address=%p\n",
		signal_number,
		(unsigned long long)context->uc_mcontext.gregs[REG_RIP] -
		(unsigned long long)task_core_base,
		(unsigned long long)context->uc_mcontext.gregs[REG_RSP],
		info->si_addr);

	if (size > 0)
		(void)write(STDERR_FILENO, message, (size_t)size);
	count = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	_exit(128 + signal_number);
}

static int install_crash_handlers(void)
{
	static unsigned char diagnostic_stack[64U << 10];
	stack_t stack = {
		.ss_sp = diagnostic_stack,
		.ss_size = sizeof(diagnostic_stack),
	};
	struct sigaction action = {
		.sa_sigaction = crash_handler,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

	sigemptyset(&action.sa_mask);
	return sigaltstack(&stack, NULL) || sigaction(SIGBUS, &action, NULL) ||
		sigaction(SIGILL, &action, NULL) ||
		sigaction(SIGSEGV, &action, NULL);
}

static int load_function(void *handle, const char *name, void *function_out)
{
	void *address;

	dlerror();
	address = dlsym(handle, name);
	if (dlerror() || !address)
		return -1;
	memcpy(function_out, &address, sizeof(address));
	return 0;
}

static int check_phase_boundary(void *handle)
{
	typedef void (*boundary_fn)(void);
	boundary_fn boundary;
	pid_t child;
	int status;

	CHECK(load_function(
		handle, "try_to_free_pages", &boundary) == 0);
	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		(void)close(STDERR_FILENO);
		boundary();
		_exit(125);
	}
	if (waitpid(child, &status, 0) != child)
		return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 126 ? 0 : -1;
}

int main(int argument_count, char **arguments)
{
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window direct = {0};
	struct kobox_posix_memory_window vmemmap = {0};
	struct kobox_posix_memory_window vmalloc = {0};
	struct kobox_posix_task *boot_task = NULL;
	struct kobox_linux_task_report report = {
		.size = sizeof(report),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
	};
	struct kobox_linux_task_layout layout;
	kobox_linux_task_boot_fn entry;
	void *direct_address;
	void *boundary_handle;
	void *handle;
	unsigned int cpu;

	CHECK(argument_count == 3);
	CHECK(install_crash_handlers() == 0);
	CHECK(kobox_posix_memory_backing_init(&backing, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&direct, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmemmap, TEST_VMEMMAP_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmalloc, TEST_VMALLOC_SIZE) == 0);
	CHECK(kobox_posix_memory_window_map(
		&direct, 0, &backing, 0, TEST_RAM_SIZE,
		KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		&direct_address) == 0);

	boundary_handle = dlopen(arguments[1], RTLD_NOW | RTLD_GLOBAL);
	if (!boundary_handle) {
		fprintf(stderr, "cannot load task phase boundary: %s\n", dlerror());
		return 1;
	}
	CHECK(check_phase_boundary(boundary_handle) == 0);
	handle = dlopen(arguments[2], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "cannot load Linux task core: %s\n", dlerror());
		return 1;
	}
	CHECK(load_function(handle, "kobox_linux_task_smp_boot", &entry) == 0);
	{
		Dl_info info;
		void *address;

		memcpy(&address, &entry, sizeof(address));
		CHECK(dladdr(address, &info) != 0);
		task_core_base = (uintptr_t)info.dli_fbase;
	}
	CHECK(load_function(
		handle, "kobox_linux_task_dispatch", &task_dispatch) == 0);
	CHECK(kobox_task_posix_init(task_dispatch) == 0);
	CHECK(kobox_posix_task_bind_current(&boot_task) == 0);
	CHECK(kobox_task_posix_operations.cpu_enter(0, boot_task) == 0);
	CHECK(kobox_task_posix_operations.cpu_irq_disable(0) == 0);

	layout = (struct kobox_linux_task_layout) {
		.size = sizeof(layout),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
		.memory = {
			.size = sizeof(layout.memory),
			.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
			.operations = &kobox_task_posix_memory_operations,
			.ram_backing = &backing,
			.direct_window = &direct,
			.vmemmap_window = &vmemmap,
			.vmalloc_window = &vmalloc,
			.direct_map = direct_address,
			.ram_size = TEST_RAM_SIZE,
			.vmemmap_base = vmemmap.address,
			.vmemmap_size = vmemmap.size,
			.vmalloc_base = vmalloc.address,
			.vmalloc_size = vmalloc.size,
			.kernel_image_physical_base = TEST_KERNEL_PHYSICAL_BASE,
		},
		.operations = &kobox_task_posix_operations,
		.boot_task = boot_task,
	};
	{
		int status = entry(&layout, &report);

		if (status)
			fprintf(stderr, "task gate returned %d (switches=%llu, IPI=%llu)\n",
				status, (unsigned long long)report.context_switches,
				(unsigned long long)report.remote_reschedule_ipis);
		CHECK(status == 0);
	}
	CHECK(report.upstream_schedule_ready);
	CHECK(report.upstream_try_to_wake_up_ready);
	CHECK(report.current_percpu_ready);
	CHECK(report.logical_cpu_count == KOBOX_LINUX_MEMORY_LOGICAL_CPUS);
	CHECK(report.context_switches != 0);
	CHECK(report.remote_reschedule_ipis != 0);
	CHECK(report.local_switch_ready);
	CHECK(report.remote_switch_ready);
	CHECK(report.migration_ready);
	CHECK(report.affinity_ready);
	CHECK(report.preempt_disable_ready);
	CHECK(report.irq_disable_ready);
	CHECK(report.exit_join_ready);
	for (cpu = 0; cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS; cpu++) {
		CHECK(report.hardirq_entries[cpu]);
		CHECK(report.idle_irq_entries[cpu]);
		CHECK(report.idle_entries[cpu]);
		CHECK(report.idle_exits[cpu]);
		CHECK(report.high_resolution_ready[cpu]);
		CHECK(report.tick_progress[cpu]);
	}
	CHECK(kobox_task_posix_operations.cpu_leave(0) == 0);
	CHECK(kobox_posix_task_destroy_current(boot_task) == 0);
	CHECK(kobox_task_posix_destroy() == 0);
	return 0;
}
