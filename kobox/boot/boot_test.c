// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "host.h"
#include "image.h"
#include "service_gate.h"
#include "wait_gate.h"
#include "rcu_gate.h"
#include "workqueue_gate.h"
#include "cleanup_gate.h"
#include "memory_gate.h"
#include "vfs_gate.h"
#include "shmem_gate.h"
#include "pressure_gate.h"
#include "vm_gate.h"
#include "module_gate.h"
#include "../mm/posix.h"
#include "../host/posix/vm_service.h"
#include "../task/posix_machine.h"
#include "../host/posix/host.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define TEST_RAM_SIZE (256UL << 20)
#define TEST_VMEMMAP_SIZE (16UL << 20)
#define TEST_VMALLOC_SIZE (256UL << 20)
#define TEST_IMAGE_PHYSICAL_BASE (16UL << 20)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "boot check failed at line %d: %s\n", \
			__LINE__, #expression); \
		return 1; \
	} \
} while (0)

static uintptr_t core_base;
static size_t core_size;
static int diagnostic_memory = -1;
static int (*verify_boot)(struct kobox_linux_boot_report *report);
static int (*verify_wait)(struct kobox_linux_wait_report *report);
static int (*verify_rcu)(struct kobox_linux_rcu_report *report);
static int (*verify_workqueue)(struct kobox_linux_workqueue_report *report);
static int (*verify_cleanup)(struct kobox_linux_cleanup_report *report);
static int (*verify_memory)(struct kobox_linux_boot_memory_report *report);
static int (*verify_vfs)(struct kobox_linux_vfs_report *report);
static int (*verify_shmem)(struct kobox_linux_shmem_report *report);
static int (*verify_pressure)(struct kobox_linux_pressure_report *report);
static int (*probe_vm)(const struct kobox_linux_vm_test *host, struct kobox_linux_vm_report *report);
static struct kobox_linux_vm_test vm_host;
static struct kobox_linux_module_test module_host = {.size = sizeof(module_host)};

static int module_access(void *address, enum kobox_linux_module_access operation)
{
	uint64_t notification_mask;
	int status, result = KOBOX_MODULE_ACCESS_FAILED;
	pid_t child, waited;

	if (!address || operation < KOBOX_MODULE_READ || operation > KOBOX_MODULE_EXECUTE ||
	    kobox_posix_notifications_save(&notification_mask))
		return result;
	child = fork();
	if (!child) {
		struct sigaction action = {.sa_handler = SIG_DFL};
		volatile unsigned long *pointer = address;
		unsigned long value;
		int (*execute)(void);

		/* No hosted Linux entry or non-async-safe library call in this child.
		 * Notifications remain masked; only the inherited mapping is probed.
		 */
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL) ||
		    sigaction(SIGILL, &action, NULL) || prctl(PR_SET_DUMPABLE, 0))
			_exit(125);
		if (operation == KOBOX_MODULE_EXECUTE) {
			memcpy(&execute, &address, sizeof(execute));
			(void)execute();
		} else {
			value = *pointer;
			if (operation == KOBOX_MODULE_WRITE)
				*pointer = value;
		}
		_exit(0);
	}
	if (child > 0) {
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited == child) {
			if (WIFEXITED(status) && !WEXITSTATUS(status))
				result = KOBOX_MODULE_ACCESS_ALLOWED;
			else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV)
				result = KOBOX_MODULE_ACCESS_DENIED;
		}
	}
	if (kobox_posix_notifications_restore(notification_mask))
		__builtin_trap();
	return result;
}
static int (*probe_modules)(const struct kobox_linux_module_test *,
			    struct kobox_linux_module_report *);

static int read_module_image(const char *path, struct kobox_linux_module_image *image)
{
	struct stat information;
	void *mapping;
	int saved_error;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return errno;
	if (fstat(fd, &information) || !S_ISREG(information.st_mode) ||
	    information.st_size <= 0 || (uint64_t)information.st_size > SIZE_MAX) {
		close(fd);
		return EINVAL;
	}
	mapping = mmap(NULL, information.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	saved_error = errno;
	close(fd);
	if (mapping == MAP_FAILED)
		return saved_error;
	image->data = mapping;
	image->length = information.st_size;
	return 0;
}

static int vm_terminate(uint64_t pid)
{
	if (!pid || (pid != vm_host.pids[0] && pid != vm_host.pids[1]))
		return -EINVAL;
	/* Do not close/reap here: the service must observe unsolicited death. */
	return kill((pid_t)pid, SIGKILL) ? -errno : 0;
}

static int vm_notify(uint32_t cpu)
{
	static const char checkpoint[] = "MM race: publication IRQ queued\n";
	int result = kobox_task_posix_operations.cpu_notify(cpu,
						KOBOX_LINUX_TASK_VM_EVENT);

	if (!result)
		(void)write(STDERR_FILENO, checkpoint, sizeof(checkpoint) - 1);
	return -result;
}

static char *format_hex(char *output, uintptr_t value)
{
	unsigned int shift = sizeof(value) * 8;

	while (shift) {
		shift -= 4;
		*output++ = "0123456789abcdef"[(value >> shift) & 15];
	}
	return output;
}

static void crash_handler(int number, siginfo_t *info, void *argument)
{
	ucontext_t *context = argument;
	uintptr_t stack[128];
	unsigned int index;
	ssize_t length;
	char message[128] = "boot fault: core+";
	char *position = message + sizeof("boot fault: core+") - 1;

	position = format_hex(position, context->uc_mcontext.gregs[REG_RIP] -
			      core_base);
	*position++ = ' ';
	position = format_hex(position, (uintptr_t)info->si_addr);
	*position++ = '\n';
	(void)write(STDERR_FILENO, message, position - message);
	/* pread is async-signal-safe and reports EFAULT/EIO instead of causing
	 * a second fault while inspecting a possibly corrupt native stack.
	 */
	length = pread(diagnostic_memory, stack, sizeof(stack),
		       context->uc_mcontext.gregs[REG_RSP]);
	for (index = 0; length > 0 && index < (size_t)length / sizeof(*stack);
	     index++) {
		if (stack[index] < core_base || stack[index] - core_base >= core_size)
			continue;
		position = format_hex(message, stack[index] - core_base);
		*position++ = '\n';
		(void)write(STDERR_FILENO, message, position - message);
	}
	_exit(128 + number);
}

static void console_write(void *context, const char *text, size_t length)
{
	(void)context;
	(void)write(STDERR_FILENO, text, length);
}

static void kernel_main(void *argument)
{
	struct kobox_linux_boot_report report = {.size = sizeof(report)};
	struct kobox_linux_wait_report wait = {.size = sizeof(wait)};
	struct kobox_linux_rcu_report rcu = {.size = sizeof(rcu)};
	struct kobox_linux_workqueue_report wq = {.size = sizeof(wq)};
	struct kobox_linux_cleanup_report cleanup = {.size = sizeof(cleanup)};
	struct kobox_linux_boot_memory_report memory = {.size = sizeof(memory)};
	struct kobox_linux_vfs_report vfs = {.size = sizeof(vfs)};
	struct kobox_linux_shmem_report shmem = {.size = sizeof(shmem)};
	struct kobox_linux_pressure_report pressure = {.size = sizeof(pressure)};
	struct kobox_linux_vm_report vm = {.size = sizeof(vm)};
	struct kobox_linux_task_report *task = argument;
	char message[256];
	uint64_t notification_mask;
	int status, length;

	status = verify_boot(&report);
	/* libc formatting must not be interrupted by a Linux callback. */
	if (kobox_posix_notifications_save(&notification_mask))
		__builtin_trap();
	length = snprintf(message, sizeof(message),
		"boot service Gate: status=%d phase=%u warnings=%llu\n",
		status, report.phase, (unsigned long long)report.warnings);
	if (length > 0 && (size_t)length < sizeof(message))
		(void)write(STDERR_FILENO, message, length);
	if (status) {
		length = snprintf(message, sizeof(message),
			"boot softirq diagnostics: irq=%u/%u tasklet=%u/%u irq_exit=%u/%u ksoftirqd=%u/%u\n",
			report.irq_callbacks[0], report.irq_callbacks[1],
			report.tasklet_runs[0], report.tasklet_runs[1],
			report.irq_exit_tasklet_runs[0], report.irq_exit_tasklet_runs[1],
			report.ksoftirqd_runs[0], report.ksoftirqd_runs[1]);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
	}
	length = snprintf(message, sizeof(message),
		"SMP: current=%u local=%u remote=%u migrate=%u affinity=%u preempt=%u irq=%u join=%u hres=%u/%u\n",
		task->current_percpu_ready, task->local_switch_ready,
		task->remote_switch_ready, task->migration_ready,
		task->affinity_ready, task->preempt_disable_ready,
		task->irq_disable_ready, task->exit_join_ready,
		task->high_resolution_ready[0], task->high_resolution_ready[1]);
	if (length > 0 && (size_t)length < sizeof(message))
		(void)write(STDERR_FILENO, message, length);
	if (kobox_posix_notifications_restore(notification_mask))
		__builtin_trap();
	if (!status) {
		status = verify_memory(&memory);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"boot memory Gate: status=%d pages=%u heap=%u slab=%u percpu=%u dynamic=%u alias=%u shmem=%u kswapd=%u warnings=%llu phase=%u cpu=%u line=%u parent_permissions=%u\n",
			status, memory.page_cases, memory.heap_cases, memory.slab_cases,
			memory.percpu_cases, memory.dynamic_vmap_allocations, memory.alias_cases,
			memory.shmem_cases, memory.kswapd_ready,
			(unsigned long long)memory.warnings, memory.phase, memory.cpu,
			memory.line, memory.parent_permission_cases);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_vfs) {
		status = verify_vfs(&vfs);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"VFS Gate: status=%d cases=%u mounts=%u io=%u reopen=%u negative=%u task_work=%u delayed_fput=%u rcu=%u file=%u inode=%u folio=%u super=%u warnings=%llu\n",
			status, vfs.cases, vfs.mounts, vfs.io_checks, vfs.linked_reopens, vfs.negative_checks,
			vfs.task_work_cases, vfs.delayed_fput_cases, vfs.rcu_holds,
			vfs.file_reclaims, vfs.inode_reclaims, vfs.folio_reclaims,
			vfs.super_reclaims, (unsigned long long)vfs.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"VFS diagnostics: cpu=%u deferred=%u unlink_first=%u phase=%u line=%u result=%d\n",
				vfs.cpu, vfs.deferred, vfs.unlink_first,
				vfs.phase, vfs.line, vfs.result);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_shmem) {
		status = verify_shmem(&shmem);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"shmem Gate: status=%d cases=%u io=%u accounting=%u sharing=%u negative=%u increments=%u races=%u locked=%u reclaimed=%u rollbacks=%u warnings=%llu\n",
			status, shmem.cases, shmem.io_checks, shmem.accounting_checks,
			shmem.sharing_checks, shmem.negative_checks, shmem.increments,
			shmem.race_rounds, shmem.locked_waits, shmem.page_reclaims,
			shmem.rollbacks, (unsigned long long)shmem.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"shmem diagnostics: cpu=%u noreserve=%u phase=%u line=%u result=%d\n",
				shmem.cpu, shmem.noreserve, shmem.phase, shmem.line, shmem.result);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_pressure) {
		status = verify_pressure(&pressure);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"RAM pressure: status=%d phase=%u line=%u result=%d pages=%u kswapd=%u direct=%u reuse=%u dcache=%u retained=%u failures=%u recovered=%u warnings=%llu\n",
			status, pressure.phase, pressure.line, pressure.result,
			pressure.pressure_pages, pressure.kswapd_freed,
			pressure.direct_freed, pressure.cache_reused,
			pressure.dentries_freed, pressure.retained,
			pressure.failures, pressure.recovered,
			(unsigned long long)pressure.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		length = snprintf(message, sizeof(message),
			"Allocation rollback: case=%u nth=%u injected=%u rollbacks=%u sweeps=%u\n",
			pressure.allocation_case, pressure.fail_nth, pressure.injected,
			pressure.rollbacks, pressure.sweeps);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && probe_vm) {
		status = probe_vm(&vm_host, &vm);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"MM/VMA case: status=%d faults=%u accesses=%u mm=%u signals=%u warnings=%llu phase=%u line=%u result=%d\n",
			status, vm.faults, vm.accesses, vm.mm_checks, vm.signals,
			(unsigned long long)vm.warnings, vm.phase, vm.line, vm.result);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		length = snprintf(message, sizeof(message),
			"MM invalidation/reuse: held_allocations=%u reclaimed=%u revoked_aliases=%u\n",
			vm.held_allocations, vm.reclaimed_pages, vm.revoked_aliases);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		length = snprintf(message, sizeof(message),
			"MM publication races: overlap=%u irqs=%u stale_resumes=%u dead_spaces=%u delayed_faults=%u\n",
			vm.publication_races, vm.publication_irqs,
			vm.stale_resumes, vm.dead_spaces, vm.delayed_faults);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (vm.pressure.size) {
			length = snprintf(message, sizeof(message),
				"MM cleanup pressure: phase=%u line=%u result=%d direct=%u kswapd=%u reused=%u target_pfn=%llu flags=%llx type=%x refs=%d migrate=%u free=%u min=%u search=%u slab_size=%u cache_match=%u\n",
				vm.pressure.phase, vm.pressure.line, vm.pressure.result,
				vm.pressure.direct_freed, vm.pressure.kswapd_freed,
				vm.pressure.cache_reused, (unsigned long long)vm.target_pfn,
				(unsigned long long)vm.target_flags, vm.target_type, vm.target_refs,
				vm.target_migrate, vm.target_free, vm.target_min, vm.search_pages,
				vm.target_slab_size, vm.target_cache_match);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		length = snprintf(message, sizeof(message),
			"MM lifetime: exits=%u rollback=%u mm_drains=%u task_drains=%u mm_reclaims=%u task_reclaims=%u file=%u inode=%u folio=%u large_alias_pages=%u buddy_slots=%u\n",
			vm.async_exits, vm.rollbacks, vm.mm_drains, vm.task_drains,
			vm.mm_reclaims, vm.task_reclaims, vm.file_reclaims,
			vm.inode_reclaims, vm.folio_reclaims, vm.large_alias_pages,
			vm.buddy_slot_reclaims);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && probe_modules) {
		struct kobox_linux_module_report modules = {.size = sizeof(modules)};

		status = probe_modules(&module_host, &modules);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"Native module lifecycle: status=%d exports=%u permissions=%u loaded=%u unloaded=%u warnings=%llu phase=%u result=%d\n",
			status, modules.exports, modules.permissions, modules.loaded, modules.unloaded,
			(unsigned long long)modules.warnings, modules.phase, modules.result);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (module_host.vm) {
			length = snprintf(message, sizeof(message),
				"GEM lifetime: phase=%u line=%u result=%d files=%u handles=%u pins=%u vmaps=%u mappings=%u accesses=%u faults=%u denied=%u partial=%u reclaimed_objects=%u reclaimed_pages=%u live=%u revoked=%u\n",
				modules.gem.phase, modules.gem.line, modules.gem.result,
				modules.gem.files, modules.gem.handles, modules.gem.pins,
				modules.gem.vmaps, modules.gem.mappings, modules.gem.accesses,
				modules.gem.faults, modules.gem.denied, modules.gem.partial_unmaps,
				modules.gem.object_reclaims, modules.gem.page_reclaims,
				modules.gem.live_checks, modules.gem.revoked);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (module_host.allocation_failures) {
			length = snprintf(message, sizeof(message),
				"GEM allocation: stage=%u nth=%u injected=%u rollbacks=%u recoveries=%u sweeps=%u pressure_accesses=%u fault_retries=%u pgtables_reclaimed=%u close_faults=%u cleanup_rollbacks=%u pressure_unmaps=%u deaths=%u aborted_faults=%u deferred=%u rejected_work=%u\n",
				modules.gem.allocation_stage, modules.gem.fail_nth,
				modules.gem.injected, modules.gem.rollbacks,
				modules.gem.recoveries, modules.gem.sweeps,
				modules.gem.pressure_accesses, modules.gem.fault_retries,
				modules.gem.page_table_reclaims, modules.gem.close_faults,
				modules.gem.cleanup_rollbacks,
				modules.gem.pressure_unmaps,
				modules.gem.client_deaths, modules.gem.aborted_faults,
				modules.gem.deferred_callbacks, modules.gem.rejected_work);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
			length = snprintf(message, sizeof(message),
				"GEM RAM pressure: phase=%u line=%u result=%d kswapd=%u direct=%u reused=%u\n",
				modules.gem.pressure.phase, modules.gem.pressure.line,
				modules.gem.pressure.result, modules.gem.pressure.kswapd_freed,
				modules.gem.pressure.direct_freed, modules.gem.pressure.cache_reused);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
			length = snprintf(message, sizeof(message),
				"GEM task cleanup: mm_drains=%u task_drains=%u mm_reclaims=%u task_reclaims=%u phase=%u line=%u\n",
				modules.gem.task_lifetime.mm_drains, modules.gem.task_lifetime.task_drains,
				modules.gem.task_lifetime.mm_reclaims, modules.gem.task_lifetime.task_reclaims,
				modules.gem.task_lifetime.phase, modules.gem.task_lifetime.line);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (status)
			(void)write(STDERR_FILENO, modules.diagnostics,
				strnlen(modules.diagnostics, sizeof(modules.diagnostics)));
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_wait) {
		status = verify_wait(&wait);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"timed wait Gate: status=%d passed=%u irq=%u warnings=%llu cpu=%u api=%s scenario=%s line=%u result=%lld elapsed_ns=%llu\n",
			status, wait.passed, wait.irq_callbacks,
			(unsigned long long)wait.warnings, wait.cpu, wait.api,
			wait.scenario, wait.failure_line, (long long)wait.result,
			(unsigned long long)wait.elapsed_ns);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"wait diagnostics: switches=%llu/%llu invalid=%u\n",
				(unsigned long long)wait.expected_switches,
				(unsigned long long)wait.observed_switches,
				wait.invalid);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_rcu) {
		status = verify_rcu(&rcu);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"RCU Gate: status=%d passed=%u normal=%u expedited=%u callbacks=%u probes=%u freed=%u preempt=%u migrate=%u idle=%u warnings=%llu\n",
			status, rcu.passed, rcu.normal_gps, rcu.expedited_gps,
			rcu.callbacks, rcu.barrier_probes, rcu.reclaimed,
			rcu.preemptions, rcu.migrations, rcu.idle_observations,
			(unsigned long long)rcu.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"RCU diagnostics: flavor=%s scenario=%s expedited=%u cpu=%u phase=%u line=%u errors=%u\n",
				rcu.flavor, rcu.scenario, rcu.expedited, rcu.cpu,
				rcu.phase, rcu.failure_line, rcu.errors);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_workqueue) {
		status = verify_workqueue(&wq);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"workqueue Gate: status=%d cases=%u callbacks=%u rescued=%u pages=%u warnings=%llu\n",
			status, wq.cases, wq.callbacks, wq.rescued, wq.pressure_pages,
			(unsigned long long)wq.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"workqueue diagnostics: queue=%s scenario=%s cpu=%u phase=%u line=%u errors=%u\n",
				wq.queue, wq.scenario, wq.cpu, wq.phase, wq.line, wq.errors);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_cleanup) {
		status = verify_cleanup(&cleanup);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"cleanup Gate: status=%d cases=%u callbacks=%u rejected=%u probes=%u freed=%u warnings=%llu\n",
			status, cleanup.cases, cleanup.callbacks, cleanup.rejected,
			cleanup.probes, cleanup.freed, (unsigned long long)cleanup.warnings);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (status) {
			length = snprintf(message, sizeof(message),
				"cleanup diagnostics: scenario=%s cpu=%u phase=%u line=%u errors=%u\n",
				cleanup.scenario, cleanup.cpu, cleanup.phase,
				cleanup.line, cleanup.errors);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	_exit(status ? 1 : 0);
}

int main(int argc, char **argv)
{
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_memory_window direct = {0};
	struct kobox_posix_memory_window vmemmap = {0};
	struct kobox_posix_memory_window vmalloc = {0};
	struct kobox_boot_image image = {0};
	struct kobox_posix_vm_service *vm_service = NULL;
	struct kobox_posix_task *boot_task = NULL;
	struct kobox_linux_task_report report = {
		.size = sizeof(report),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
	};
	struct kobox_linux_boot_layout layout;
	struct sigaction action = {
		.sa_sigaction = crash_handler,
		.sa_flags = SA_SIGINFO,
	};
	int (*entry)(const struct kobox_linux_boot_layout *layout,
		     struct kobox_linux_task_report *report);
	kobox_linux_task_notification_fn dispatch;
	void *direct_address;
	void *address;
	void *handle;
	Dl_info info;
	int status;

	CHECK(argc == 2 || (argc == 7 && !strcmp(argv[2], "--modules")) ||
	      (argc == 9 && (!strcmp(argv[2], "--gem") ||
			    !strcmp(argv[2], "--gem-object-last") ||
			    !strcmp(argv[2], "--gem-failure") ||
			    !strcmp(argv[2], "--gem-cleanup") ||
			    !strcmp(argv[2], "--gem-cleanup-object-last") ||
			    !strcmp(argv[2], "--gem-cleanup-death") ||
			    !strcmp(argv[2], "--gem-cleanup-death-object-last") ||
			    !strcmp(argv[2], "--gem-failure-object-last"))) ||
	      (argc == 4 && (!strcmp(argv[2], "--vm-probe") ||
					!strcmp(argv[2], "--vm-probe-ro") ||
					!strcmp(argv[2], "--vm-probe-reuse") ||
					!strcmp(argv[2], "--vm-probe-irq") ||
					!strcmp(argv[2], "--vm-probe-truncate") ||
					!strcmp(argv[2], "--vm-probe-late-fault") ||
					!strcmp(argv[2], "--vm-probe-pressure-fault") ||
					!strcmp(argv[2], "--vm-probe-exit-publish") ||
					!strcmp(argv[2], "--vm-probe-exit") ||
					!strcmp(argv[2], "--vm-probe-lifetime") ||
					!strcmp(argv[2], "--vm-probe-death") ||
					!strcmp(argv[2], "--vm-probe-rollback"))) ||
	      (argc == 3 && (!strcmp(argv[2], "--timed-wait") ||
					!strcmp(argv[2], "--rcu") ||
					!strcmp(argv[2], "--workqueue") ||
					!strcmp(argv[2], "--cleanup") ||
					!strcmp(argv[2], "--vfs") ||
					!strcmp(argv[2], "--shmem") ||
					!strcmp(argv[2], "--pressure") ||
					!strcmp(argv[2], "--alloc-failure") ||
					!strcmp(argv[2], "--all"))));
	diagnostic_memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
	CHECK(diagnostic_memory >= 0);
	sigemptyset(&action.sa_mask);
	CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
	CHECK(sigaction(SIGBUS, &action, NULL) == 0);
	CHECK(sigaction(SIGILL, &action, NULL) == 0);
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "cannot load boot core: %s\n", dlerror());
		return 1;
	}
	address = dlsym(handle, "kobox_linux_boot_start");
	CHECK(address && dladdr(address, &info));
	core_base = (uintptr_t)info.dli_fbase;
	memcpy(&entry, &address, sizeof(entry));
	address = dlsym(handle, "kobox_linux_task_dispatch");
	CHECK(address);
	memcpy(&dispatch, &address, sizeof(dispatch));
	address = dlsym(handle, "kobox_linux_boot_verify");
	CHECK(address);
	memcpy(&verify_boot, &address, sizeof(verify_boot));
	address = dlsym(handle, "kobox_linux_boot_memory_verify");
	CHECK(address);
	memcpy(&verify_memory, &address, sizeof(verify_memory));
	if (argc == 7 || argc == 9) {
		unsigned int index;

		address = dlsym(handle, "kobox_linux_module_probe");
		CHECK(address);
		memcpy(&probe_modules, &address, sizeof(probe_modules));
		for (index = 0; index < KOBOX_GEM_MODULES; index++)
			CHECK(!read_module_image(argv[(argc == 9 ? 4 : 3) + index],
						&module_host.images[index]));
		module_host.access = module_access;
		if (argc == 9) {
			CHECK(!read_module_image(argv[8], &module_host.lifetime_image));
			module_host.vm = &vm_host;
			module_host.buffer_cleanup = !strcmp(argv[2], "--gem-cleanup") ||
				!strcmp(argv[2], "--gem-cleanup-object-last") ? KOBOX_GEM_CLEANUP_NORMAL :
				!strcmp(argv[2], "--gem-cleanup-death") ||
				!strcmp(argv[2], "--gem-cleanup-death-object-last") ? KOBOX_GEM_CLEANUP_DEATH :
				KOBOX_GEM_CLEANUP_NONE;
			module_host.allocation_failures = !strcmp(argv[2], "--gem-failure") ||
				!strcmp(argv[2], "--gem-failure-object-last") || module_host.buffer_cleanup;
			module_host.gem_final_owner = (!strcmp(argv[2], "--gem-object-last") ||
				!strcmp(argv[2], "--gem-cleanup-object-last") ||
				!strcmp(argv[2], "--gem-cleanup-death-object-last") ||
				!strcmp(argv[2], "--gem-failure-object-last")) ?
				KOBOX_GEM_FINAL_OBJECT : KOBOX_GEM_FINAL_VMA;
		}
	}
	if (argc == 4) {
		address = dlsym(handle, "kobox_linux_vm_probe");
		CHECK(address);
		memcpy(&probe_vm, &address, sizeof(probe_vm));
	}
	if (argc == 3 && (!strcmp(argv[2], "--vfs") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_vfs_verify");
		CHECK(address);
		memcpy(&verify_vfs, &address, sizeof(verify_vfs));
	}
	if (argc == 3 && (!strcmp(argv[2], "--shmem") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_shmem_verify");
		CHECK(address);
		memcpy(&verify_shmem, &address, sizeof(verify_shmem));
	}
	if (argc == 3 && (!strcmp(argv[2], "--timed-wait") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_wait_verify");
		CHECK(address);
		memcpy(&verify_wait, &address, sizeof(verify_wait));
	}
	if (argc == 3 && !strcmp(argv[2], "--pressure")) {
		address = dlsym(handle, "kobox_linux_pressure_verify");
		CHECK(address);
		memcpy(&verify_pressure, &address, sizeof(verify_pressure));
	}
	if (argc == 3 && !strcmp(argv[2], "--alloc-failure")) {
		address = dlsym(handle, "kobox_linux_allocation_verify");
		CHECK(address);
		memcpy(&verify_pressure, &address, sizeof(verify_pressure));
	}
	if (argc == 3 && (!strcmp(argv[2], "--rcu") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_rcu_verify");
		CHECK(address);
		memcpy(&verify_rcu, &address, sizeof(verify_rcu));
	}
	if (argc == 3 && (!strcmp(argv[2], "--workqueue") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_workqueue_verify");
		CHECK(address);
		memcpy(&verify_workqueue, &address, sizeof(verify_workqueue));
	}
	if (argc == 3 && (!strcmp(argv[2], "--cleanup") || !strcmp(argv[2], "--all"))) {
		address = dlsym(handle, "kobox_linux_cleanup_verify");
		CHECK(address);
		memcpy(&verify_cleanup, &address, sizeof(verify_cleanup));
	}
	CHECK(kobox_posix_memory_backing_init(&backing, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&direct, TEST_RAM_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmemmap, TEST_VMEMMAP_SIZE) == 0);
	CHECK(kobox_posix_memory_window_init(&vmalloc, TEST_VMALLOC_SIZE) == 0);
	CHECK(kobox_posix_memory_window_map(&direct, 0, &backing, 0,
		TEST_RAM_SIZE, KOBOX_POSIX_MEMORY_READ | KOBOX_POSIX_MEMORY_WRITE,
		&direct_address) == 0);
	CHECK(kobox_boot_image_alias(handle, &backing, direct_address,
		TEST_IMAGE_PHYSICAL_BASE, &image) == 0);
	core_size = image.size;
	if (probe_vm || module_host.vm) {
		unsigned int index;

		CHECK(kobox_posix_vm_service_create(&vm_service, kobox_vm_posix_notify, NULL) == 0);
		vm_host = (struct kobox_linux_vm_test) {
			.size = sizeof(vm_host), .operations = &kobox_vm_posix_operations,
			.start = KOBOX_VM_WINDOW_BASE, .length = KOBOX_VM_WINDOW_SIZE,
			.probe = kobox_vm_posix_probe,
			.readonly_case = !strcmp(argv[2], "--vm-probe-ro"),
			.reuse_case = !strcmp(argv[2], "--vm-probe-reuse"),
			.race_case = !strcmp(argv[2], "--vm-probe-irq") ? KOBOX_VM_RACE_IRQ :
				!strcmp(argv[2], "--vm-probe-truncate") ? KOBOX_VM_RACE_TRUNCATE :
				!strcmp(argv[2], "--vm-probe-late-fault") ? KOBOX_VM_RACE_LATE_FAULT :
				!strcmp(argv[2], "--vm-probe-pressure-fault") ? KOBOX_VM_RACE_PRESSURE_FAULT :
				!strcmp(argv[2], "--vm-probe-exit-publish") ? KOBOX_VM_RACE_EXIT_PUBLISH :
				!strcmp(argv[2], "--vm-probe-exit") ? KOBOX_VM_RACE_EXIT : KOBOX_VM_RACE_NONE,
			.notify = vm_notify,
			.lifetime_case = !strcmp(argv[2], "--vm-probe-lifetime") ? KOBOX_VM_LIFETIME_NORMAL :
				!strcmp(argv[2], "--vm-probe-death") ? KOBOX_VM_LIFETIME_DEATH :
				!strcmp(argv[2], "--vm-probe-rollback") ? KOBOX_VM_LIFETIME_ROLLBACK : KOBOX_VM_LIFETIME_NONE,
			.terminate = vm_terminate,
		};
		for (index = 0; index < 2; index++) {
			struct kobox_posix_vm_remote *remote = NULL;
			pid_t pid;

			CHECK(kobox_posix_vm_remote_create(vm_service, argv[3], &backing, &remote, &pid) == 0);
			vm_host.spaces[index] = remote;
			vm_host.pids[index] = pid;
		}
	}
	CHECK(kobox_task_posix_init(dispatch) == 0);
	CHECK(kobox_posix_task_bind_current(&boot_task) == 0);
	CHECK(kobox_task_posix_operations.cpu_enter(0, boot_task) == 0);
	CHECK(kobox_task_posix_operations.cpu_irq_disable(0) == 0);
	layout = (struct kobox_linux_boot_layout) {
		.size = sizeof(layout),
		.exceptions_install = kobox_posix_exceptions_install,
		.image_protect = kobox_boot_image_protect,
		.image = &image,
		.task = {
			.size = sizeof(layout.task),
			.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
			.memory = {
				.size = sizeof(layout.task.memory),
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
				.kernel_image_physical_base = TEST_IMAGE_PHYSICAL_BASE,
			},
			.operations = &kobox_task_posix_operations,
			.boot_task = boot_task,
		},
		.command_line = "console=kobox earlycon loglevel=8",
		.console_write = console_write,
		.kernel_main = kernel_main,
		.kernel_argument = &report,
	};
	status = entry(&layout, &report);
	fprintf(stderr, "upstream start_kernel unexpectedly returned: %d\n", status);
	return 1;
}
