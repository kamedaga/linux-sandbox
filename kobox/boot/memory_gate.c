// SPDX-License-Identifier: GPL-2.0-only

#include "memory_gate.h"
#include "host.h"

#include <linux/string.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/mount.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/sections.h>
#include <asm/tlbflush.h>

#define DYNAMIC_CHUNKS 4

static DEFINE_PER_CPU(unsigned long, memory_gate_static);

struct cpu_probe {
	unsigned long __percpu *dynamic;
	unsigned long *alias;
	unsigned int cpu;
	unsigned int errors;
};

static int fail(struct kobox_linux_boot_memory_report *report, unsigned int line)
{
	report->line = line;
	report->warnings = kobox_linux_exception_warnings();
	return -EINVAL;
}

static int page_allocator(struct kobox_linux_boot_memory_report *report)
{
	struct page *pages;
	unsigned int order, index;
	unsigned long *address;
	unsigned long pfn;

	for (order = 0; order <= 4; order++) {
		pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		if (!pages)
			return fail(report, __LINE__);
		pfn = page_to_pfn(pages);
		if (pfn & ((1UL << order) - 1))
			return fail(report, __LINE__);
		for (index = 0; index < 1U << order; index++) {
			address = page_address(pages + index);
			if (pfn_to_page(pfn + index) != pages + index ||
			    virt_to_page(address) != pages + index ||
			    __pa(address) != PFN_PHYS(pfn + index) ||
			    memchr_inv(address, 0, PAGE_SIZE))
				return fail(report, __LINE__);
			address[0] = pfn + index;
			address[PAGE_SIZE / sizeof(*address) - 1] = ~(pfn + index);
		}
		__free_pages(pages, order);
		report->page_cases++;
	}
	return 0;
}

static int heap_allocator(struct kobox_linux_boot_memory_report *report)
{
	static const size_t sizes[] = {1, 32, 173, 4096, 8193, 65537};
	struct kmem_cache *cache;
	void *objects[32];
	void *memory;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(sizes); index++) {
		memory = kzalloc(sizes[index], GFP_KERNEL);
		if (!memory || memchr_inv(memory, 0, sizes[index]))
			return fail(report, __LINE__);
		memset(memory, 0x5a, sizes[index]);
		if (memchr_inv(memory, 0x5a, sizes[index]))
			return fail(report, __LINE__);
		kfree(memory);
		report->heap_cases++;
	}
	cache = kmem_cache_create("boot-memory-gate", 173, 0, 0, NULL);
	if (!cache)
		return fail(report, __LINE__);
	for (index = 0; index < ARRAY_SIZE(objects); index++) {
		objects[index] = kmem_cache_alloc(cache, GFP_KERNEL);
		if (!objects[index])
			return fail(report, __LINE__);
		memset(objects[index], index, 173);
	}
	for (index = 0; index < ARRAY_SIZE(objects); index++) {
		if (memchr_inv(objects[index], index, 173))
			return fail(report, __LINE__);
		kmem_cache_free(cache, objects[index]);
		report->slab_cases++;
	}
	kmem_cache_destroy(cache);
	return 0;
}

static void cpu_storage_probe(void *argument)
{
	struct cpu_probe *probe = argument;
	unsigned long value = 0x73190000UL + probe->cpu;
	unsigned long *dynamic;

	preempt_disable();
	if (raw_smp_processor_id() != probe->cpu ||
	    current != raw_cpu_read(current_task))
		probe->errors++;
	this_cpu_write(memory_gate_static, value);
	dynamic = this_cpu_ptr(probe->dynamic);
	if (dynamic[0] || dynamic[PCPU_MIN_UNIT_SIZE / sizeof(*dynamic) - 1])
		probe->errors++;
	dynamic[0] = value;
	dynamic[PCPU_MIN_UNIT_SIZE / sizeof(*dynamic) - 1] = ~value;
	if (probe->alias) {
		if (probe->alias[0] != 0xa7110000UL + probe->cpu)
			probe->errors++;
		probe->alias[1] = value;
	}
	preempt_enable();
}

static int read_alias(const void *address, unsigned long *value)
{
	/* Native address filtering rejects hosted kernel addresses; this known
	 * fixture alias is probed with upstream's exception-table load itself.
	 */
	pagefault_disable();
	__get_kernel_nofault(value, address, unsigned long, fault);
	pagefault_enable();
	return 0;
fault:
	pagefault_enable();
	return -EFAULT;
}

static int parent_permissions(unsigned long address,
			      struct kobox_linux_boot_memory_report *report)
{
	pgd_t original = *pgd_offset_k(address), root = original;
	unsigned int level, expected_level;
	pte_t *expected = lookup_address(address, &expected_level);
	bool nx, rw;

	/* Test the native helper on a copied root entry. Never change the live
	 * ancestor shared by kernel mappings merely to exercise its attributes.
	 */
	if (!expected || lookup_address_in_pgd_attr(&root, address, &level, &nx, &rw) !=
	    expected || level != expected_level || !rw)
		return fail(report, __LINE__);
	report->parent_permission_cases++;
	root = __pgd(pgd_val(original) & ~_PAGE_RW);
	if (lookup_address_in_pgd_attr(&root, address, &level, &nx, &rw) != expected || rw)
		return fail(report, __LINE__);
	report->parent_permission_cases++;
	root = __pgd(pgd_val(original) | _PAGE_NX);
	if (lookup_address_in_pgd_attr(&root, address, &level, &nx, &rw) != expected || !nx)
		return fail(report, __LINE__);
	report->parent_permission_cases++;
	return 0;
}

static int percpu_and_alias(struct kobox_linux_boot_memory_report *report)
{
	unsigned long __percpu *dynamic[DYNAMIC_CHUNKS];
	struct page *pages[2];
	unsigned long *alias, *direct;
	unsigned long value;
	unsigned int index, cpu;
	struct cpu_probe probe;

	for (index = 0; index < ARRAY_SIZE(dynamic); index++) {
		/* Force real dynamic chunks, not only the embedded first chunk. */
		dynamic[index] = __alloc_percpu(PCPU_MIN_UNIT_SIZE, PAGE_SIZE);
		if (!dynamic[index])
			return fail(report, __LINE__);
		if (is_vmalloc_addr(per_cpu_ptr(dynamic[index], 0)))
			report->dynamic_vmap_allocations++;
	}
	if (!report->dynamic_vmap_allocations ||
	    per_cpu_ptr(&memory_gate_static, 0) == per_cpu_ptr(&memory_gate_static, 1))
		return fail(report, __LINE__);
	for (cpu = 0; cpu < ARRAY_SIZE(pages); cpu++) {
		pages[cpu] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[cpu])
			return fail(report, __LINE__);
		direct = page_address(pages[cpu]);
		direct[0] = 0xa7110000UL + cpu;
	}
	alias = vmap(pages, ARRAY_SIZE(pages), VM_MAP, PAGE_KERNEL);
	if (!alias || !is_vmalloc_addr(alias) ||
	    is_vmalloc_addr(page_address(pages[0])) || is_vmalloc_addr(_text))
		return fail(report, __LINE__);
	if (parent_permissions((unsigned long)alias, report))
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(dynamic); index++) {
		for_each_online_cpu(cpu) {
			probe = (struct cpu_probe) {
				.dynamic = dynamic[index],
				.alias = alias + cpu * PAGE_SIZE / sizeof(*alias),
				.cpu = cpu,
			};
			if (smp_call_function_single(cpu, cpu_storage_probe, &probe, 1) ||
			    probe.errors)
				return fail(report, __LINE__);
			value = 0x73190000UL + cpu;
			direct = per_cpu_ptr(dynamic[index], cpu);
			if (per_cpu(memory_gate_static, cpu) != value ||
			    direct[0] != value ||
			    direct[PCPU_MIN_UNIT_SIZE / sizeof(*direct) - 1] != ~value ||
			    ((unsigned long *)page_address(pages[cpu]))[1] != value ||
			    vmalloc_to_page(probe.alias) != pages[cpu])
				return fail(report, __LINE__);
			report->percpu_cases++;
			report->alias_cases++;
		}
	}
	/* Unmap before returning PFNs to buddy, including host alias revocation. */
	vunmap(alias);
	vm_unmap_aliases();
	if (read_alias(alias, &value) != -EFAULT)
		return fail(report, __LINE__);
	for (cpu = 0; cpu < ARRAY_SIZE(pages); cpu++)
		__free_page(pages[cpu]);
	for (index = 0; index < ARRAY_SIZE(dynamic); index++)
		free_percpu(dynamic[index]);
	return 0;
}

static int sparse_mapping(struct kobox_linux_boot_memory_report *report)
{
	struct vm_struct *area;
	struct page *pages[3];
	unsigned long start, end, value;
	unsigned long *alias;
	unsigned int index;

	area = get_vm_area(5 * PAGE_SIZE, VM_SPARSE);
	if (!area)
		return fail(report, __LINE__);
	start = (unsigned long)area->addr;
	end = start + 5 * PAGE_SIZE;
	for (index = 0; index < ARRAY_SIZE(pages); index++) {
		pages[index] = alloc_page(GFP_KERNEL);
		if (!pages[index])
			return fail(report, __LINE__);
		*(unsigned long *)page_address(pages[index]) = 0x517a0000 + index;
		alias = (void *)(start + index * 2 * PAGE_SIZE);
		if (vm_area_map_pages(area, (unsigned long)alias,
				     (unsigned long)alias + PAGE_SIZE, &pages[index]))
			return fail(report, __LINE__);
	}
	/* Exercise the architecture contract with a sparse, CPU-like span. */
	flush_cache_vmap(start, end);
	if (read_alias((void *)(start + PAGE_SIZE), &value) != -EFAULT ||
	    read_alias((void *)(start + 3 * PAGE_SIZE), &value) != -EFAULT)
		return fail(report, __LINE__);
	flush_cache_vunmap(start, end);
	vm_area_unmap_pages(area, start + 2 * PAGE_SIZE, start + 3 * PAGE_SIZE);
	flush_tlb_kernel_range(start, end);
	/* VMALLOC_END is inclusive, whereas the flush range is half-open. */
	__flush_tlb_all();
	for (index = 0; index < ARRAY_SIZE(pages); index++) {
		alias = (void *)(start + index * 2 * PAGE_SIZE);
		if (index == 1) {
			if (read_alias(alias, &value) != -EFAULT)
				return fail(report, __LINE__);
		} else if (read_alias(alias, &value) || value != 0x517a0000 + index) {
			return fail(report, __LINE__);
		}
		report->alias_cases++;
	}
	free_vm_area(area);
	vm_unmap_aliases();
	if (read_alias((void *)start, &value) != -EFAULT)
		return fail(report, __LINE__);
	for (index = 0; index < ARRAY_SIZE(pages); index++)
		__free_page(pages[index]);
	return 0;
}

static int shmem_boot(struct kobox_linux_boot_memory_report *report)
{
	struct file *file;
	struct folio *folio, *same;
	struct address_space *mapping;
	unsigned long *address;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return fail(report, __LINE__);
		file = shmem_file_setup("boot-shmem-gate", PAGE_SIZE, VM_NORESERVE);
		if (IS_ERR(file))
			return fail(report, __LINE__);
		mapping = file->f_mapping;
		if (!shmem_mapping(mapping) || mapping->nrpages ||
		    i_size_read(file_inode(file)) != PAGE_SIZE)
			return fail(report, __LINE__);
		folio = shmem_read_folio_gfp(mapping, 0, GFP_KERNEL);
		if (IS_ERR(folio))
			return fail(report, __LINE__);
		address = folio_address(folio);
		if (!folio_test_swapbacked(folio) || !folio_test_uptodate(folio) ||
		    folio_mapping(folio) != mapping ||
		    memchr_inv(address, 0, PAGE_SIZE) || mapping->nrpages != 1)
			return fail(report, __LINE__);
		folio_lock(folio);
		address[0] = 0x5a7e0000UL + cpu;
		folio_mark_dirty(folio);
		folio_unlock(folio);
		same = shmem_read_folio_gfp(mapping, 0, GFP_KERNEL);
		if (IS_ERR(same) || same != folio ||
		    *(unsigned long *)folio_address(same) != 0x5a7e0000UL + cpu)
			return fail(report, __LINE__);
		folio_put(same);
		/* This internal file has no mount/user callbacks depending on us.
		 * Synchronous fput lets us check actual eviction while retaining the
		 * caller's folio reference, not read a freed inode or page.
		 */
		__fput_sync(file);
		if (folio_mapping(folio) || folio_ref_count(folio) != 1)
			return fail(report, __LINE__);
		folio_put(folio);
		report->shmem_cases++;
	}
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_boot_memory_verify(struct kobox_linux_boot_memory_report *report)
{
	cpumask_t saved;
	struct pglist_data *pgdat;
	unsigned int cpu;
	int status;

	if (!report || report->size != sizeof(*report))
		return -EINVAL;
	report->phase = 1;
	if (!IS_ENABLED(CONFIG_SHMEM) || !IS_ENABLED(CONFIG_TMPFS) ||
	    system_state != SYSTEM_RUNNING || task_pid_nr(current) != 1 ||
	    num_online_cpus() != 2 || !slab_is_available() ||
	    !rcu_inkernel_boot_has_ended() || !current->fs ||
	    !current->fs->root.mnt || !current->fs->root.dentry ||
	    !totalram_pages() || kobox_linux_exception_warnings())
		return fail(report, __LINE__);
	for_each_online_pgdat(pgdat) {
		if (!pgdat->kswapd || !(pgdat->kswapd->flags & PF_KTHREAD))
			return fail(report, __LINE__);
		report->kswapd_ready++;
	}
	cpumask_copy(&saved, current->cpus_ptr);
	report->phase = 2;
	for_each_online_cpu(cpu) {
		report->cpu = cpu;
		if (set_cpus_allowed_ptr(current, cpumask_of(cpu)))
			return fail(report, __LINE__);
		status = page_allocator(report) ?: heap_allocator(report);
		if (status)
			return status;
	}
	report->phase = 3;
	status = percpu_and_alias(report) ?: sparse_mapping(report);
	if (status)
		return status;
	report->phase = 4;
	status = shmem_boot(report);
	if (status)
		return status;
	report->warnings = kobox_linux_exception_warnings();
	if (report->warnings)
		return fail(report, __LINE__);
	report->phase = 5;
	return set_cpus_allowed_ptr(current, &saved);
}
