// SPDX-License-Identifier: GPL-2.0-only

#include "module_gate.h"
#include "vm_lifetime.h"
#include "exception.h"
#include "allocation_gate.h"
#include "../mm/port.h"

#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/moduleloader.h>
#include <linux/mount.h>
#include <linux/rcupdate.h>
#include <linux/elf.h>
#include <linux/execmem.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/task_work.h>
#include <linux/swap.h>
#include "../../kernel/module/internal.h"
#include <asm/current.h>
#include <asm/irqflags.h>
#include <asm/percpu.h>
#include <asm/preempt.h>
#include <asm/ptrace.h>
#include <asm/set_memory.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>

long __x64_sys_init_module(const struct pt_regs *regs);
long __x64_sys_delete_module(const struct pt_regs *regs);

struct failure_module_call {
	const struct kobox_linux_module_test *test;
	struct kobox_linux_module_report *report;
	int (*verify)(const struct kobox_linux_vm_test *,
		      struct kobox_gem_lifetime_report *,
		      const struct kobox_gem_failure_services *,
		      enum kobox_gem_final_owner);
};

static void drain_lifetime(void)
{
	flush_module_init_free_work();
	task_work_run();
	flush_delayed_fput();
	lru_add_drain_all();
	rcu_barrier();
}

static int call_failure_module(void *argument)
{
	struct failure_module_call *call = argument;
	struct kobox_gem_failure_services services = {
		.mount = kobox_linux_allocation_mount(),
		.cleanup = call->test->buffer_cleanup,
		.drain = drain_lifetime,
		.vmalloc_pages = vmalloc_nr_pages,
		.pressure = kobox_linux_pressure_inspect,
		.tasks_begin = kobox_vm_lifetime_tasks_begin,
		.tasks_finish = kobox_vm_lifetime_finish,
	};
	int result;

	if (IS_ERR(services.mount))
		return PTR_ERR(services.mount);
	result = call->verify(call->test->vm, &call->report->gem, &services,
			      call->test->gem_final_owner);
	if (result)
		return result;
	mntput(services.mount);
	drain_lifetime();
	return 0;
}

static int probe_read(const void *address, unsigned long *value)
{
	pagefault_disable();
	__get_kernel_nofault(value, address, unsigned long, fault);
	pagefault_enable();
	return 0;
fault:
	pagefault_enable();
	return -EFAULT;
}

static int probe_write(void *address, unsigned long value)
{
	pagefault_disable();
	__put_kernel_nofault(address, &value, unsigned long, fault);
	pagefault_enable();
	return 0;
fault:
	pagefault_enable();
	return -EFAULT;
}

static int check_kernel_permissions(const struct kobox_linux_module_test *test,
				    struct kobox_linux_module_report *report)
{
	/* mov $0x7319, %eax; ret -- instruction bytes, not a replacement API. */
	static const unsigned char code[] = {0xb8, 0x19, 0x73, 0, 0, 0xc3};
	unsigned long address, value, core_value;
	struct page *page;
	void *memory, *direct;
	int (*execute)(void);
	int result = -EINVAL;

	memory = execmem_alloc(EXECMEM_MODULE_TEXT, PAGE_SIZE);
	if (!memory)
		return -ENOMEM;
	address = (unsigned long)memory;
	page = vmalloc_to_page(memory);
	get_page(page);
	direct = page_address(page);
	memcpy(memory, code, sizeof(code));
	memcpy(&execute, &memory, sizeof(execute));
	set_vm_flush_reset_perms(memory);
	if (test->access(memory, KOBOX_MODULE_EXECUTE) != KOBOX_MODULE_ACCESS_DENIED)
		goto out;
	report->permissions++;
	if (set_memory_rox(address, 1) || probe_read(memory, &value) ||
	    probe_write(memory, value) != -EFAULT ||
	    probe_write(direct, value) != -EFAULT || execute() != 0x7319 ||
	    test->access(direct, KOBOX_MODULE_EXECUTE) != KOBOX_MODULE_ACCESS_DENIED)
		goto out;
	report->permissions++;
	/* Full CPA/TLB publication must not weaken boot's core RO aliases. */
	__flush_tlb_all();
	if (probe_read(__start_rodata, &core_value) ||
	    probe_write(__start_rodata, core_value) != -EFAULT ||
	    probe_write(__va(__pa_symbol(__start_rodata)), core_value) != -EFAULT)
		goto out;
	report->permissions++;
	if (set_memory_nx(address, 1) || set_memory_rw(address, 1) ||
	    probe_write(memory, value) || probe_write(direct, value) ||
	    test->access(memory, KOBOX_MODULE_EXECUTE) != KOBOX_MODULE_ACCESS_DENIED)
		goto out;
	report->permissions++;
	if (set_direct_map_invalid_noflush(page) ||
	    probe_read(direct, &value) != -EFAULT || probe_read(memory, &value) ||
	    set_direct_map_default_noflush(page) || probe_write(direct, value))
		goto out;
	report->permissions++;
	result = 0;
out:
	execmem_free(memory);
	vm_unmap_aliases();
	if (probe_read(memory, &value) != -EFAULT || page_ref_count(page) != 1 ||
	    probe_read(direct, &value) || probe_write(direct, value))
		result = -EINVAL;
	else
		report->permissions++;
	put_page(page);
	return result;
}

static bool image_range(const struct kobox_linux_module_image *image,
			unsigned long offset, unsigned long size)
{
	return offset <= image->length && size <= image->length - offset;
}

/* Read-only diagnostics against native ksymtab, not an ELF loading path.
 * The pinned core has CONFIG_PRINTK=n, so missing imports need a test report.
 */
static int check_imports(const struct kobox_linux_module_image *image,
			 struct kobox_linux_module_report *report)
{
	const Elf_Ehdr *header = image->data;
	const Elf_Shdr *sections;
	unsigned int index;
	int missing = 0;
	size_t used = 0;

	if (!header || !image_range(image, 0, sizeof(*header)) ||
	    header->e_shentsize != sizeof(*sections) ||
	    !image_range(image, header->e_shoff,
			 (unsigned long)header->e_shnum * sizeof(*sections)))
		return -ENOEXEC;
	sections = image->data + header->e_shoff;
	guard(mutex)(&module_mutex);
	for (index = 0; index < header->e_shnum; index++) {
		const Elf_Shdr *section = &sections[index], *strings;
		const Elf_Sym *symbols;
		unsigned long entry;

		if (section->sh_type != SHT_SYMTAB)
			continue;
		if (section->sh_link >= header->e_shnum ||
		    section->sh_entsize != sizeof(*symbols) ||
		    section->sh_size % sizeof(*symbols) ||
		    !image_range(image, section->sh_offset, section->sh_size))
			return -ENOEXEC;
		strings = &sections[section->sh_link];
		if (!image_range(image, strings->sh_offset, strings->sh_size))
			return -ENOEXEC;
		symbols = image->data + section->sh_offset;
		for (entry = 1; entry < section->sh_size / sizeof(*symbols); entry++) {
			const Elf_Sym *symbol = &symbols[entry];
			struct find_symbol_arg find = {.gplok = true};

			if (symbol->st_shndx != SHN_UNDEF ||
			    ELF_ST_BIND(symbol->st_info) == STB_WEAK)
				continue;
			if (symbol->st_name >= strings->sh_size)
				return -ENOEXEC;
			find.name = image->data + strings->sh_offset + symbol->st_name;
			if (!memchr(find.name, '\0', strings->sh_size - symbol->st_name))
				return -ENOEXEC;
			if (!strcmp(find.name, "_GLOBAL_OFFSET_TABLE_") || find_symbol(&find))
				continue;
			used += scnprintf(report->diagnostics + used,
				 sizeof(report->diagnostics) - used,
				 "native ksymtab missing: %s\n", find.name);
			missing++;
		}
	}
	return missing ? -ENOENT : 0;
}

int kobox_linux_module_probe(const struct kobox_linux_module_test *test,
			    struct kobox_linux_module_report *report)
{
	static const struct {
		const char *name;
		const void *address;
	} exports[] = {
		{"kobox_provider_current_task", kobox_provider_current_task},
		{"kobox_provider_current_percpu_offset", kobox_provider_current_percpu_offset},
		{"kobox_provider_irq_save_flags", kobox_provider_irq_save_flags},
		{"kobox_provider_preempt_save", kobox_provider_preempt_save},
		{"kobox_provider_preempt_restore", kobox_provider_preempt_restore},
		{"kobox_vm_space_create", kobox_vm_space_create},
		{"kobox_vm_space_destroy", kobox_vm_space_destroy},
		{"kobox_vm_resolve_fault", kobox_vm_resolve_fault},
	};
	static const char * const names[KOBOX_GEM_MODULES + 1] = {
		"i2c_core", "drm_panel_orientation_quirks", "drm", "drm_shmem_helper",
		"lifetime_test",
	};
	struct pt_regs regs = {0};
	unsigned int index;
	long result = 0;

	if (!test || test->size != sizeof(*test) || !test->access || !report ||
	    report->size != sizeof(*report))
		return -EINVAL;
	report->phase = 1;
	for (index = 0; index < ARRAY_SIZE(exports); index++) {
		void *address = __symbol_get(exports[index].name);

		if (!address)
			return -ENOENT;
		__symbol_put(exports[index].name);
		if (address != exports[index].address)
			return -EINVAL;
		report->exports++;
	}
	pr_info("GEM module prerequisite: native arch exports verified\n");
	result = check_kernel_permissions(test, report);
	if (result)
		goto out_report;
	for (index = 0; index < KOBOX_GEM_MODULES + !!test->vm; index++) {
		const struct kobox_linux_module_image *image = index < KOBOX_GEM_MODULES ?
			&test->images[index] : &test->lifetime_image;

		report->phase = 2 + index;
		if (!image->data || !image->length) {
			result = -EINVAL;
			break;
		}
		result = check_imports(image, report);
		if (result)
			break;
		regs.di = (unsigned long)image->data;
		regs.si = image->length;
		regs.dx = (unsigned long)"";
		result = __x64_sys_init_module(&regs);
		if (result)
			break;
		report->loaded++;
	}
	if (!result && test->vm && test->allocation_failures) {
		struct failure_module_call call = { .test = test, .report = report };

		call.verify = __symbol_get("kobox_gem_failure_test");
		if (!call.verify) {
			result = -ENOENT;
		} else {
			result = kobox_linux_with_allocation_failures(call_failure_module, &call);
			if (result)
				goto out_report;
			__symbol_put("kobox_gem_failure_test");
		}
	} else if (!result && test->vm) {
		int (*verify)(const struct kobox_linux_vm_test *host,
			struct kobox_gem_lifetime_report *gem, void (*drain)(void),
			enum kobox_gem_final_owner final_owner);

		verify = __symbol_get("kobox_gem_lifetime_test");
		if (!verify) {
			result = -ENOENT;
		} else {
			result = verify(test->vm, &report->gem, drain_lifetime,
					test->gem_final_owner);
			/* Failed test tasks may still own module code until exit. */
			if (result)
				goto out_report;
			__symbol_put("kobox_gem_lifetime_test");
		}
	}
	for (index = report->loaded; index; index--) {
		long unloaded;

		regs.di = (unsigned long)names[index - 1];
		regs.si = O_NONBLOCK;
		unloaded = __x64_sys_delete_module(&regs);
		if (unloaded) {
			if (!result)
				result = unloaded;
			break;
		}
		report->unloaded++;
	}
	rcu_barrier();
out_report:
	report->warnings = kobox_linux_exception_warnings();
	if (!result && report->warnings)
		result = -EINVAL;
	report->result = result;
	return result;
}
