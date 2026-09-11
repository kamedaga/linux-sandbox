// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "host.h"
#include "boot_test.h"
#include "client_task_gate.h"
#include "syscall_gate.h"
#include "exec_gate.h"
#include "../arch/x86_64/user_layout.h"
#include "dma_gate.h"
#include "irq_gate.h"
#include "virtio_gate.h"
#include "../host/posix/image.h"
#include "../host/posix/exception.h"
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
#include "module_launch.h"
#include "pci_host.h"
#include "../mm/posix.h"
#include "../host/posix/vm_service.h"
#include "../task/posix_machine.h"
#include "../host/posix/host.h"

#include "../host/posix/core.h"
#include "../host/posix/bootstrap.h"
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
static int (*verify_client_task)(struct kobox_client_task_report *report);
static int (*verify_syscall)(const struct kobox_syscall_test *, struct kobox_syscall_report *);
static bool autonomous_client;
static int (*verify_exec)(const struct kobox_exec_test *, struct kobox_exec_report *);
static struct kobox_linux_module_image exec_image;
static struct kobox_posix_vm_service *exec_vm_service;
static int (*verify_vfs)(struct kobox_linux_vfs_report *report);
static int (*verify_shmem)(struct kobox_linux_shmem_report *report);
static int (*verify_pressure)(struct kobox_linux_pressure_report *report);
static int (*verify_pci)(const struct kobox_linux_pci_host *,
			 struct kobox_linux_pci_report *, int (*)(void *));
static int (*verify_dma)(const struct kobox_linux_pci_host *,
			 const struct kobox_linux_dma_test *, struct kobox_linux_dma_report *);
static int (*verify_irq)(const struct kobox_linux_pci_host *,
			 const struct kobox_linux_irq_test *, struct kobox_linux_irq_report *);
static int (*verify_virtio)(const struct kobox_linux_virtio_test *,
			    struct kobox_linux_virtio_report *);
static int (*probe_vm)(const struct kobox_linux_vm_test *host, struct kobox_linux_vm_report *report);
static struct kobox_linux_vm_test vm_host;
static struct kobox_linux_module_test module_host = {.size = sizeof(module_host)};
static const struct kobox_boot_test_resources *resource_host;
static int resources_drained;
static int (*run_modules)(const struct kobox_linux_module_launch *,
			  struct kobox_linux_module_launch_report *);

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
static int pci_mapping_faults(void *address)
{
	return module_access(address, KOBOX_MODULE_READ);
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
	struct kobox_linux_pci_report pci = {.size = sizeof(pci)};
	struct kobox_linux_dma_report dma = {.size = sizeof(dma)};
	struct kobox_linux_irq_report irq = {.size = sizeof(irq)};
	struct kobox_linux_vm_report vm = {.size = sizeof(vm)};
	struct kobox_linux_task_report *task = argument;
	char message[512];
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
	if (!status && verify_virtio) {
		struct kobox_linux_virtio_report gpu = {.size = sizeof(gpu)};
		struct kobox_linux_virtio_test test = *resource_host->virtio;
		const struct kobox_exec_test client = {
			.size = sizeof(client), .vm = &vm_host,
			.image = exec_image.data, .length = exec_image.length,
			.files = resource_host->client_files,
			.file_count = resource_host->client_file_count,
		};

		if (verify_exec)
			test.client = &client;
		status = verify_virtio(&test, &gpu);
		if (!status && verify_exec)
			status = -kobox_posix_vm_service_quiescent(exec_vm_service);
		resources_drained = gpu.drained;
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"Native virtio: status=%d loaded=%u unloaded=%u bound=%u vectors=%u nodes=%u phase=%u line=%u cleanup=%d drained=%u warnings=%llu interrupts=%llu\n",
			status, gpu.loaded, gpu.unloaded, gpu.bound, gpu.vectors, gpu.nodes,
			gpu.phase, gpu.line, gpu.cleanup, gpu.drained,
			(unsigned long long)gpu.warnings, (unsigned long long)gpu.interrupts);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (gpu.diagnostics[0])
			(void)write(STDERR_FILENO, gpu.diagnostics,
				    strnlen(gpu.diagnostics, sizeof(gpu.diagnostics)));
		if (test.client) {
			if (gpu.client.diagnostics[0])
				(void)write(STDERR_FILENO, gpu.client.diagnostics,
					    strnlen(gpu.client.diagnostics, sizeof(gpu.client.diagnostics)));
			length = snprintf(message, sizeof(message),
				"DRM client: entered=%u exited=%u cpus=%u phase=%u line=%u program=%d user_line=%llu user_error=%lld\n",
				gpu.client.entered, gpu.client.exited, gpu.client.cpu_mask,
				gpu.client.phase, gpu.client.line, gpu.client.program_status,
				(unsigned long long)gpu.client.user_failure.line,
				(long long)gpu.client.user_failure.error);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_pci) {
		status = verify_pci(resource_host->pci, &pci, pci_mapping_faults);
		resources_drained = pci.scans == pci.removals;
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"PCI enumeration: status=%d scans=%u caps=%u removals=%u maps=%u revoked=%u cache=%u bar=%llx size=%llx warnings=%llu phase=%u\n",
			status, pci.scans, pci.capabilities, pci.removals, pci.mappings,
			pci.revoked_mappings, pci.cache_mode,
			(unsigned long long)pci.bar_start,
			(unsigned long long)pci.bar_size,
			(unsigned long long)pci.warnings, pci.phase);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_dma) {
		status = verify_dma(resource_host->pci, resource_host->dma, &dma);
		resources_drained = dma.drained;
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"DMA Gate: status=%d cases=%u phase=%u drained=%u warnings=%llu cpu_mask=%x rounds=%u sole_pins=%u pressure=%u reclaimed=%u pressure_line=%u\n",
			status, dma.cases, dma.phase, dma.drained, (unsigned long long)dma.warnings,
			dma.cpu_mask, dma.parallel_rounds, dma.sole_pins,
			dma.pressure_pages, dma.pressure_reclaimed, dma.pressure_line);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_irq) {
		status = verify_irq(resource_host->pci, resource_host->irq, &irq);
		resources_drained = irq.drained;
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"IRQ routing: status=%d vectors=%u deliveries=%u cpu_mask=%x modes=%u rounds=%u masks=%u migrations=%u rollbacks=%u sync=%u stale=%u dma=%u pending_free=%u warnings=%llu line=%u drained=%u\n",
			status, irq.vectors, irq.deliveries, irq.cpu_mask, irq.modes, irq.rounds,
			irq.masks, irq.migrations, irq.rollbacks, irq.synchronizations, irq.stale,
			irq.dma, irq.pending_free, (unsigned long long)irq.warnings, irq.line, irq.drained);
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
	if (!status && verify_client_task) {
		struct kobox_client_task_report client = {.size = sizeof(client)};

		status = verify_client_task(&client);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"Client task prerequisite: status=%d tasks=%u switches=%u mappings=%u creds=%u files=%u reaped=%u warnings=%llu phase=%u line=%u\n",
			status, client.tasks, client.switches, client.mappings,
			client.credentials, client.files, client.reaped,
			(unsigned long long)client.warnings, client.phase, client.line);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_exec && !verify_virtio) {
		const struct kobox_exec_test host = {
			.size = sizeof(host), .vm = &vm_host,
			.image = exec_image.data, .length = exec_image.length,
		};
		struct kobox_exec_report report = {.size = sizeof(report)};

		status = verify_exec(&host, &report);
		if (!status)
			status = -kobox_posix_vm_service_quiescent(exec_vm_service);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"External ELF exec: status=%d entered=%u exited=%u reclaimed=%u cpus=%u warnings=%llu phase=%u line=%u program=%d result=%d user_pid=%llu user_line=%llu user_error=%lld\n",
			status, report.entered, report.exited, report.reclaimed, report.cpu_mask,
			(unsigned long long)report.warnings, report.phase, report.line,
			report.program_status, report.result,
			(unsigned long long)report.user_failure.pid,
			(unsigned long long)report.user_failure.line,
			(long long)report.user_failure.error);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && verify_syscall) {
		const struct kobox_syscall_test host = {
			.size = sizeof(host), .vm = &vm_host,
			.issue = autonomous_client ? NULL : kobox_vm_posix_syscall_probe,
		};
		struct kobox_syscall_report report = {.size = sizeof(report)};

		status = verify_syscall(&host, &report);
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"External syscall dispatch: status=%d calls=%u faults=%u tls=%u nested=%u invalid=%u fd=%u exit=%u reclaimed=%u cpus=%u warnings=%llu line=%u nr=%llu returned=%lld rights=%u truncated=%u queued_exit=%u rendezvous=%u race_sent=%u race_rejected=%u inherited=%u cow=%u binding_rollbacks=%u forks=%u clones=%u threads=%u group_exits=%u autonomous=%u\n",
			status, report.calls, report.faults, report.tls, report.nested, report.invalid, report.descriptors,
			report.exited, report.reclaimed, report.cpu_mask, (unsigned long long)report.warnings,
			report.line, (unsigned long long)report.number, (long long)report.returned,
			report.transfers, report.truncated, report.queued_exit,
			report.rendezvous, report.race_sent, report.race_rejected,
			report.inherited, report.cow, report.binding_rollbacks, report.native_forks,
			report.shared_clones, report.threads, report.group_exits, report.autonomous);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
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
	if (!status && run_modules) {
		struct kobox_linux_module_launch_report modules = {.size = sizeof(modules)};

		status = run_modules(resource_host->modules, &modules);
		resources_drained = modules.loaded == modules.unloaded && !modules.cleanup_result;
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		length = snprintf(message, sizeof(message),
			"Native manifest lifecycle: status=%d loaded=%zu unloaded=%zu cleanup=%d\n",
			status, modules.loaded, modules.unloaded, modules.cleanup_result);
		if (length > 0 && (size_t)length < sizeof(message))
			(void)write(STDERR_FILENO, message, length);
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (!status && probe_modules) {
		struct kobox_linux_module_report modules = {.size = sizeof(modules)};

		status = probe_modules(&module_host, &modules);
		resources_drained = modules.loaded == modules.unloaded;
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
		if (module_host.issue_syscall) {
			length = snprintf(message, sizeof(message),
				"DRM syscalls: result=%d line=%u nr=%llu returned=%lld calls=%u files=%u handles=%u nested=%u invalid=%u fd=%u faults=%u reclaimed=%u exited=%u cpus=%u transfers=%u truncated=%u queued_exit=%u\n",
				modules.syscalls.result, modules.syscalls.line,
				(unsigned long long)modules.syscalls.number,
				(long long)modules.syscalls.returned, modules.syscalls.calls,
				modules.syscalls.drm_files, modules.syscalls.gem_handles,
				modules.syscalls.nested, modules.syscalls.invalid, modules.syscalls.descriptors,
				modules.syscalls.faults, modules.syscalls.reclaimed,
				modules.syscalls.exited, modules.syscalls.cpu_mask,
				modules.syscalls.transfers, modules.syscalls.truncated,
				modules.syscalls.queued_exit);
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
			length = snprintf(message, sizeof(message),
				"cleanup watchdog: hold_phase=%u work=%u delayed=%u probe_timeout_phase=%u calls=%u phase=%u cleaner=%u active=%u\n",
				cleanup.hold_phase, cleanup.hold_work_active,
				cleanup.hold_delayed_active, cleanup.probe_timeout_phase,
				cleanup.probe_calls, cleanup.probe_phase,
				cleanup.probe_cleaner, cleanup.probe_active);
			if (length > 0 && (size_t)length < sizeof(message))
				(void)write(STDERR_FILENO, message, length);
		}
		if (kobox_posix_notifications_restore(notification_mask))
			__builtin_trap();
	}
	if (resources_drained && resource_host && resource_host->close) {
		/* Native unload and RCU drain have completed. Stop host signal
		 * reentry while releasing the bootstrap-owned registry and images.
		 */
		if (kobox_posix_notifications_save(&notification_mask))
			__builtin_trap();
		resource_host->close(resource_host->context);
	}
	_exit(status ? 1 : 0);
}

int kobox_boot_test_run(int argc, char **argv,
			const struct kobox_boot_test_resources *resources)
{
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_bootstrap bootstrap = {0};
	const struct kobox_posix_boot_profile profile = {
		.ram_size = TEST_RAM_SIZE,
		.vmemmap_size = TEST_VMEMMAP_SIZE,
		.vmalloc_size = TEST_VMALLOC_SIZE,
		.image_physical_base = TEST_IMAGE_PHYSICAL_BASE,
	};
	struct kobox_posix_vm_service *vm_service = NULL;
	struct kobox_linux_task_report report = {
		.size = sizeof(report),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
	};
	struct sigaction action = {
		.sa_sigaction = crash_handler,
		.sa_flags = SA_SIGINFO,
	};
	struct kobox_posix_core *native_core = NULL;
	struct kobox_boot_core *core;
	void *address;
	int status;

	resource_host = resources;
	module_host.lifecycle = resources ? resources->lifecycle : NULL;
	CHECK(argc == 2 || (argc == 5 && !strcmp(argv[2], "--elf-exec")) ||
	      (argc == 7 && !strcmp(argv[2], "--modules")) ||
	      (argc == 8 && (!strcmp(argv[2], "--resource-port") ||
			    !strcmp(argv[2], "--resource-port-fail")) && resources) ||
	      (argc == 9 && (!strcmp(argv[2], "--gem") ||
			    !strcmp(argv[2], "--gem-syscall") ||
			    !strcmp(argv[2], "--gem-fd-transfer") ||
			    !strcmp(argv[2], "--gem-object-last") ||
			    !strcmp(argv[2], "--gem-failure") ||
			    !strcmp(argv[2], "--gem-cleanup") ||
			    !strcmp(argv[2], "--gem-cleanup-object-last") ||
			    !strcmp(argv[2], "--gem-cleanup-death") ||
			    !strcmp(argv[2], "--gem-cleanup-death-object-last") ||
			    !strcmp(argv[2], "--gem-failure-object-last"))) ||
	      (argc == 4 && (!strcmp(argv[2], "--client-run") ||
					!strcmp(argv[2], "--syscall") || !strcmp(argv[2], "--fd-transfer") ||
					!strcmp(argv[2], "--fd-exit-race") ||
					!strcmp(argv[2], "--fd-inheritance") ||
					!strcmp(argv[2], "--fork") ||
					!strcmp(argv[2], "--clone") ||
					!strcmp(argv[2], "--thread") ||
					!strcmp(argv[2], "--thread-exit-wait") ||
					!strcmp(argv[2], "--thread-exit-running") ||
					!strcmp(argv[2], "--thread-exit-peer") ||
					!strcmp(argv[2], "--vm-probe") ||
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
					!strcmp(argv[2], "--client-task") ||
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
	CHECK(!kobox_posix_core_open(argv[1], &native_core));
	core = kobox_posix_core_boot(native_core);
	core_base = (uintptr_t)kobox_posix_core_base(native_core);
	address = core->lookup(core->loader, "kobox_linux_boot_verify");
	CHECK(address);
	memcpy(&verify_boot, &address, sizeof(verify_boot));
	address = core->lookup(core->loader, "kobox_linux_boot_memory_verify");
	CHECK(address);
	memcpy(&verify_memory, &address, sizeof(verify_memory));
	if (resources && resources->pci) {
		CHECK(argc == 2);
		address = core->lookup(core->loader, resources->pci_verifier ?
			       resources->pci_verifier : "kobox_linux_pci_verify");
		CHECK(address);
		memcpy(&verify_pci, &address, sizeof(verify_pci));
	}
	if (resources && resources->dma) {
		CHECK(resources->pci && resources->prepare_dma);
		address = core->lookup(core->loader, "kobox_linux_dma_verify");
		CHECK(address);
		memcpy(&verify_dma, &address, sizeof(verify_dma));
	}
	if (resources && resources->modules) {
		CHECK(argc == 2);
		address = core->lookup(core->loader, "kobox_linux_modules_run");
		CHECK(address);
		memcpy(&run_modules, &address, sizeof(run_modules));
	}
	if (resources && resources->irq) {
		CHECK(resources->pci);
		address = core->lookup(core->loader, "kobox_linux_irq_verify");
		CHECK(address);
		memcpy(&verify_irq, &address, sizeof(verify_irq));
	}
	if (resources && resources->virtio) {
		CHECK((argc == 2 || argc == 5) && resources->prepare_dma);
		address = core->lookup(core->loader, "kobox_linux_virtio_verify");
		CHECK(address);
		memcpy(&verify_virtio, &address, sizeof(verify_virtio));
	}
	if (argc == 5) {
		address = core->lookup(core->loader, "kobox_linux_exec_verify");
		CHECK(address);
		memcpy(&verify_exec, &address, sizeof(verify_exec));
		CHECK(!read_module_image(argv[4], &exec_image));
	}
	if (argc == 7 || argc == 8 || argc == 9) {
		unsigned int index;

		address = core->lookup(core->loader, "kobox_linux_module_probe");
		CHECK(address);
		memcpy(&probe_modules, &address, sizeof(probe_modules));
		for (index = 0; index < KOBOX_GEM_MODULES; index++)
			CHECK(!read_module_image(argv[(argc == 9 ? 4 : 3) + index],
						&module_host.images[index]));
		module_host.access = module_access;
		if (argc == 8) {
			CHECK(!read_module_image(argv[7], &module_host.resource_image));
			module_host.resource_fail_init = !strcmp(argv[2], "--resource-port-fail");
		}
		if (argc == 9) {
			CHECK(!read_module_image(argv[8], &module_host.lifetime_image));
			module_host.vm = &vm_host;
			module_host.syscall_rights = !strcmp(argv[2], "--gem-fd-transfer");
			if (!strcmp(argv[2], "--gem-syscall") || module_host.syscall_rights)
				module_host.issue_syscall = kobox_vm_posix_syscall_probe;
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
	if (argc == 4 && strcmp(argv[2], "--client-run") &&
	    strcmp(argv[2], "--syscall") && strcmp(argv[2], "--fd-transfer") &&
	    strcmp(argv[2], "--fd-exit-race") && strcmp(argv[2], "--fd-inheritance") &&
	    strcmp(argv[2], "--fork") && strcmp(argv[2], "--clone") && strcmp(argv[2], "--thread") &&
	    strcmp(argv[2], "--thread-exit-wait") && strcmp(argv[2], "--thread-exit-running") &&
	    strcmp(argv[2], "--thread-exit-peer")) {
		address = core->lookup(core->loader, "kobox_linux_vm_probe");
		CHECK(address);
		memcpy(&probe_vm, &address, sizeof(probe_vm));
	}
	if (argc == 4 && (!strcmp(argv[2], "--client-run") ||
			 !strcmp(argv[2], "--syscall") || !strcmp(argv[2], "--fd-transfer") ||
			 !strcmp(argv[2], "--fd-exit-race") || !strcmp(argv[2], "--fd-inheritance") ||
			 !strcmp(argv[2], "--fork") || !strcmp(argv[2], "--clone") ||
			 !strcmp(argv[2], "--thread") || !strcmp(argv[2], "--thread-exit-wait") ||
			 !strcmp(argv[2], "--thread-exit-running") || !strcmp(argv[2], "--thread-exit-peer"))) {
		autonomous_client = !strcmp(argv[2], "--client-run");
		address = core->lookup(core->loader, autonomous_client ?
			"kobox_linux_autonomous_client_verify" : !strcmp(argv[2], "--thread-exit-peer") ?
			"kobox_linux_group_exit_peer_verify" : !strcmp(argv[2], "--thread-exit-wait") ?
			"kobox_linux_group_exit_wait_verify" : !strcmp(argv[2], "--thread-exit-running") ?
			"kobox_linux_group_exit_running_verify" :
			!strcmp(argv[2], "--thread") ? "kobox_linux_thread_verify" :
			!strcmp(argv[2], "--clone") ? "kobox_linux_clone_verify" :
			!strcmp(argv[2], "--fork") ? "kobox_linux_fork_verify" :
			!strcmp(argv[2], "--fd-inheritance") ?
			"kobox_linux_fd_inheritance_verify" : !strcmp(argv[2], "--fd-exit-race") ?
			"kobox_linux_fd_exit_race_verify" : !strcmp(argv[2], "--fd-transfer") ?
			"kobox_linux_fd_transfer_verify" : "kobox_linux_syscall_verify");
		CHECK(address);
		memcpy(&verify_syscall, &address, sizeof(verify_syscall));
	}
	if (argc == 3 && !strcmp(argv[2], "--client-task")) {
		address = core->lookup(core->loader, "kobox_linux_client_task_verify");
		CHECK(address);
		memcpy(&verify_client_task, &address, sizeof(verify_client_task));
	}
	if (argc == 3 && (!strcmp(argv[2], "--vfs") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_vfs_verify");
		CHECK(address);
		memcpy(&verify_vfs, &address, sizeof(verify_vfs));
	}
	if (argc == 3 && (!strcmp(argv[2], "--shmem") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_shmem_verify");
		CHECK(address);
		memcpy(&verify_shmem, &address, sizeof(verify_shmem));
	}
	if (argc == 3 && (!strcmp(argv[2], "--timed-wait") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_wait_verify");
		CHECK(address);
		memcpy(&verify_wait, &address, sizeof(verify_wait));
	}
	if (argc == 3 && !strcmp(argv[2], "--pressure")) {
		address = core->lookup(core->loader, "kobox_linux_pressure_verify");
		CHECK(address);
		memcpy(&verify_pressure, &address, sizeof(verify_pressure));
	}
	if (argc == 3 && !strcmp(argv[2], "--alloc-failure")) {
		address = core->lookup(core->loader, "kobox_linux_allocation_verify");
		CHECK(address);
		memcpy(&verify_pressure, &address, sizeof(verify_pressure));
	}
	if (argc == 3 && (!strcmp(argv[2], "--rcu") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_rcu_verify");
		CHECK(address);
		memcpy(&verify_rcu, &address, sizeof(verify_rcu));
	}
	if (argc == 3 && (!strcmp(argv[2], "--workqueue") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_workqueue_verify");
		CHECK(address);
		memcpy(&verify_workqueue, &address, sizeof(verify_workqueue));
	}
	if (argc == 3 && (!strcmp(argv[2], "--cleanup") || !strcmp(argv[2], "--all"))) {
		address = core->lookup(core->loader, "kobox_linux_cleanup_verify");
		CHECK(address);
		memcpy(&verify_cleanup, &address, sizeof(verify_cleanup));
	}
	CHECK(kobox_posix_memory_backing_init(&backing, TEST_RAM_SIZE) == 0);
	if (resources && resources->prepare_dma)
		CHECK(!resources->prepare_dma(resources->context, backing.descriptor, backing.size));
	CHECK(!kobox_posix_bootstrap_prepare(&bootstrap, native_core, &backing, &profile));
	core_size = bootstrap.image.size;
	if (probe_vm || module_host.vm || verify_syscall || verify_exec) {
		unsigned int index;

		CHECK(kobox_posix_vm_service_create(&vm_service, kobox_vm_posix_notify, NULL) == 0);
		if (resources && resources->vm_ready)
			resources->vm_ready(resources->context, vm_service);
		if (verify_exec)
			exec_vm_service = vm_service;
		vm_host = (struct kobox_linux_vm_test) {
			.size = sizeof(vm_host), .operations = &kobox_vm_posix_operations,
			.start = verify_exec ? KOBOX_X86_USER_START :
				autonomous_client ? KOBOX_VM_TEST_WINDOW_BASE + (UINT64_C(1) << 32) :
				KOBOX_VM_TEST_WINDOW_BASE,
			.length = verify_exec ? KOBOX_X86_USER_END - KOBOX_X86_USER_START :
				autonomous_client ? 64UL * 1024 * 1024 : KOBOX_VM_TEST_WINDOW_SIZE,
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

			CHECK(kobox_posix_vm_remote_create(vm_service, argv[3], &backing,
				vm_host.start, vm_host.length, &remote, &pid) == 0);
			vm_host.spaces[index] = remote;
			vm_host.pids[index] = pid;
		}
	}
	bootstrap.layout.resources = resources ? resources->port : NULL;
	bootstrap.layout.command_line = "console=kobox earlycon loglevel=8";
	bootstrap.layout.console_write = console_write;
	bootstrap.layout.kernel_main = kernel_main;
	bootstrap.layout.kernel_argument = &report;
	CHECK(!kobox_posix_bootstrap_start(&bootstrap, &report, &status));
	fprintf(stderr, "upstream start_kernel unexpectedly returned: %d\n", status);
	return 1;
}
