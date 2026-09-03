// SPDX-License-Identifier: GPL-2.0-only

__thread unsigned long kobox_provider_percpu_offset;

unsigned long kobox_provider_task_size_limit;

/* enum system_states uses int representation in the pinned Linux profile. */
int system_state;

_Bool early_boot_irqs_disabled;

_Bool initcall_debug;

int nr_ioapics;

int __boot_cpu_id;
unsigned int __num_online_cpus = 2;
unsigned long __cpu_possible_mask = 3;
unsigned long __cpu_online_mask = 3;
unsigned long __cpu_enabled_mask = 3;
unsigned long __cpu_present_mask = 3;
unsigned long __cpu_active_mask = 3;
unsigned long __cpu_dying_mask;
const unsigned long cpu_all_bits = 3;
static unsigned int kobox_cpu_hotplug_disable_depth = 1;
static unsigned char kobox_sparse_irq_locked;

#define KOBOX_CPU_BIT(n) [n + 1] = { 1UL << n }
const unsigned long cpu_bit_bitmap[65][1] = {
	KOBOX_CPU_BIT(0), KOBOX_CPU_BIT(1), KOBOX_CPU_BIT(2), KOBOX_CPU_BIT(3),
	KOBOX_CPU_BIT(4), KOBOX_CPU_BIT(5), KOBOX_CPU_BIT(6), KOBOX_CPU_BIT(7),
	KOBOX_CPU_BIT(8), KOBOX_CPU_BIT(9), KOBOX_CPU_BIT(10), KOBOX_CPU_BIT(11),
	KOBOX_CPU_BIT(12), KOBOX_CPU_BIT(13), KOBOX_CPU_BIT(14), KOBOX_CPU_BIT(15),
	KOBOX_CPU_BIT(16), KOBOX_CPU_BIT(17), KOBOX_CPU_BIT(18), KOBOX_CPU_BIT(19),
	KOBOX_CPU_BIT(20), KOBOX_CPU_BIT(21), KOBOX_CPU_BIT(22), KOBOX_CPU_BIT(23),
	KOBOX_CPU_BIT(24), KOBOX_CPU_BIT(25), KOBOX_CPU_BIT(26), KOBOX_CPU_BIT(27),
	KOBOX_CPU_BIT(28), KOBOX_CPU_BIT(29), KOBOX_CPU_BIT(30), KOBOX_CPU_BIT(31),
	KOBOX_CPU_BIT(32), KOBOX_CPU_BIT(33), KOBOX_CPU_BIT(34), KOBOX_CPU_BIT(35),
	KOBOX_CPU_BIT(36), KOBOX_CPU_BIT(37), KOBOX_CPU_BIT(38), KOBOX_CPU_BIT(39),
	KOBOX_CPU_BIT(40), KOBOX_CPU_BIT(41), KOBOX_CPU_BIT(42), KOBOX_CPU_BIT(43),
	KOBOX_CPU_BIT(44), KOBOX_CPU_BIT(45), KOBOX_CPU_BIT(46), KOBOX_CPU_BIT(47),
	KOBOX_CPU_BIT(48), KOBOX_CPU_BIT(49), KOBOX_CPU_BIT(50), KOBOX_CPU_BIT(51),
	KOBOX_CPU_BIT(52), KOBOX_CPU_BIT(53), KOBOX_CPU_BIT(54), KOBOX_CPU_BIT(55),
	KOBOX_CPU_BIT(56), KOBOX_CPU_BIT(57), KOBOX_CPU_BIT(58), KOBOX_CPU_BIT(59),
	KOBOX_CPU_BIT(60), KOBOX_CPU_BIT(61), KOBOX_CPU_BIT(62), KOBOX_CPU_BIT(63),
};
#undef KOBOX_CPU_BIT

void cpus_read_lock(void)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
}

int cpus_read_trylock(void)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return 1;
}

void cpus_read_unlock(void)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
}

void cpus_write_lock(void)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void cpus_write_unlock(void)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
}

void lockdep_assert_cpus_held(void)
{
	/* Read-side exclusion is unconditional for an immutable topology. */
}

void cpu_hotplug_disable_offlining(void)
{
	__atomic_store_n(&kobox_cpu_hotplug_disable_depth, 1, __ATOMIC_RELEASE);
}

void cpu_hotplug_disable(void)
{
	(void)__atomic_add_fetch(&kobox_cpu_hotplug_disable_depth, 1,
				 __ATOMIC_ACQ_REL);
}

void cpu_hotplug_enable(void)
{
	unsigned int depth = __atomic_load_n(&kobox_cpu_hotplug_disable_depth,
					     __ATOMIC_ACQUIRE);

	while (depth > 1 &&
	       !__atomic_compare_exchange_n(&kobox_cpu_hotplug_disable_depth,
					    &depth, depth - 1, 0,
					    __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE))
		;
}

unsigned long loops_per_jiffy = 1UL << 12;

unsigned int arch_dynirq_lower_bound(unsigned int from)
{
	return from;
}

int early_pci_allowed(void)
{
	return 0;
}

int irq_affinity_online_cpu(unsigned int cpu)
{
	/* The two-CPU provider topology is immutable after construction. */
	(void)cpu;
	return 0;
}

void irq_migrate_all_off_this_cpu(void)
{
	/* An immutable provider topology has no CPU-offline transition. */
}

void irq_lock_sparse(void)
{
	while (__atomic_test_and_set(&kobox_sparse_irq_locked,
				     __ATOMIC_ACQUIRE))
		;
}

void irq_unlock_sparse(void)
{
	__atomic_clear(&kobox_sparse_irq_locked, __ATOMIC_RELEASE);
}

void kobox_provider_set_percpu_offset(unsigned long offset)
{
	kobox_provider_percpu_offset = offset;
}

unsigned long kobox_provider_get_percpu_offset(void)
{
	return kobox_provider_percpu_offset;
}

void kobox_provider_set_task_size_limit(unsigned long limit)
{
	kobox_provider_task_size_limit = limit;
}

unsigned long kobox_provider_get_task_size_limit(void)
{
	return kobox_provider_task_size_limit;
}
