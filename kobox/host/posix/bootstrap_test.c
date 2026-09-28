// SPDX-License-Identifier: GPL-2.0-only
#include "bootstrap.h"
#include "../../boot/image_layout.h"

#include <elf.h>
#include <stdio.h>
#include <string.h>
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

/* Upstream Linux functions are local to the linked core by design. This
 * test-only lookup uses the already-validated image symbol table without
 * widening the production export surface just for a reservation check. */
static void *linux_test_symbol(struct kobox_posix_core *core, const char *name)
{
	const struct kobox_fixed_image *image = kobox_posix_core_image(core);
	const Elf64_Sym *symbols = image->symbols;
	size_t i;

	for (i = 0; i < image->symbol_count; i++) {
		const Elf64_Sym *symbol = &symbols[i];
		const char *candidate;
		size_t available;

		if (symbol->st_shndx == SHN_UNDEF ||
		    symbol->st_name >= image->strings_size ||
		    ELF64_ST_TYPE(symbol->st_info) != STT_FUNC)
			continue;
		candidate = image->strings + symbol->st_name;
		available = image->strings_size - symbol->st_name;
		if (!memchr(candidate, 0, available) || strcmp(candidate, name) ||
		    symbol->st_value < image->link_base ||
		    symbol->st_value - image->link_base >= image->image_size ||
		    symbol->st_size > image->image_size -
			(symbol->st_value - image->link_base))
			continue;
		return (char *)kobox_posix_core_base(core) +
			(symbol->st_value - image->link_base);
	}
	return NULL;
}

static void pid_one(void *argument)
{
	const struct boot_observation *observation = argument;
	struct kobox_boot_core *core =
		kobox_posix_core_boot(observation->bootstrap->core);
	void *(*reserve_area)(unsigned long, unsigned long, const void *);
	void (*release_area)(void *);
	void *symbol, *area;

	/* The production port calls this only from upstream PID 1 after
	 * SYSTEM_RUNNING, init-memory release and RCU boot-end publication.
	 * No Gate entry point or hand-initialized subsystem is involved.
	 */
	if (!core->started || !observation->report->context_switches)
		_exit(1);
	/* PCI BARs may need one 256 MiB ioremap. Reserve and release the
	 * virtual area through the same Linux allocator, including its guard
	 * pages, without requiring physical RAM for the test. */
	symbol = linux_test_symbol(observation->bootstrap->core,
				   "get_vm_area_caller");
	if (!symbol) {
		console_write(NULL, "missing Linux vmalloc reserve symbol\n",
			      sizeof("missing Linux vmalloc reserve symbol\n") - 1);
		_exit(1);
	}
	memcpy(&reserve_area, &symbol, sizeof(reserve_area));
	symbol = linux_test_symbol(observation->bootstrap->core, "free_vm_area");
	if (!symbol) {
		console_write(NULL, "missing Linux vmalloc release symbol\n",
			      sizeof("missing Linux vmalloc release symbol\n") - 1);
		_exit(1);
	}
	memcpy(&release_area, &symbol, sizeof(release_area));
	area = reserve_area(256UL << 20, 1, NULL); /* VM_IOREMAP */
	if (!area) {
		console_write(NULL, "Linux 256 MiB vmalloc reservation failed\n",
			      sizeof("Linux 256 MiB vmalloc reservation failed\n") - 1);
		_exit(1);
	}
	release_area(area);
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
		.vmalloc_size = KOBOX_CORE_VMALLOC_SIZE,
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
