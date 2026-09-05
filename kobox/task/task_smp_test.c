// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "host.h"
#include "../host/posix/host.h"

#include <dlfcn.h>
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

static struct kobox_posix_cpu cpus[KOBOX_LINUX_MEMORY_LOGICAL_CPUS];
static kobox_linux_task_notification_fn task_dispatch;
static uintptr_t task_core_base;

static int host_map(void *window, size_t window_offset, void *backing,
		    size_t backing_offset, size_t size,
		    unsigned int protection, void **address_out)
{
	unsigned int native = 0;
	uint64_t mask;
	int status;
	int restore_status;

	if (protection & KOBOX_LINUX_MEMORY_READ)
		native |= KOBOX_POSIX_MEMORY_READ;
	if (protection & KOBOX_LINUX_MEMORY_WRITE)
		native |= KOBOX_POSIX_MEMORY_WRITE;
	status = kobox_posix_notifications_save(&mask);
	if (status)
		return status;
	status = kobox_posix_memory_window_map(window, window_offset, backing,
		backing_offset, size, native, address_out);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

static int host_reset(void *window, size_t window_offset, size_t size)
{
	uint64_t mask;
	int status = kobox_posix_notifications_save(&mask);
	int restore_status;

	if (status)
		return status;
	status = kobox_posix_memory_window_reset(window, window_offset, size);
	restore_status = kobox_posix_notifications_restore(mask);
	return status ? status : restore_status;
}

static int host_task_bind_current(void **task_out)
{
	return kobox_posix_task_bind_current(
		(struct kobox_posix_task **)task_out);
}

static int host_task_create(
	void **task_out,
	void *(*entry)(void *),
	void *argument)
{
	return kobox_posix_task_start(
		(struct kobox_posix_task **)task_out, entry, argument);
}

static int host_task_wake(void *task)
{
	return kobox_posix_task_wake(task);
}

static int host_task_park(void *task)
{
	return kobox_posix_task_park(task);
}

static int host_task_join_destroy(void *task)
{
	return kobox_posix_task_join_destroy(task);
}

static int host_task_destroy_current(void *task)
{
	return kobox_posix_task_destroy_current(task);
}

static int host_cpu_enter(uint32_t cpu, void *task)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_enter_task(&cpus[cpu], task);
}

static int host_cpu_leave(uint32_t cpu)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_leave(&cpus[cpu]);
}

static int host_cpu_switch(
	uint32_t cpu,
	void *previous_task,
	void *next_task,
	uint8_t exiting)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_switch(
		&cpus[cpu], previous_task, next_task, exiting != 0);
}

static int host_cpu_wait(
	uint32_t cpu,
	uint64_t observed_sequence,
	uint64_t *sequence_out)
{
	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	return kobox_posix_cpu_wait(
		&cpus[cpu], observed_sequence, sequence_out);
}

static int host_cpu_notify(
	uint32_t cpu,
	enum kobox_linux_task_notification notification)
{
	enum kobox_posix_notification native;

	if (cpu >= KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -1;
	native = notification == KOBOX_LINUX_TASK_RESCHEDULE ?
		KOBOX_POSIX_NOTIFICATION_IRQ : KOBOX_POSIX_NOTIFICATION_TICK;
	return kobox_posix_cpu_notify(&cpus[cpu], native);
}

static int host_cpu_irq_disable(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_irq_disable(&cpus[cpu]) : -1;
}

static int host_cpu_irq_enable(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_irq_enable(&cpus[cpu]) : -1;
}

static uint8_t host_cpu_irq_disabled(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS &&
		kobox_posix_cpu_irq_disabled(&cpus[cpu]);
}

static uint64_t host_cpu_notification_sequence(uint32_t cpu)
{
	return cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS ?
		kobox_posix_cpu_notification_sequence(&cpus[cpu]) : 0;
}

static void host_notification(
	void *context,
	uint32_t cpu,
	enum kobox_posix_notification notification,
	uint64_t count)
{
	enum kobox_linux_task_notification translated =
		notification == KOBOX_POSIX_NOTIFICATION_IRQ ?
		KOBOX_LINUX_TASK_RESCHEDULE : KOBOX_LINUX_TASK_CALL_FUNCTION;

	(void)context;
	task_dispatch(cpu, translated, count);
}

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
	static const struct kobox_linux_memory_host_operations memory_operations = {
		.size = sizeof(memory_operations),
		.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
		.map = host_map,
		.reset = host_reset,
	};
	static const struct kobox_linux_task_host_operations task_operations = {
		.size = sizeof(task_operations),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
		.task_bind_current = host_task_bind_current,
		.task_create = host_task_create,
		.task_wake = host_task_wake,
		.task_park = host_task_park,
		.task_join_destroy = host_task_join_destroy,
		.task_destroy_current = host_task_destroy_current,
		.task_exit = kobox_posix_task_exit,
		.cpu_enter = host_cpu_enter,
		.cpu_leave = host_cpu_leave,
		.cpu_switch = host_cpu_switch,
		.cpu_wait = host_cpu_wait,
		.cpu_notify = host_cpu_notify,
		.cpu_irq_disable = host_cpu_irq_disable,
		.cpu_irq_enable = host_cpu_irq_enable,
		.notifications_save = kobox_posix_notifications_save,
		.notifications_restore = kobox_posix_notifications_restore,
		.cpu_irq_disabled = host_cpu_irq_disabled,
		.cpu_notification_sequence = host_cpu_notification_sequence,
		.monotonic_ns = kobox_posix_monotonic_ns,
		.realtime_ns = kobox_posix_realtime_ns,
	};
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
	for (cpu = 0; cpu < KOBOX_LINUX_MEMORY_LOGICAL_CPUS; cpu++)
		CHECK(kobox_posix_cpu_init(
			&cpus[cpu], cpu, host_notification, NULL) == 0);
	CHECK(kobox_posix_task_bind_current(&boot_task) == 0);
	CHECK(kobox_posix_cpu_enter_task(&cpus[0], boot_task) == 0);
	CHECK(kobox_posix_cpu_irq_disable(&cpus[0]) == 0);

	layout = (struct kobox_linux_task_layout) {
		.size = sizeof(layout),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
		.memory = {
			.size = sizeof(layout.memory),
			.identity = KOBOX_LINUX_MEMORY_HOST_IDENTITY,
			.operations = &memory_operations,
			.ram_backing = &backing,
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
		.operations = &task_operations,
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
	CHECK(kobox_posix_cpu_leave(&cpus[0]) == 0);
	CHECK(kobox_posix_task_destroy_current(boot_task) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[1]) == 0);
	CHECK(kobox_posix_cpu_destroy(&cpus[0]) == 0);
	return 0;
}
