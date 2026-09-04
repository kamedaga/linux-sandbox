// SPDX-License-Identifier: GPL-2.0-only

#include "host.h"

#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/topology.h>
#include <linux/vmalloc.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/processor.h>

#define KOBOX_MEMORY_MINIMUM_RAM_SIZE (128UL << 20)
#define KOBOX_MEMORY_TEST_SLAB_SIZE 173U
#define KOBOX_MEMORY_MAX_EARLY_CPUHP_REGISTRATIONS 8U

extern char _text[];
extern char __bss_stop[];
extern char __per_cpu_start[];
extern void mm_core_init(void);
extern void radix_tree_init(void);
extern void sparse_init(void);

unsigned long page_offset_base;
unsigned long vmalloc_base;
unsigned long vmemmap_base;
unsigned long phys_base;
pteval_t __default_kernel_pte_mask = ~0UL;
pteval_t __supported_pte_mask = ~0UL;
unsigned int __pgtable_l5_enabled;
unsigned int pgdir_shift = 39;
unsigned int ptrs_per_p4d = 1;
pgd_t init_top_pgt[PTRS_PER_PGD] __aligned(PAGE_SIZE);
pud_t level3_kernel_pgt[PTRS_PER_PUD] __aligned(PAGE_SIZE);
pmd_t level2_kernel_pgt[PTRS_PER_PMD] __aligned(PAGE_SIZE);
unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)]
	__aligned(PAGE_SIZE);
struct task_struct init_task;
struct cpuinfo_x86 boot_cpu_data = {
	.x86_virt_bits = 48,
	.x86_phys_bits = 52,
	.x86_cache_alignment = SMP_CACHE_BYTES,
	.x86_clflush_size = SMP_CACHE_BYTES,
	.x86_cache_bits = 52,
};
unsigned long __per_cpu_offset[NR_CPUS] __ro_after_init;
DEFINE_PER_CPU_CACHE_HOT(int, cpu_number);
DEFINE_PER_CPU_CACHE_HOT(int, __preempt_count) = INIT_PREEMPT_COUNT;
DEFINE_PER_CPU_CACHE_HOT(struct task_struct *, current_task) =
	&init_task;
unsigned long initial_code;
unsigned int smpboot_control;
unsigned int __max_threads_per_core = 1;
int __max_smt_threads = 1;
struct cpumask __cpu_primary_thread_mask;
struct cpumask __cpu_possible_mask;
struct cpumask __cpu_online_mask;
struct cpumask __cpu_enabled_mask;
struct cpumask __cpu_present_mask;
struct cpumask __cpu_active_mask;
struct cpumask __cpu_dying_mask;
atomic_t __num_online_cpus = ATOMIC_INIT(1);
unsigned int nr_cpu_ids = KOBOX_LINUX_MEMORY_LOGICAL_CPUS;
int __boot_cpu_id;
int after_bootmem;
int kernel_set_to_readonly;
enum system_states system_state = SYSTEM_BOOTING;
u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;
atomic_long_t _totalram_pages;
unsigned long totalreserve_pages;
unsigned long totalcma_pages;
unsigned long max_pfn_mapped;
DEFINE_SPINLOCK(pgd_lock);

static __thread unsigned long kobox_percpu_offset;
static __thread unsigned int kobox_irq_disable_depth = 1;
static const struct kobox_linux_memory_layout *memory_layout;
static DEFINE_PER_CPU(unsigned long, kobox_memory_static_percpu);
static struct {
	enum cpuhp_state state;
	const char *name;
	int (*startup)(unsigned int cpu);
	int (*teardown)(unsigned int cpu);
} early_cpuhp_registrations[KOBOX_MEMORY_MAX_EARLY_CPUHP_REGISTRATIONS];
static unsigned int early_cpuhp_registration_count;

unsigned long kobox_provider_current_percpu_offset(void)
{
	return kobox_percpu_offset;
}

unsigned long kobox_provider_get_task_size_limit(void)
{
	return (1UL << 47) - PAGE_SIZE;
}

unsigned long kobox_provider_irq_save_flags(void)
{
	return kobox_irq_disable_depth != 0;
}

void kobox_provider_irq_disable(void)
{
	kobox_irq_disable_depth++;
}

void kobox_provider_irq_enable(void)
{
	if (!kobox_irq_disable_depth)
		BUG();
	kobox_irq_disable_depth--;
}

unsigned long kobox_provider_irq_save(void)
{
	unsigned long flags = kobox_provider_irq_save_flags();

	kobox_provider_irq_disable();
	return flags;
}

void kobox_provider_irq_restore(unsigned long flags)
{
	if (kobox_irq_disable_depth)
		kobox_irq_disable_depth--;
	if (flags && !kobox_irq_disable_depth)
		kobox_irq_disable_depth = 1;
}

int __cond_resched(void)
{
	/* start_kernel reaches mm_core_init() before sched_init(). */
	if (should_resched(0))
		BUG();
	return 0;
}

static int mapped_range(unsigned long start, unsigned long end,
			void *window, unsigned long window_base)
{
	unsigned long address;

	if (!memory_layout || start > end || start < window_base ||
	    end - window_base > memory_layout->vmalloc_size)
		return -EINVAL;
	for (address = start; address < end; address += PAGE_SIZE) {
		unsigned long pfn = vmalloc_to_pfn((void *)address);
		void *mapped;
		int status;

		if (pfn >= max_pfn)
			return -ERANGE;
		status = memory_layout->operations->map(
			window, address - window_base,
			memory_layout->ram_backing, pfn << PAGE_SHIFT,
			PAGE_SIZE, KOBOX_LINUX_MEMORY_READ |
			KOBOX_LINUX_MEMORY_WRITE, &mapped);
		if (status || mapped != (void *)address)
			return -EIO;
	}
	return 0;
}

void kobox_provider_flush_cache_vmap(unsigned long start, unsigned long end)
{
	if (mapped_range(start, end, memory_layout->vmalloc_window,
			 vmalloc_base))
		BUG();
}

void kobox_provider_flush_cache_vunmap(unsigned long start, unsigned long end)
{
	if (!memory_layout || start > end || start < vmalloc_base ||
	    end - vmalloc_base > memory_layout->vmalloc_size ||
	    memory_layout->operations->reset(memory_layout->vmalloc_window,
		start - vmalloc_base, end - start))
		BUG();
}

void arch_sync_kernel_mappings(unsigned long start, unsigned long end)
{
	(void)start;
	(void)end;
}

void __flush_tlb_all(void)
{
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	(void)start;
	(void)end;
}

void pcpu_populate_pte(unsigned long address)
{
	(void)address;
}

void pgtable_cache_init(void)
{
}

void arch_mm_preinit(void)
{
}

void mem_init(void)
{
	after_bootmem = 1;
}

void pti_init(void)
{
}

void alternatives_enable_smp(void)
{
}

int __cpuhp_setup_state(enum cpuhp_state state, const char *name, bool invoke,
			int (*startup)(unsigned int cpu),
			int (*teardown)(unsigned int cpu), bool multi_instance)
{
	unsigned int index;

	if (invoke || multi_instance)
		return -EOPNOTSUPP;
	for (index = 0; index < early_cpuhp_registration_count; index++) {
		if (early_cpuhp_registrations[index].state == state)
			return -EEXIST;
	}
	if (early_cpuhp_registration_count >=
	    ARRAY_SIZE(early_cpuhp_registrations))
		return -ENOSPC;
	early_cpuhp_registrations[early_cpuhp_registration_count++] =
		(typeof(*early_cpuhp_registrations)) {
			.state = state,
			.name = name,
			.startup = startup,
			.teardown = teardown,
		};
	return 0;
}

bool pfn_range_is_mapped(unsigned long start_pfn, unsigned long end_pfn)
{
	return start_pfn <= end_pfn && end_pfn <= max_pfn;
}

static int pcpu_cpu_distance(unsigned int from, unsigned int to)
{
	(void)from;
	(void)to;
	return LOCAL_DISTANCE;
}

static int pcpu_cpu_to_node(int cpu)
{
	(void)cpu;
	return 0;
}

void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int status;

	status = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
		PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, pcpu_cpu_distance,
		pcpu_cpu_to_node);
	if (status)
		BUG();
	delta = (unsigned long)pcpu_base_addr -
		(unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu) {
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
		per_cpu(cpu_number, cpu) = cpu;
	}
}

static void __init setup_memory_zones(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES] = { 0 };

#ifdef CONFIG_ZONE_DMA
	max_zone_pfns[ZONE_DMA] = min(MAX_DMA_PFN, max_low_pfn);
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = min(MAX_DMA32_PFN, max_low_pfn);
#endif
	max_zone_pfns[ZONE_NORMAL] = max_low_pfn;
	free_area_init(max_zone_pfns);
}

int vmemmap_populate(unsigned long start, unsigned long end, int node,
		     struct vmem_altmap *altmap)
{
	phys_addr_t physical;
	void *mapped;
	size_t size;
	int status;

	(void)node;
	if (altmap || !memory_layout || start >= end ||
	    !PAGE_ALIGNED(start) || !PAGE_ALIGNED(end) ||
	    start < vmemmap_base ||
	    end - vmemmap_base > memory_layout->vmemmap_size)
		return -EINVAL;
	size = end - start;
	physical = memblock_phys_alloc_range(size, PAGE_SIZE, PAGE_SIZE,
					     memory_layout->ram_size);
	if (!physical)
		return -ENOMEM;
	status = memory_layout->operations->map(
		memory_layout->vmemmap_window, start - vmemmap_base,
		memory_layout->ram_backing, physical, size,
		KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE, &mapped);
	if (status || mapped != (void *)start)
		return -ENOMEM;
	return 0;
}

void vmemmap_populate_print_last(void)
{
}

static int validate_layout(const struct kobox_linux_memory_layout *layout)
{
	unsigned long image_size = PAGE_ALIGN(__bss_stop - _text);

	if (!layout || layout->size != sizeof(*layout) ||
	    layout->identity != KOBOX_LINUX_MEMORY_HOST_IDENTITY ||
	    !layout->operations ||
	    layout->operations->size != sizeof(*layout->operations) ||
	    layout->operations->identity != KOBOX_LINUX_MEMORY_HOST_IDENTITY ||
	    !layout->operations->map || !layout->operations->reset ||
	    !layout->ram_backing || !layout->vmemmap_window ||
	    !layout->vmalloc_window || !layout->direct_map ||
	    !layout->vmemmap_base || !layout->vmalloc_base ||
	    layout->ram_size < KOBOX_MEMORY_MINIMUM_RAM_SIZE ||
	    !PAGE_ALIGNED(layout->ram_size) ||
	    !PAGE_ALIGNED((unsigned long)layout->direct_map) ||
	    !PAGE_ALIGNED((unsigned long)layout->vmemmap_base) ||
	    !PAGE_ALIGNED(layout->vmemmap_size) ||
	    !PAGE_ALIGNED((unsigned long)layout->vmalloc_base) ||
	    !PAGE_ALIGNED(layout->vmalloc_size) ||
	    !PAGE_ALIGNED(layout->kernel_image_physical_base) ||
	    layout->kernel_image_physical_base > layout->ram_size ||
	    image_size > layout->ram_size - layout->kernel_image_physical_base)
		return -EINVAL;
	return 0;
}

static int prepare_memory(const struct kobox_linux_memory_layout *layout)
{
	unsigned long image_size = PAGE_ALIGN(__bss_stop - _text);
	int status;

	memory_layout = layout;
	page_offset_base = (unsigned long)layout->direct_map;
	vmemmap_base = (unsigned long)layout->vmemmap_base;
	vmalloc_base = (unsigned long)layout->vmalloc_base;
	phys_base = __START_KERNEL_map - (unsigned long)_text +
		layout->kernel_image_physical_base;

	status = memblock_add(0, layout->ram_size);
	if (status)
		return status;
	status = memblock_reserve(0, PAGE_SIZE);
	if (status)
		return status;
	status = memblock_reserve(layout->kernel_image_physical_base,
				  image_size);
	if (status)
		return status;
	status = memblock_set_node(0, layout->ram_size, &memblock.memory, 0);
	if (status)
		return status;

	min_low_pfn = 0;
	max_pfn = PFN_DOWN(layout->ram_size);
	max_low_pfn = max_pfn;
	max_possible_pfn = max_pfn;
	max_pfn_mapped = max_pfn;
	high_memory = (void *)(page_offset_base + layout->ram_size);

	sparse_init();
	return 0;
}

static int prepare_percpu(void)
{
	cpumask_clear(&__cpu_possible_mask);
	cpumask_clear(&__cpu_online_mask);
	cpumask_clear(&__cpu_enabled_mask);
	cpumask_clear(&__cpu_present_mask);
	cpumask_clear(&__cpu_active_mask);
	cpumask_clear(&__cpu_dying_mask);
	cpumask_set_cpu(0, &__cpu_possible_mask);
	cpumask_set_cpu(0, &__cpu_online_mask);
	cpumask_set_cpu(0, &__cpu_enabled_mask);
	cpumask_set_cpu(0, &__cpu_present_mask);
	cpumask_set_cpu(0, &__cpu_active_mask);
	set_cpu_possible(1, true);
	set_cpu_enabled(1, true);
	set_cpu_present(1, true);
	cpumask_copy(&__cpu_primary_thread_mask, cpu_possible_mask);
	if (nr_cpu_ids != KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -EINVAL;
	setup_per_cpu_areas();
	kobox_percpu_offset = __per_cpu_offset[0];
	return 0;
}

static int run_gate(struct kobox_linux_memory_report *report)
{
	struct page *pages[2] = { NULL, NULL };
	struct kmem_cache *cache;
	unsigned long *dynamic_percpu;
	unsigned long *direct[2];
	unsigned long *alias;
	unsigned long pfn;
	void *heap;
	void *slab;
	int status = -EINVAL;

	pages[0] = alloc_page(GFP_KERNEL | __GFP_ZERO);
	pages[1] = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!pages[0] || !pages[1])
		goto out_pages;
	pfn = page_to_pfn(pages[0]);
	report->allocated_pfn = pfn;
	report->page_pfn_roundtrip = pfn_to_page(pfn) == pages[0];
	direct[0] = page_address(pages[0]);
	direct[1] = page_address(pages[1]);
	report->direct_map_roundtrip =
		(unsigned long)direct[0] == page_offset_base + (pfn << PAGE_SHIFT) &&
		virt_to_page(direct[0]) == pages[0];
	if (!report->page_pfn_roundtrip || !report->direct_map_roundtrip)
		goto out_pages;

	heap = kmalloc(8193, GFP_KERNEL);
	if (!heap)
		goto out_pages;
	memset(heap, 0x5a, 8193);
	report->kmalloc_ready = ((unsigned char *)heap)[8192] == 0x5a;
	kfree(heap);
	if (!report->kmalloc_ready)
		goto out_pages;

	cache = kmem_cache_create("kobox-memory-gate",
		KOBOX_MEMORY_TEST_SLAB_SIZE, 0, 0, NULL);
	if (!cache)
		goto out_pages;
	slab = kmem_cache_alloc(cache, GFP_KERNEL);
	if (!slab)
		goto out_pages;
	memset(slab, 0xa6, KOBOX_MEMORY_TEST_SLAB_SIZE);
	report->slub_ready =
		((unsigned char *)slab)[KOBOX_MEMORY_TEST_SLAB_SIZE - 1] == 0xa6;
	kmem_cache_free(cache, slab);
	if (!report->slub_ready)
		goto out_pages;

	per_cpu(kobox_memory_static_percpu, 0) = 0x10203040UL;
	per_cpu(kobox_memory_static_percpu, 1) = 0x50607080UL;
	report->static_percpu_ready =
		per_cpu(kobox_memory_static_percpu, 0) == 0x10203040UL &&
		per_cpu(kobox_memory_static_percpu, 1) == 0x50607080UL &&
		per_cpu_ptr(&kobox_memory_static_percpu, 0) !=
		per_cpu_ptr(&kobox_memory_static_percpu, 1);
	if (!report->static_percpu_ready)
		goto out_pages;

	dynamic_percpu = __alloc_percpu(sizeof(*dynamic_percpu),
					 __alignof__(*dynamic_percpu));
	if (!dynamic_percpu)
		goto out_pages;
	*per_cpu_ptr(dynamic_percpu, 0) = 0x11223344UL;
	*per_cpu_ptr(dynamic_percpu, 1) = 0x55667788UL;
	report->dynamic_percpu_ready =
		*per_cpu_ptr(dynamic_percpu, 0) == 0x11223344UL &&
		*per_cpu_ptr(dynamic_percpu, 1) == 0x55667788UL &&
		per_cpu_ptr(dynamic_percpu, 0) != per_cpu_ptr(dynamic_percpu, 1);
	if (!report->dynamic_percpu_ready)
		goto out_pages;

	direct[0][0] = 0x88776655UL;
	direct[1][0] = 0x44332211UL;
	alias = vmap(pages, ARRAY_SIZE(pages), VM_MAP, PAGE_KERNEL);
	if (!alias)
		goto out_pages;
	report->vmap_alias_ready =
		alias[0] == direct[0][0] &&
		alias[PAGE_SIZE / sizeof(*alias)] == direct[1][0];
	alias[1] = 0xdeadbeefUL;
	alias[PAGE_SIZE / sizeof(*alias) + 1] = 0xcafef00dUL;
	report->vmap_alias_ready = report->vmap_alias_ready &&
		direct[0][1] == 0xdeadbeefUL && direct[1][1] == 0xcafef00dUL;
	if (!report->vmap_alias_ready)
		goto out_pages;
	status = 0;

out_pages:
	if (pages[1])
		__free_page(pages[1]);
	if (pages[0])
		__free_page(pages[0]);
	return status;
}

__attribute__((visibility("default")))
int kobox_linux_memory_early_boot(
	const struct kobox_linux_memory_layout *layout,
	struct kobox_linux_memory_report *report)
{
	bool page_alloc_cpuhp = false;
	bool radix_cpuhp = false;
	unsigned int index;
	int status;

	if (!report || report->size != sizeof(*report) ||
	    report->identity != KOBOX_LINUX_MEMORY_HOST_IDENTITY)
		return -EINVAL;
	status = validate_layout(layout);
	if (status)
		return status;
	status = prepare_memory(layout);
	if (status)
		return status;
	status = prepare_percpu();
	if (status)
		return status;
	setup_memory_zones();
	mm_core_init();
	radix_tree_init();
	report->ram_size = layout->ram_size;
	report->page_offset_base = page_offset_base;
	report->vmemmap_base = vmemmap_base;
	report->vmalloc_base = vmalloc_base;
	report->phys_base = phys_base;
	report->logical_cpu_count = nr_cpu_ids;
	report->early_cpuhp_registration_count =
		early_cpuhp_registration_count;
	for (index = 0; index < early_cpuhp_registration_count; index++) {
		page_alloc_cpuhp |= early_cpuhp_registrations[index].state ==
			CPUHP_PAGE_ALLOC;
		radix_cpuhp |= early_cpuhp_registrations[index].state ==
			CPUHP_RADIX_DEAD;
	}
	report->early_cpuhp_registrations_ready =
		page_alloc_cpuhp && radix_cpuhp;
	report->kernel_image_translation_ready =
		__pa_symbol(_text) == layout->kernel_image_physical_base;
	report->mm_core_initialized = slab_is_available();
	if (!report->mm_core_initialized ||
	    !report->kernel_image_translation_ready ||
	    !report->early_cpuhp_registrations_ready)
		return -EINVAL;
	return run_gate(report);
}
