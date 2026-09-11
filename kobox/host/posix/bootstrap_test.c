// SPDX-License-Identifier: GPL-2.0-only
#include "bootstrap.h"

#include <stdio.h>
#include <unistd.h>

struct boot_observation {
	struct kobox_posix_bootstrap *bootstrap;
	struct kobox_linux_task_report *report;
};

static void console_write(void *context, const char *text, size_t length)
{
	(void)context;
	(void)write(STDERR_FILENO, text, length);
}

static void pid_one(void *argument)
{
	const struct boot_observation *observation = argument;
	struct kobox_boot_core *core =
		kobox_posix_core_boot(observation->bootstrap->core);

	/* The production port calls this only from upstream PID 1 after
	 * SYSTEM_RUNNING, init-memory release and RCU boot-end publication.
	 * No Gate entry point or hand-initialized subsystem is involved.
	 */
	if (!core->started || !observation->report->context_switches)
		_exit(1);
	console_write(NULL, "Production core: upstream PID 1 handoff\n",
		      sizeof("Production core: upstream PID 1 handoff\n") - 1);
	_exit(0);
}

int main(int argc, char **argv)
{
	struct kobox_posix_core *core = NULL;
	struct kobox_posix_memory_backing backing = {0};
	struct kobox_posix_bootstrap bootstrap = {0};
	const struct kobox_posix_boot_profile profile = {
		.ram_size = 256UL << 20,
		.vmemmap_size = 16UL << 20,
		.vmalloc_size = 256UL << 20,
		.image_physical_base = 16UL << 20,
	};
	struct kobox_linux_task_report report = {
		.size = sizeof(report),
		.identity = KOBOX_LINUX_TASK_HOST_IDENTITY,
	};
	struct boot_observation observation = {&bootstrap, &report};
	int result = 0, status;

	if (argc != 2)
		return 2;
	status = kobox_posix_core_open(argv[1], &core);
	if (!status)
		status = kobox_posix_memory_backing_init(&backing, profile.ram_size);
	if (!status)
		status = kobox_posix_bootstrap_prepare(&bootstrap, core, &backing, &profile);
	if (status) {
		fprintf(stderr, "production bootstrap preparation failed: %d\n", status);
		return 1;
	}
	bootstrap.layout.console_write = console_write;
	bootstrap.layout.command_line = "console=kobox earlycon loglevel=8";
	bootstrap.layout.kernel_main = pid_one;
	bootstrap.layout.kernel_argument = &observation;
	status = kobox_posix_bootstrap_start(&bootstrap, &report, &result);
	fprintf(stderr, "production boot unexpectedly returned: %d/%d\n", status, result);
	return 1;
}
