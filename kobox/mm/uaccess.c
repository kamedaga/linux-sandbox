// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/futex.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm/pgtable.h>
#include <asm/futex.h>

enum transfer {
	READ_USER, READ_USER_NONTEMPORAL, READ_USER_FLUSHCACHE,
	WRITE_USER, ZERO_USER, CMPXCHG_USER, FUTEX_USER,
};

struct atomic_request {
	u64 expected, value, observed;
	int operation;
};

static void atomic_access(void *alias, size_t size, enum transfer operation,
			  struct atomic_request *request)
{
	u32 old, value, seen;

	if (operation == CMPXCHG_USER) {
		switch (size) {
		case 1:
			request->observed = cmpxchg((u8 *)alias, request->expected, request->value);
			break;
		case 2:
			request->observed = cmpxchg((u16 *)alias, request->expected, request->value);
			break;
		case 4:
			request->observed = cmpxchg((u32 *)alias, request->expected, request->value);
			break;
		case 8:
			request->observed = cmpxchg((u64 *)alias, request->expected, request->value);
			break;
		}
		return;
	}
	old = READ_ONCE(*(u32 *)alias);
	for (;;) {
		switch (request->operation) {
		case FUTEX_OP_SET:
			value = request->value;
			break;
		case FUTEX_OP_ADD:
			value = old + (u32)request->value;
			break;
		case FUTEX_OP_OR:
			value = old | (u32)request->value;
			break;
		case FUTEX_OP_ANDN:
			value = old & ~(u32)request->value;
			break;
		case FUTEX_OP_XOR:
			value = old ^ (u32)request->value;
			break;
		default:
			__builtin_trap();
		}
		seen = cmpxchg((u32 *)alias, old, value);
		if (seen == old)
			break;
		old = seen;
	}
	request->observed = old;
}

/* PTE locks protect both the translation and its page reference until the
 * operation finishes. RCU plus disabled IRQs protect the page-table walk,
 * including mmu_gather's single-table allocation-failure fallback.
 * This is a software MMU access, not a replacement VMA/fault/reclaim policy.
 */
static int transfer_page(unsigned long address, void *buffer, size_t size,
			 enum transfer operation)
{
	struct mm_struct *mm = current->mm;
	unsigned long irqflags;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	void *alias;
	int result = -EAGAIN;
	bool write = operation >= WRITE_USER;

	/* An NMI cannot wait on a PTE lock held by the interrupted task. */
	if (in_nmi())
		return -EFAULT;
	rcu_read_lock();
	local_irq_save(irqflags);
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
	if ((pte_flags(pte) & (_PAGE_PRESENT | _PAGE_USER)) !=
	    (_PAGE_PRESENT | _PAGE_USER) ||
	    (write && !pte_write(pte)))
		goto unlock;
	if (!pfn_valid(pte_pfn(pte))) {
		result = -EOPNOTSUPP;
		goto unlock;
	}
	/* Software permission checks must also gate speculative RAM access. */
	barrier_nospec();
	alias = page_address(pte_page(pte)) + offset_in_page(address);
	/* Emulate hardware A/D updates while the PTE is locked. In particular,
	 * do not dirty a detached folio/mapping after a concurrent truncate.
	 * Linux's page_mkclean and reclaim observe the PTE in the usual way.
	 */
	pte = pte_mkyoung(pte);
	if (write)
		pte = pte_mkdirty(pte);
	set_pte_at(mm, address, ptep, pte);
	if (operation == READ_USER)
		memcpy(buffer, alias, size);
	else if (operation == READ_USER_NONTEMPORAL) {
		if (copy_to_nontemporal(buffer, alias, size))
			__builtin_trap();
	} else if (operation == READ_USER_FLUSHCACHE) {
		if (copy_user_flushcache(buffer, (void __user *)alias, size))
			__builtin_trap();
	} else if (operation == WRITE_USER)
		memcpy(alias, buffer, size);
	else if (operation == ZERO_USER)
		memset(alias, 0, size);
	else
		atomic_access(alias, size, operation, buffer);
	result = 0;
unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	local_irq_restore(irqflags);
	rcu_read_unlock();
	return result;
}

static unsigned long transfer(unsigned long address, void *buffer,
			      unsigned long size, enum transfer operation)
{
	unsigned long remaining = size;
	bool faultable = operation != READ_USER_NONTEMPORAL &&
		!faulthandler_disabled() && !irqs_disabled();
	unsigned int flags = operation < WRITE_USER ? 0 : FAULT_FLAG_WRITE;
	bool unlocked;
	int result;

	if (!current->mm || address >= TASK_SIZE || size > TASK_SIZE - address)
		return size;
	while (remaining) {
		size_t chunk = min(remaining, PAGE_SIZE - offset_in_page(address));

		result = transfer_page(address, buffer, chunk, operation);
		if (result) {
			if (result != -EAGAIN || !faultable)
				break;
			if (mmap_read_lock_killable(current->mm))
				break;
			unlocked = false;
			result = fixup_user_fault(current->mm, address, flags, &unlocked);
			mmap_read_unlock(current->mm);
			if (result)
				break;
			cond_resched();
			continue;
		}
		address += chunk;
		if (buffer)
			buffer += chunk;
		remaining -= chunk;
	}
	return remaining;
}

static bool native_input(void)
{
	bool native;

	/* Boot/kernel test callers may use native process-local input buffers.
	 * Never infer this mode from the supplied address or a failed guest
	 * lookup. A client task must have its own mm and can never fall back.
	 */
	native = !current->mm &&
		((current->flags & PF_KTHREAD) || task_pid_nr(current) == 1);
	if (native)
		barrier_nospec();
	return native;
}

unsigned long kobox_raw_copy_from_user(void *to, const void __user *from,
				       unsigned long size)
{
	if (!access_ok(from, size))
		return size;
	if (native_input())
		return copy_user_generic(to, (__force const void *)from, size);
	return transfer((unsigned long)from, to, size, READ_USER);
}
EXPORT_SYMBOL(kobox_raw_copy_from_user);

unsigned long kobox_read_user_nontemporal(void *to, const void __user *from,
					unsigned long size)
{
	if (!access_ok(from, size))
		return size;
	if (native_input())
		return copy_to_nontemporal(to, (__force const void *)from, size);
	return transfer((unsigned long)from, to, size, READ_USER_NONTEMPORAL);
}
EXPORT_SYMBOL(kobox_read_user_nontemporal);

unsigned long kobox_read_user_flushcache(void *to, const void __user *from,
				       unsigned long size)
{
	if (!access_ok(from, size))
		return size;
	if (native_input())
		return copy_user_flushcache(to, from, size);
	return transfer((unsigned long)from, to, size, READ_USER_FLUSHCACHE);
}
EXPORT_SYMBOL(kobox_read_user_flushcache);

unsigned long kobox_raw_copy_to_user(void __user *to, const void *from,
				     unsigned long size)
{
	if (!access_ok(to, size))
		return size;
	if (native_input())
		return copy_user_generic((__force void *)to, from, size);
	return transfer((unsigned long)to, (void *)from, size, WRITE_USER);
}
EXPORT_SYMBOL(kobox_raw_copy_to_user);

unsigned long kobox_clear_user(void __user *to, unsigned long size)
{
	might_fault();
	if (!access_ok(to, size))
		return size;
	if (native_input()) {
		/* Call the native inline function hidden by the hosted macro. */
		return kobox_native_clear_user(to, size);
	}
	return transfer((unsigned long)to, NULL, size, ZERO_USER);
}
EXPORT_SYMBOL(kobox_clear_user);

/* Native exception-table operations are used only for trusted local inputs. */
#define NATIVE_CMPXCHG(type, suffix, constraint) do { \
	type old = expected; \
	(void)__try_cmpxchg_user_asm(suffix, constraint, (type __user *)address, \
				     &old, (type)value, fault); \
	*observed = old; \
	return 0; \
} while (0)

int kobox_user_cmpxchg(void __user *address, unsigned int size,
		       u64 expected, u64 value, u64 *observed)
{
	struct atomic_request request = {.expected = expected, .value = value};
	int result;

	if ((size != 1 && size != 2 && size != 4 && size != 8) ||
	    !access_ok(address, size) || size > PAGE_SIZE - offset_in_page(address))
		return -EFAULT;
	if (native_input()) {
		switch (size) {
		case 1:
			NATIVE_CMPXCHG(u8, "b", "q");
		case 2:
			NATIVE_CMPXCHG(u16, "w", "r");
		case 4:
			NATIVE_CMPXCHG(u32, "l", "r");
		case 8:
			NATIVE_CMPXCHG(u64, "q", "r");
		}
	}
	if (!current->mm)
		return -EFAULT;
	result = transfer_page((unsigned long)address, &request, size, CMPXCHG_USER);
	if (result)
		return -EFAULT;
	*observed = request.observed;
	return 0;
fault:
	return -EFAULT;
}
EXPORT_SYMBOL(kobox_user_cmpxchg);

int kobox_futex_op(int operation, int operand, int *old, u32 __user *address)
{
	struct atomic_request request = {.operation = operation, .value = (u32)operand};
	int result;

	if (operation < FUTEX_OP_SET || operation > FUTEX_OP_XOR)
		return -ENOSYS;
	if (!access_ok(address, sizeof(*address)) || !IS_ALIGNED((unsigned long)address, 4))
		return -EFAULT;
	if (native_input())
		return kobox_native_futex_op(operation, operand, old, address);
	if (!current->mm)
		return -EFAULT;
	result = transfer_page((unsigned long)address, &request, sizeof(*address), FUTEX_USER);
	if (result)
		return -EFAULT;
	*old = request.observed;
	return 0;
}
EXPORT_SYMBOL(kobox_futex_op);
