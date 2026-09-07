// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"
#include "../memory/port.h"
#include "../task/boot.h"
#include "../task/time_port.h"

#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/idle.h>
#include <linux/smp.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <linux/kobox_boot.h>
#include <linux/sched/signal.h>

#include <asm/setup.h>
#include <asm/smp.h>
#include <asm/sections.h>
#include <asm/i8259.h>
#include <asm/apic.h>
#include <asm/set_memory.h>
#include <asm/x86_init.h>
#include <asm/fpu/sched.h>
#include <asm/pci_x86.h>

static const struct kobox_linux_boot_layout *boot_layout;
static unsigned long image_alias_probe;

bool kobox_linux_boot_text_address(unsigned long address)
{
	return kernel_text_address(address);
}

bool kobox_linux_boot_host_init(void)
{
	return boot_layout && boot_layout->kernel_main;
}

void __noreturn kobox_linux_boot_run_init(void)
{
	if (!kobox_linux_boot_host_init() || task_pid_nr(current) != 1 ||
	    system_state != SYSTEM_RUNNING || !kernel_set_to_readonly ||
	    !rcu_inkernel_boot_has_ended())
		panic("invalid hosted PID 1 boot handoff");
	boot_layout->kernel_main(boot_layout->kernel_argument);
	panic("hosted PID 1 entry returned");
}

static void protect_image(unsigned long begin, unsigned long end,
			  unsigned int protection)
{
	unsigned long offset = begin - (unsigned long)_text;
	int status;

	if (!boot_layout || begin >= end || begin & ~PAGE_MASK ||
	    end & ~PAGE_MASK || offset >= (unsigned long)(__bss_stop - _text) ||
	    end - begin > (unsigned long)(__bss_stop - _text) - offset)
		panic("invalid hosted image protection range");
	/* Keep the machine PTEs authoritative for later full TLB publication;
	 * otherwise a module CPA flush could re-enable writes to core rodata.
	 */
	kobox_linux_memory_image_permissions(offset, end - begin,
		!protection || (protection & KOBOX_IMAGE_WRITE));
	status = boot_layout->image_protect(boot_layout->image, offset,
					  end - begin, protection);
	if (status)
		panic("hosted image protection failed: %d", status);
}

void free_init_pages(const char *what, unsigned long begin, unsigned long end)
{
	unsigned long address;
	void *alias;

	if (begin == end)
		return;
	/* Revoke the executable/image alias before buddy can reuse these PFNs.
	 * Linux still poisons, releases and accounts every actual reserved page.
	 */
	alias = kobox_linux_boot_text_alias((void *)begin, end - begin);
	for (address = (unsigned long)alias;
	     address < (unsigned long)alias + end - begin; address += PAGE_SIZE)
		if (!PageReserved(virt_to_page((void *)address)))
			panic("hosted init page was not reserved");
	protect_image(begin, end, 0);
	free_reserved_area(alias, (char *)alias + end - begin,
			   POISON_FREE_INITMEM, what);
}

void mark_rodata_ro(void)
{
	extern char __start_ro_after_init[], __end_ro_after_init[];

	if (system_state != SYSTEM_FREEING_INITMEM || kernel_set_to_readonly)
		panic("hosted image publication outside boot end");
	protect_image((unsigned long)_stext, PAGE_ALIGN((unsigned long)_etext),
		      KOBOX_IMAGE_READ | KOBOX_IMAGE_EXECUTE);
	protect_image((unsigned long)__start_rodata,
		      PAGE_ALIGN((unsigned long)__end_rodata), KOBOX_IMAGE_READ);
	protect_image((unsigned long)__start_ro_after_init,
		      (unsigned long)__end_ro_after_init, KOBOX_IMAGE_READ);
	/* ELF metadata, GOT and TLS retain their independently owned segments.
	 * Native PMD-gap reclamation cannot describe this shared-object layout.
	 */
	kernel_set_to_readonly = 1;
}

void *kobox_linux_boot_text_alias(void *where, unsigned long size)
{
	const struct kobox_linux_memory_layout *memory;
	unsigned long offset = (unsigned long)where - (unsigned long)_text;
	unsigned long image_size = __bss_stop - _text;

	if (!boot_layout || !size || offset >= image_size ||
	    size > image_size - offset)
		__builtin_trap();
	memory = &boot_layout->task.memory;
	return (char *)memory->direct_map + memory->kernel_image_physical_base +
		offset;
}

static bool image_is_shared(void)
{
	unsigned long *alias = kobox_linux_boot_text_alias(&image_alias_probe,
							 sizeof(image_alias_probe));
	bool shared;

	WRITE_ONCE(*alias, 0x6b6f626f78UL);
	shared = READ_ONCE(image_alias_probe) == 0x6b6f626f78UL;
	WRITE_ONCE(*alias, 0);
	return shared;
}

static void host_console_write(struct console *console, const char *text,
			       unsigned int length)
{
	boot_layout->console_write(boot_layout->console_context, text, length);
}

static struct console host_console = {
	.name = "kobox",
	.write = host_console_write,
	.flags = CON_BOOT | CON_PRINTBUFFER,
	.index = -1,
};

void __init trap_init(void)
{
	int status;

	if (!boot_layout || !irqs_disabled() || raw_smp_processor_id())
		__builtin_trap();
	/* The host owns IDT/GDT/TSS and native exception stacks. Its synchronous
	 * faults enter the Linux exception-table and WARN/BUG paths via this port.
	 */
	status = boot_layout->exceptions_install(kobox_linux_exception_dispatch);
	if (status)
		panic("hosted exception registration failed: %d", status);
}

void __init init_IRQ(void)
{
	if (!irqs_disabled() || raw_smp_processor_id() || nr_legacy_irqs())
		__builtin_trap();
	/* Reuse Linux's vector reservation and CPU-online accounting. Host CPU
	 * notifications already supply the interrupt entry; no IDT/PIC is ours.
	 */
	lapic_assign_system_vectors();
}

void __init setup_arch(char **command_line)
{
	int status;

	if (!boot_layout || !irqs_disabled())
		__builtin_trap();
	/* The hosted platform has no legacy PIC to probe or program. */
	legacy_pic = &null_legacy_pic;
	apic_is_disabled = true;
	/* PCI resources enter through the host bridge, never BIOS, ECAM or
	 * machine-global configuration ports. No such access is granted here.
	 */
	pci_probe = PCI_PROBE_NOEARLY;
	status = kobox_linux_memory_setup_arch();
	if (status)
		panic("hosted RAM setup failed: %d", status);
	if (strscpy(boot_command_line, boot_layout->command_line,
		    COMMAND_LINE_SIZE) < 0)
		panic("hosted command line exceeds COMMAND_LINE_SIZE");
	*command_line = boot_command_line;
	register_console(&host_console);
}

static void __init host_prepare_boot_cpu(void)
{
	kobox_linux_memory_set_cpu(0);
}

static void __init host_prepare_cpus(unsigned int max_cpus)
{
	int status;

	if (max_cpus != KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		panic("hosted boot requires two logical CPUs");
	status = kobox_linux_task_prepare_cpus();
	if (status)
		panic("hosted CPU topology setup failed: %d", status);
}

static void __init host_cpus_done(unsigned int max_cpus)
{
	if (num_online_cpus() != KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		panic("hosted CPUHP bringup incomplete");
}

static void __noreturn host_secondary_entry(void)
{
	unsigned int cpu = raw_smp_processor_id();

	local_irq_disable();
	preempt_count_set(PREEMPT_DISABLED);
	fpu__init_cpu();
	mmgrab(&init_mm);
	current->active_mm = &init_mm;
	cpuhp_ap_sync_alive();
	/* Linux executes its own CPUHP starting callbacks on this CPU. */
	notify_cpu_starting(cpu);
	set_cpu_online(cpu, true);
	kobox_task_clock_init();
	local_irq_enable();
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}

static void __init host_install_secondary_entry(void)
{
	int status = kobox_linux_task_install_secondary_entry(host_secondary_entry);

	if (status)
		panic("hosted AP trampoline registration failed: %d", status);
}

void arch_cpu_idle_exit(void)
{
	kobox_linux_task_idle_exit();
}

void __init time_init(void)
{
	int status = kobox_linux_task_register_clocksource();

	if (status)
		panic("hosted clocksource registration failed: %d", status);
	kobox_task_clock_init();
}

__attribute__((visibility("default")))
int kobox_linux_boot_start(const struct kobox_linux_boot_layout *layout,
			   struct kobox_linux_task_report *report)
{
	int status;

	if (boot_layout || !layout || layout->size != sizeof(*layout) ||
	    !layout->exceptions_install || !layout->command_line ||
	    !layout->console_write || !layout->kernel_main ||
	    !layout->image_protect || !layout->image)
		return -EINVAL;
	status = kobox_linux_task_bind_boot(&layout->task, report);
	if (status)
		return status;
	boot_layout = layout;
	if (!image_is_shared())
		return -EINVAL;
	smp_ops.smp_prepare_boot_cpu = host_prepare_boot_cpu;
	smp_ops.smp_prepare_cpus = host_prepare_cpus;
	smp_ops.smp_cpus_done = host_cpus_done;
	smp_ops.kick_ap_alive = kobox_linux_task_kick_cpu;
	x86_platform.realmode_init = host_install_secondary_entry;
	start_kernel();
}
