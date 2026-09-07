// SPDX-License-Identifier: GPL-2.0-only

/* Linker/TLS test data only. This object is never part of a Linux runtime. */
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <asm/current.h>
#include <asm/processor.h>
#include <asm/sync_core.h>

DEFINE_PER_CPU_CACHE_HOT(struct task_struct *, current_task);
DEFINE_PER_CPU_CACHE_HOT(unsigned long, cpu_current_top_of_stack);
u64 jiffies_64;

const unsigned long fixture_page_size = PAGE_SIZE;
const unsigned long fixture_thread_size = THREAD_SIZE;
const unsigned long fixture_stack_padding =
	TOP_OF_KERNEL_STACK_PADDING + sizeof(struct pt_regs);
const unsigned long fixture_rodata = 0x12345678;
unsigned long fixture_data = 0x87654321;
unsigned long fixture_ro_after_init __ro_after_init = 99;
unsigned long fixture_init_data __initdata = 101;
static __thread unsigned int fixture_tls = 7;

static int __init fixture_early(void)
{
	return 11;
}
early_initcall(fixture_early);

static int __init fixture_device(void)
{
	return 22;
}
device_initcall(fixture_device);

unsigned int fixture_advance(void)
{
	sync_core();
	return ++fixture_tls;
}
