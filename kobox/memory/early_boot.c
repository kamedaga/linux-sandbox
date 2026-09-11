// SPDX-License-Identifier: GPL-2.0-only

#define KOBOX_HOSTED_RAM 1

#include "host.h"
#include "../arch/x86_64/host_call.h"
#include "port.h"
#ifdef KOBOX_BOOT_RUNTIME
#include "../mm/port.h"
#include "../arch/x86_64/user_layout.h"
#include "mmio.h"
#endif

#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/export.h>
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
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/processor.h>

#ifndef KOBOX_BOOT_RUNTIME
/* Isolated pre-boot fixtures have no panic console. Mapping failure is
 * nevertheless fatal; it must never be reported as a completed operation.
 */
#define panic(...) __builtin_trap()
#endif

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
unsigned long kobox_memory_vmalloc_end;
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
#ifndef KOBOX_TASK_PORT_PHASE
struct task_struct init_task;
#endif
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
#ifndef KOBOX_BOOT_RUNTIME
struct cpumask __cpu_possible_mask;
struct cpumask __cpu_online_mask;
struct cpumask __cpu_enabled_mask;
struct cpumask __cpu_present_mask;
struct cpumask __cpu_active_mask;
struct cpumask __cpu_dying_mask;
atomic_t __num_online_cpus = ATOMIC_INIT(1);
unsigned int nr_cpu_ids = KOBOX_LINUX_MEMORY_LOGICAL_CPUS;
int __boot_cpu_id;
#endif
int after_bootmem;
int kernel_set_to_readonly;
#ifndef KOBOX_BOOT_RUNTIME
enum system_states system_state = SYSTEM_BOOTING;
#endif
#ifndef KOBOX_TASK_PORT_PHASE
u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;
#endif
#ifndef KOBOX_BOOT_RUNTIME
atomic_long_t _totalram_pages;
unsigned long totalreserve_pages;
unsigned long totalcma_pages;
#endif
unsigned long max_pfn_mapped;
DEFINE_SPINLOCK(pgd_lock);

#ifdef KOBOX_TASK_PORT_PHASE
__thread unsigned long kobox_percpu_offset;
#else
static __thread unsigned long kobox_percpu_offset;
#endif
#ifndef KOBOX_TASK_PORT_PHASE
static __thread unsigned int kobox_irq_disable_depth = 1;
#endif
static const struct kobox_linux_memory_layout *memory_layout;

bool kobox_linux_memory_address_is_ram(unsigned long address)
{
	if (!memory_layout)
		return false;
	return address - (unsigned long)_text <
		PAGE_ALIGN(__bss_stop - _text) ||
		address - page_offset_base < memory_layout->ram_size;
}
EXPORT_SYMBOL(kobox_linux_memory_address_is_ram);

unsigned long kobox_linux_memory_phys_addr(unsigned long address)
{
	unsigned long offset = address - (unsigned long)_text;

	if (!memory_layout)
		__builtin_trap();
	/* Both aliases are host-chosen positive addresses. Native x86's
	 * carry test against __START_KERNEL_map cannot distinguish them.
	 */
	if (offset < PAGE_ALIGN(__bss_stop - _text))
		return memory_layout->kernel_image_physical_base + offset;
	return address - page_offset_base;
}
EXPORT_SYMBOL(kobox_linux_memory_phys_addr);
static DEFINE_RAW_SPINLOCK(kernel_alias_lock);
static bool direct_tables_ready;
#ifndef KOBOX_BOOT_RUNTIME
static DEFINE_PER_CPU(unsigned long, kobox_memory_static_percpu);
#endif
#ifndef KOBOX_TASK_PORT_PHASE
static struct {
	enum cpuhp_state state;
	const char *name;
	int (*startup)(unsigned int cpu);
	int (*teardown)(unsigned int cpu);
} early_cpuhp_registrations[KOBOX_MEMORY_MAX_EARLY_CPUHP_REGISTRATIONS];
static unsigned int early_cpuhp_registration_count;
#endif

unsigned long kobox_provider_current_percpu_offset(void)
{
	return READ_ONCE(kobox_percpu_offset);
}

#ifdef KOBOX_TASK_PORT_PHASE
void kobox_linux_memory_set_cpu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		BUG();
	kobox_percpu_offset = __per_cpu_offset[cpu];
}
#endif

unsigned long kobox_provider_get_task_size_limit(void)
{
#ifdef KOBOX_BOOT_RUNTIME
	return KOBOX_X86_USER_END;
#else
	return (1UL << 47) - PAGE_SIZE;
#endif
}

#ifndef KOBOX_TASK_PORT_PHASE
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
#endif

static pte_t kernel_pte(unsigned long address)
{
	unsigned int level;
	bool nx, rw;
	pte_t *entry, pte;

	entry = lookup_address_in_pgd_attr(pgd_offset_k(address), address,
					   &level, &nx, &rw);
	if (!entry)
		return __pte(0);
	switch (level) {
	case PG_LEVEL_4K:
		pte = ptep_get(entry);
		break;
	case PG_LEVEL_2M: {
		pmd_t pmd = pmdp_get((pmd_t *)entry);

		pte = pfn_pte(pmd_pfn(pmd) + ((address & ~PMD_MASK) >> PAGE_SHIFT),
			      pgprot_large_2_4k(__pgprot(pmd_flags(pmd))));
		break;
	}
	case PG_LEVEL_1G: {
		pud_t pud = pudp_get((pud_t *)entry);

		pte = pfn_pte(pud_pfn(pud) + ((address & ~PUD_MASK) >> PAGE_SHIFT),
			      pgprot_large_2_4k(__pgprot(pud_flags(pud))));
		break;
	}
	default:
		if (pte_flags(ptep_get(entry)) & _PAGE_PRESENT)
			panic("unsupported hosted kernel page-table level %u", level);
		return __pte(0);
	}
	/* Normalize only the host's translation view, not Linux's page tables.
	 * Native huge-leaf PFN/PAT decoding and ancestor permissions both apply.
	 */
	if (nx)
		pte = pte_set_flags(pte, _PAGE_NX);
	if (!rw)
		pte = pte_clear_flags(pte, _PAGE_RW);

	return pte;
}

static unsigned int host_pte_protection(pte_t pte)
{
	unsigned int protection = 0;

	if (pte_flags(pte) & _PAGE_PRESENT) {
		protection |= KOBOX_LINUX_MEMORY_READ;
		if (pte_write(pte))
			protection |= KOBOX_LINUX_MEMORY_WRITE;
		if (pte_exec(pte))
			protection |= KOBOX_LINUX_MEMORY_EXECUTE;
	}
	return protection;
}

static int mapped_range(unsigned long start, unsigned long end,
			void *window, unsigned long window_base, bool publish_mmio)
{
	unsigned long address;

	if (!memory_layout || start > end || start < window_base ||
	    end - window_base > memory_layout->vmalloc_size ||
	    offset_in_page(start) || offset_in_page(end))
		return -EINVAL;
	for (address = start; address < end; address += PAGE_SIZE) {
		pte_t pte = kernel_pte(address);
		unsigned long pfn;
		void *mapped;
		int status;

		/* per-CPU flushes span units and their unpopulated gaps. */
		if (!host_pte_protection(pte))
			continue;
		pfn = pte_pfn(pte);
		if (pfn >= max_pfn) {
#ifdef KOBOX_BOOT_RUNTIME
			status = kobox_mmio_publish(address, pte, publish_mmio);
			if (status)
				return status;
			continue;
#else
			return -ERANGE;
#endif
		}
		status = kobox_host_call(memory_layout->operations->map(
			window, address - window_base,
			memory_layout->ram_backing, pfn << PAGE_SHIFT,
			PAGE_SIZE, host_pte_protection(pte), &mapped));
		if (status || mapped != (void *)address)
			return -EIO;
	}
	return 0;
}

void kobox_provider_flush_cache_vmap(unsigned long start, unsigned long end)
{
	unsigned long flags;
	int status;

	/* Serialize host publication with invalidation of overlapping flush spans.
	 * Host map/reset are leaf memory operations, never guest callbacks.
	 */
	raw_spin_lock_irqsave(&kernel_alias_lock, flags);
	status = mapped_range(start, end, memory_layout->vmalloc_window,
			      vmalloc_base, false);
	raw_spin_unlock_irqrestore(&kernel_alias_lock, flags);
	if (status)
		panic("hosted vmap publication failed: %lx-%lx window %lx error %d",
		      start, end, vmalloc_base, status);
}

#ifdef KOBOX_BOOT_RUNTIME
int kobox_linux_memory_publish(unsigned long start, unsigned long end)
{
	unsigned long flags;
	int status;

	raw_spin_lock_irqsave(&kernel_alias_lock, flags);
	status = mapped_range(start, end, memory_layout->vmalloc_window,
			      vmalloc_base, true);
	raw_spin_unlock_irqrestore(&kernel_alias_lock, flags);
	return status;
}
#endif

void kobox_provider_flush_cache_vunmap(unsigned long start, unsigned long end)
{
	/* Coherent x86 needs no pre-unmap cache operation. Linux has not cleared
	 * PTEs yet, and a per-CPU span can include live pages between its units.
	 * Revoke host aliases at the post-clear TLB boundary instead.
	 */
	if (!memory_layout || start > end || start < vmalloc_base ||
	    end - vmalloc_base > memory_layout->vmalloc_size)
		BUG();
}

void arch_sync_kernel_mappings(unsigned long start, unsigned long end)
{
	/*
	 * Hosted kernel translations belong solely to init_mm and the runtime
	 * process. Guest mms have independent low addresses, even where a host
	 * RAM/vmalloc alias has the same numeric address. Never replicate those
	 * PGD entries into a guest. The host kernel mapping is published at the
	 * actual TLB/alias boundary below.
	 */
	(void)start;
	(void)end;
}

void __flush_tlb_all(void)
{
#ifdef KOBOX_BOOT_RUNTIME
	kobox_vm_flush_all();
#endif
	flush_tlb_kernel_range(0, ULONG_MAX);
}

static void protect_direct_range(unsigned long start, unsigned long end)
{
	unsigned long address, begin;
	unsigned int protection;

	if (!direct_tables_ready)
		return;
	start = max(start, page_offset_base);
	end = min(end, page_offset_base + memory_layout->ram_size);
	for (address = start; address < end; ) {
		begin = address;
		protection = host_pte_protection(kernel_pte(address));
		do {
			address += PAGE_SIZE;
		} while (address < end &&
			 host_pte_protection(kernel_pte(address)) == protection);
		if (kobox_host_call(memory_layout->operations->protect(memory_layout->direct_window,
			begin - page_offset_base, address - begin, protection)))
			panic("hosted direct-map protection failed");
	}
}

static int reset_kernel_window(void *context, unsigned long start,
			       unsigned long end)
{
	return kobox_host_call(memory_layout->operations->reset(context,
					       start - vmalloc_base, end - start));
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned long address, unmapped, flags;

	if (!memory_layout)
		BUG();
	if (start >= end)
		return;
	if (offset_in_page(start) || (end != ULONG_MAX && offset_in_page(end)))
		BUG();
	/* CPA keeps its primary address in start even when aliases require a
	 * global flush. The sentinel covers every window, including a direct
	 * map below that address; it is not the upper bound of a range.
	 */
	if (end == TLB_FLUSH_ALL)
		start = 0;
	raw_spin_lock_irqsave(&kernel_alias_lock, flags);
	protect_direct_range(start, end);
	/* The host owns translations for the direct map and core image. This
	 * boundary reconciles Linux's dynamic kernel aliases with its real PTEs.
	 * Lazy vmalloc invalidation is safe only if still-present pages inside
	 * a coalesced flush span retain their mappings.
	 */
	start = max(start, vmalloc_base);
	/* VMALLOC_END is inclusive; cache/TLB ranges exclude their end. */
	end = min(end, kobox_memory_vmalloc_end + 1);
	if (start >= end)
		goto out_unlock;
	/* A concurrent publisher must not map a page between this PTE snapshot
	 * and host reset. Its PTE may change, but its host map follows this reset.
	 */
	for (address = start; address < end; ) {
		if (host_pte_protection(kernel_pte(address))) {
			if (mapped_range(address, address + PAGE_SIZE,
					 memory_layout->vmalloc_window, vmalloc_base, false))
				panic("hosted kernel PTE protection failed");
			address += PAGE_SIZE;
			continue;
		}
		unmapped = address;
		do {
			address += PAGE_SIZE;
		} while (address < end && !host_pte_protection(kernel_pte(address)));
#ifdef KOBOX_BOOT_RUNTIME
		if (kobox_mmio_reset(unmapped, address, memory_layout->vmalloc_window,
				     reset_kernel_window))
			panic("hosted kernel alias invalidation failed");
#else
		if (reset_kernel_window(memory_layout->vmalloc_window, unmapped, address))
			BUG();
#endif
	}
out_unlock:
	raw_spin_unlock_irqrestore(&kernel_alias_lock, flags);
}

#ifdef KOBOX_BOOT_RUNTIME
void kobox_linux_memory_image_permissions(unsigned long offset,
					 unsigned long length, bool writable)
{
	unsigned long start, end, address, flags;

	if (!memory_layout || !direct_tables_ready || offset_in_page(offset) ||
	    !length || offset_in_page(length) ||
	    offset >= (unsigned long)(__bss_stop - _text) ||
	    length > (unsigned long)(__bss_stop - _text) - offset)
		panic("invalid hosted core direct-map protection");
	start = page_offset_base + memory_layout->kernel_image_physical_base + offset;
	end = start + length;
	raw_spin_lock_irqsave(&kernel_alias_lock, flags);
	for (address = start; address < end; address += PAGE_SIZE) {
		unsigned int level;
		pte_t *pte = lookup_address(address, &level);

		/* Boot creates this writable direct-map view with 4K PTEs. */
		if (!pte || level != PG_LEVEL_4K || !pte_present(ptep_get(pte)))
			panic("missing hosted core direct-map PTE");
		set_pte(pte, pfn_pte(pte_pfn(ptep_get(pte)),
				    writable ? PAGE_KERNEL : PAGE_KERNEL_RO));
	}
	protect_direct_range(start, end);
	raw_spin_unlock_irqrestore(&kernel_alias_lock, flags);
}

void kobox_linux_memory_sync_direct(struct page *page, unsigned int nr)
{
	unsigned long start = (unsigned long)page_address(page);
	unsigned long pfn = page_to_pfn(page);

	if (!nr || pfn >= max_pfn || nr > max_pfn - pfn)
		panic("invalid hosted direct-map publication");
	/* Native noflush permission restoration is visible on the next hardware
	 * page-table walk. The host must publish it before Linux reuses the page.
	 */
	flush_tlb_kernel_range(start, start + (unsigned long)nr * PAGE_SIZE);
}
#endif

void pcpu_populate_pte(unsigned long address)
{
	(void)address;
}

#ifndef KOBOX_BOOT_RUNTIME
void pgtable_cache_init(void)
{
}
#endif

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

#ifndef KOBOX_BOOT_RUNTIME
void alternatives_enable_smp(void)
{
}
#endif

#ifndef KOBOX_TASK_PORT_PHASE
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
#endif

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
	/* start_kernel() invokes this hook before its boot-CPU setup hook. */
	kobox_percpu_offset = __per_cpu_offset[0];
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
	status = kobox_host_call(memory_layout->operations->map(
		memory_layout->vmemmap_window, start - vmemmap_base,
		memory_layout->ram_backing, physical, size,
		KOBOX_LINUX_MEMORY_READ | KOBOX_LINUX_MEMORY_WRITE, &mapped));
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
	    !layout->operations->protect || !layout->direct_window ||
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
	    !layout->vmalloc_size || !layout->vmemmap_size ||
	    layout->ram_size > ULONG_MAX - (unsigned long)layout->direct_map ||
	    layout->vmemmap_size > ULONG_MAX - (unsigned long)layout->vmemmap_base ||
	    layout->vmalloc_size > ULONG_MAX - (unsigned long)layout->vmalloc_base ||
	    !PAGE_ALIGNED(layout->kernel_image_physical_base) ||
	    layout->kernel_image_physical_base > layout->ram_size ||
	    image_size > layout->ram_size - layout->kernel_image_physical_base)
		return -EINVAL;
	return 0;
}

int kobox_linux_memory_bind(const struct kobox_linux_memory_layout *layout)
{
	int status;

	status = validate_layout(layout);
	if (status)
		return status;
	if (memory_layout)
		return -EBUSY;
	memory_layout = layout;
	page_offset_base = (unsigned long)layout->direct_map;
	vmemmap_base = (unsigned long)layout->vmemmap_base;
	vmalloc_base = (unsigned long)layout->vmalloc_base;
	kobox_memory_vmalloc_end = vmalloc_base + layout->vmalloc_size - 1;
	phys_base = __START_KERNEL_map - (unsigned long)_text +
		layout->kernel_image_physical_base;
	return 0;
}

static void __init prepare_direct_tables(void)
{
	unsigned long address, pfn;

	/* Machine page tables for the RAM mapping already owned by the host.
	 * memblock reserves these tables before struct page and buddy startup.
	 * Native CPA owns subsequent permission changes and alias decisions.
	 */
	for (pfn = 0; pfn < max_pfn; pfn++) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		address = page_offset_base + (pfn << PAGE_SHIFT);
		pgd = pgd_offset_k(address);
		p4d = p4d_offset(pgd, address);
		if (p4d_none(*p4d))
			p4d_populate(&init_mm, p4d,
				     memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE));
		pud = pud_offset(p4d, address);
		if (pud_none(*pud))
			pud_populate(&init_mm, pud,
				     memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE));
		pmd = pmd_offset(pud, address);
		if (pmd_none(*pmd))
			pmd_populate_kernel(&init_mm, pmd,
				memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE));
		pte = pte_offset_kernel(pmd, address);
		if (!pte_none(*pte))
			panic("hosted RAM overlaps an existing kernel PTE");
		set_pte(pte, pfn_pte(pfn, PAGE_KERNEL));
	}
	direct_tables_ready = true;
}

static int prepare_memory(const struct kobox_linux_memory_layout *layout)
{
	unsigned long image_size = PAGE_ALIGN(__bss_stop - _text);
	int status;

	if (!memory_layout) {
		status = kobox_linux_memory_bind(layout);
		if (status)
			return status;
	}
	if (memory_layout != layout)
		return -EINVAL;

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

	prepare_direct_tables();
	sparse_init();
	return 0;
}

static void prepare_cpu_masks(void)
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
}

#ifndef KOBOX_BOOT_RUNTIME
static int prepare_percpu(void)
{
	prepare_cpu_masks();
	if (nr_cpu_ids != KOBOX_LINUX_MEMORY_LOGICAL_CPUS)
		return -EINVAL;
	setup_per_cpu_areas();
	kobox_percpu_offset = __per_cpu_offset[0];
	return 0;
}
#endif

int __init kobox_linux_memory_setup_arch(void)
{
	int status;

	if (!memory_layout)
		return -EINVAL;
	prepare_cpu_masks();
	status = prepare_memory(memory_layout);
	if (status)
		return status;
	setup_memory_zones();
	/* start_kernel() owns setup_per_cpu_areas() and mm_core_init(). */
	return 0;
}

#ifndef KOBOX_BOOT_RUNTIME
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
		is_vmalloc_addr(alias) && !is_vmalloc_addr(direct[0]) &&
		!is_vmalloc_addr(direct[1]) && !is_vmalloc_addr(&_text[0]) &&
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
#ifndef KOBOX_TASK_PORT_PHASE
	bool page_alloc_cpuhp = false;
	bool radix_cpuhp = false;
	unsigned int index;
#endif
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
#ifdef KOBOX_TASK_PORT_PHASE
	boot_cpu_hotplug_init();
#endif
	setup_memory_zones();
	mm_core_init();
	radix_tree_init();
	report->ram_size = layout->ram_size;
	report->page_offset_base = page_offset_base;
	report->vmemmap_base = vmemmap_base;
	report->vmalloc_base = vmalloc_base;
	report->phys_base = phys_base;
	report->logical_cpu_count = nr_cpu_ids;
#ifndef KOBOX_TASK_PORT_PHASE
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
	if (!report->early_cpuhp_registrations_ready)
		return -EINVAL;
#endif
	report->kernel_image_translation_ready =
		__pa_symbol(_text) == layout->kernel_image_physical_base &&
		__pa(_text) == __pa_symbol(_text) &&
		page_to_pfn(ZERO_PAGE(0)) == (__pa_symbol(empty_zero_page) >> PAGE_SHIFT);
	report->mm_core_initialized = slab_is_available();
	if (!report->mm_core_initialized ||
	    !report->kernel_image_translation_ready)
		return -EINVAL;
	return run_gate(report);
}
#endif
