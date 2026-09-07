// SPDX-License-Identifier: GPL-2.0-only

#include "port.h"

#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/ptrace.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>

/* Fatal machine failure cannot be confused with completed TLB invalidation. */
#undef BUG
#define BUG() __builtin_trap()

static LIST_HEAD(vm_spaces);
static DEFINE_RAW_SPINLOCK(vm_spaces_lock);

void kobox_vm_interrupt(void)
{
	struct kobox_vm_space *space;
	unsigned long flags;

	if (!in_hardirq())
		BUG();
	raw_spin_lock_irqsave(&vm_spaces_lock, flags);
	list_for_each_entry(space, &vm_spaces, entry)
		wake_up_all(&space->events);
	raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
}

void switch_mm_irqs_off(struct mm_struct *previous, struct mm_struct *next,
		       struct task_struct *task)
{
	struct mm_struct *loaded = this_cpu_read(cpu_tlbstate.loaded_mm);
	unsigned int cpu = raw_smp_processor_id();

	if (!irqs_disabled() || !next)
		BUG();
	/* OS process mappings, not CR3, implement the user translation context.
	 * Retain native per-CPU mm bookkeeping for upstream MM/scheduler users.
	 */
	if (loaded && loaded != &init_mm)
		cpumask_clear_cpu(cpu, mm_cpumask(loaded));
	if (next != &init_mm)
		cpumask_set_cpu(cpu, mm_cpumask(next));
	this_cpu_write(cpu_tlbstate.loaded_mm, next);
	this_cpu_write(cpu_tlbstate_shared.is_lazy, false);
	this_cpu_write(cpu_tlbstate.loaded_mm_asid, 0);
	this_cpu_write(cpu_tlbstate.ctxs[0].ctx_id, next->context.ctx_id);
	this_cpu_write(cpu_tlbstate.ctxs[0].tlb_gen, atomic64_read(&next->context.tlb_gen));
	/* Preserve the switch_mm barrier required by upstream membarrier. */
	smp_mb();
}

void switch_mm(struct mm_struct *previous, struct mm_struct *next, struct task_struct *task)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm_irqs_off(previous, next, task);
	local_irq_restore(flags);
}

static void invalidate(struct kobox_vm_space *space, unsigned long start, unsigned long end)
{
	int result;

	start = max(start, space->start);
	end = min(end, space->end);
	if (start >= end)
		return;
	raw_spin_lock(&space->translation_lock);
	result = space->operations->reset(space->host_space, start, end - start);
	/* Only a completed unmap, including verified process death implemented
	 * by the host binding, can permit upstream to release the old pages.
	 */
	if (result)
		BUG();
	space->invalidations++;
	raw_spin_unlock(&space->translation_lock);
}

void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start, unsigned long end,
		       unsigned int stride_shift, bool freed_tables)
{
	struct kobox_vm_space *space;
	unsigned long flags;

	inc_mm_tlb_gen(mm);
	raw_spin_lock_irqsave(&vm_spaces_lock, flags);
	list_for_each_entry(space, &vm_spaces, entry)
		if (space->mm == mm)
			invalidate(space, start, end);
	raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
	/* An unbound mm has no host translations; its normal Linux destruction
	 * still runs. Secondary MMU consumers retain upstream notification.
	 */
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, start, end);
}

void kobox_vm_flush_all(void)
{
	struct kobox_vm_space *space;
	unsigned long flags;

	raw_spin_lock_irqsave(&vm_spaces_lock, flags);
	list_for_each_entry(space, &vm_spaces, entry)
		invalidate(space, space->start, space->end);
	raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
}

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	/* A broadcast host invalidation is the conservative hardware operation
	 * for a batch whose native metadata contains CPU masks, not mm pointers.
	 */
	kobox_vm_flush_all();
	cpumask_clear(&batch->cpumask);
	batch->unmapped_pages = false;
}

struct kobox_vm_space *kobox_vm_space_create(void *host_space,
	const struct kobox_linux_vm_host_operations *operations,
	unsigned long start, unsigned long size)
{
	struct kobox_vm_space *space;
	unsigned long flags;

	if (!host_space || !operations || operations->size != sizeof(*operations) ||
	    !operations->map || !operations->reset || !operations->close ||
	    !operations->resume || !operations->event ||
	    !start || !size || !PAGE_ALIGNED(start) || !PAGE_ALIGNED(size) ||
	    start >= TASK_SIZE || size > TASK_SIZE - start)
		return ERR_PTR(-EINVAL);
	space = kzalloc(sizeof(*space), GFP_KERNEL);
	if (!space)
		return ERR_PTR(-ENOMEM);
	space->mm = mm_alloc();
	if (!space->mm) {
		kfree(space);
		return ERR_PTR(-ENOMEM);
	}
	space->mm->task_size = TASK_SIZE;
	arch_pick_mmap_layout(space->mm, &current->signal->rlim[RLIMIT_STACK]);
	space->host_space = host_space;
	space->operations = operations;
	space->start = start;
	space->end = start + size;
	raw_spin_lock_init(&space->translation_lock);
	init_waitqueue_head(&space->events);
	raw_spin_lock_irqsave(&vm_spaces_lock, flags);
	list_add_tail(&space->entry, &vm_spaces);
	raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
	return space;
}

int kobox_vm_space_destroy(struct kobox_vm_space *space)
{
	unsigned long flags;
	int result;

	if (!space || current->mm == space->mm || atomic_read(&space->mm->mm_users) != 1)
		return -EBUSY;
	/* Users are joined by the caller. Kill/reap is still required before
	 * detaching the hardware context from Linux's page-release boundaries.
	 */
	raw_spin_lock_irqsave(&vm_spaces_lock, flags);
	result = space->operations->close(space->host_space);
	if (result) {
		raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
		return result;
	}
	list_del(&space->entry);
	raw_spin_unlock_irqrestore(&vm_spaces_lock, flags);
	mmput(space->mm);
	kfree(space);
	return 0;
}

static int publish_pte(struct kobox_vm_space *space, unsigned long address)
{
	struct mm_struct *mm = space->mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	unsigned long flags;
	unsigned int protection = KOBOX_VM_READ;
	int result = -EAGAIN;

	mmap_read_lock(mm);
	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		goto out;
	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		goto out;
	pud = pud_offset(p4d, address);
	if (pud_none(*pud))
		goto out;
	if (pud_leaf(*pud)) {
		result = -EOPNOTSUPP;
		goto out;
	}
	if (pud_bad(*pud))
		goto out;
	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		goto out;
	if (pmd_leaf(*pmd)) {
		result = -EOPNOTSUPP;
		goto out;
	}
	if (pmd_bad(*pmd))
		goto out;
	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		goto out;
	pte = ptep_get(ptep);
	/* Software pte_present includes PROT_NONE; pte_protnone is a no-op
	 * without NUMA balancing. Require actual hardware user accessibility.
	 */
	if ((pte_flags(pte) & (_PAGE_PRESENT | _PAGE_USER)) != (_PAGE_PRESENT | _PAGE_USER))
		goto unlock;
	if (!pfn_valid(pte_pfn(pte))) {
		result = -EOPNOTSUPP;
		goto unlock;
	}
	/* Unlike a hardware MMU, host loads/stores cannot update guest A/D bits.
	 * Keep a clean PTE read-only so the first write enters Linux's fault path.
	 */
	if (pte_write(pte) && pte_dirty(pte))
		protection |= KOBOX_VM_WRITE;
	if (!(pte_flags(pte) & _PAGE_NX))
		protection |= KOBOX_VM_EXECUTE;
	/*
	 * VM IRQ delivery takes the registry lock. A concurrent flush holds
	 * that lock while waiting for this translation lock, so IRQ delivery
	 * must remain masked for the entire publication, not just the host call.
	 */
	raw_spin_lock_irqsave(&space->translation_lock, flags);
	result = space->operations->map(space->host_space, address & PAGE_MASK,
			PFN_PHYS(pte_pfn(pte)), PAGE_SIZE, protection);
	if (!result)
		space->publications++;
	raw_spin_unlock_irqrestore(&space->translation_lock, flags);
unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	mmap_read_unlock(mm);
	return result;
}

int kobox_vm_resolve_fault(struct kobox_vm_space *space, struct kobox_linux_vm_fault *fault)
{
	struct pt_regs regs = {.cs = __USER_CS, .ss = __USER_DS, .orig_ax = ~0UL};
	kernel_siginfo_t info;
	enum pid_type type;
	sigset_t mask;
	unsigned long flags;
	int signal;

	if (!space || !fault || current->mm != space->mm ||
	    fault->address < space->start || fault->address >= space->end ||
	    !(fault->error & X86_PF_USER) || !(fault->flags & X86_EFLAGS_IF) ||
	    irqs_disabled() || in_interrupt())
		return -EINVAL;
	regs.ip = fault->ip;
	regs.sp = fault->sp;
	regs.flags = fault->flags;
	/* This is a delivered external CPU fault in a kernel service task, not
	 * an interrupt or a fabricated IRQ-entry transition from Linux idle.
	 */
	local_irq_disable();
	do_user_addr_fault(&regs, fault->error, fault->address);
	local_irq_enable();
	sigfillset(&mask);
	sigdelset(&mask, SIGSEGV);
	sigdelset(&mask, SIGBUS);
	spin_lock_irqsave(&current->sighand->siglock, flags);
	signal = dequeue_signal(&mask, &info, &type);
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	fault->signal = signal;
	fault->signal_code = signal ? info.si_code : 0;
	if (signal)
		return 0;
	/* A concurrent unmap may have won after Linux released mmap_lock.
	 * EAGAIN requests another actual instruction attempt, never a stale map.
	 */
	return publish_pte(space, fault->address);
}
