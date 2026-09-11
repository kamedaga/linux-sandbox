// SPDX-License-Identifier: GPL-2.0-only

#include "mmio.h"
#include "../arch/x86_64/host_call.h"

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <asm/memtype.h>
#include <asm/io.h>
#include <asm/mtrr.h>
#include <asm/pgtable.h>
#include "../../arch/x86/mm/physaddr.h"

struct kobox_mmio_region {
	struct list_head list;
	struct kobox_mmio_host host;
	unsigned long aliases;
};

struct mmio_alias {
	struct kobox_mmio_region *region;
	pte_t pte;
};

static LIST_HEAD(regions);
static DEFINE_XARRAY(aliases);
static DEFINE_RAW_SPINLOCK(mmio_lock);

static bool mmio_transaction(unsigned long address, unsigned int width,
			     u64 *value, bool write)
{
	struct mmio_alias *alias, *last;
	struct kobox_mmio_host *host;
	unsigned long flags, end;
	u64 physical;
	int result = 0;
	bool handled = false;

	if ((width != 1 && width != 2 && width != 4 && width != 8) ||
	    check_add_overflow(address, width - 1, &end))
		panic("invalid MMIO transaction width or range");
	raw_spin_lock_irqsave(&mmio_lock, flags);
	alias = xa_load(&aliases, address >> PAGE_SHIFT);
	if (!alias || !alias->region->host.read)
		goto out;
	handled = true;
	host = &alias->region->host;
	last = xa_load(&aliases, end >> PAGE_SHIFT);
	if (!last || last->region != alias->region ||
	    pte_pfn(last->pte) != pte_pfn(alias->pte) +
		((end >> PAGE_SHIFT) - (address >> PAGE_SHIFT)) ||
	    (write && (!pte_write(alias->pte) || !pte_write(last->pte)))) {
		result = -EFAULT;
		goto out;
	}
	physical = ((u64)pte_pfn(alias->pte) << PAGE_SHIFT) + offset_in_page(address);
	if (write)
		result = kobox_host_call(host->write(host->context, physical, width, *value));
	else
		result = kobox_host_call(host->read(host->context, physical, width, value));
out:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	if (result)
		panic("MMIO transaction failed: address=%lx width=%u write=%u result=%d",
		      address, width, write, result);
	return handled;
}

u64 kobox_mmio_read(const volatile void __iomem *address, unsigned int width)
{
	const volatile void *native = (const volatile void __force *)address;
	u64 value = 0;

	if (mmio_transaction((unsigned long)address, width, &value, false))
		return value;
	barrier();
	switch (width) {
	case 1: value = *(const volatile u8 *)native; break;
	case 2: value = *(const volatile u16 *)native; break;
	case 4: value = *(const volatile u32 *)native; break;
	case 8: value = *(const volatile u64 *)native; break;
	}
	barrier();
	return value;
}

void kobox_mmio_write(volatile void __iomem *address, unsigned int width, u64 value)
{
	volatile void *native = (volatile void __force *)address;

	if (mmio_transaction((unsigned long)address, width, &value, true))
		return;
	barrier();
	switch (width) {
	case 1: *(volatile u8 *)native = value; break;
	case 2: *(volatile u16 *)native = value; break;
	case 4: *(volatile u32 *)native = value; break;
	case 8: *(volatile u64 *)native = value; break;
	}
	barrier();
}

u8 mtrr_type_lookup(u64 start, u64 end, u8 *uniform)
{
	struct kobox_mmio_region *region;
	unsigned long flags;
	u8 type = MTRR_TYPE_UNCACHABLE;

	*uniform = 0;
	if (start >= end)
		return type;
	/* Guest PFNs are offsets in host backing objects, not host machine
	 * physical addresses. There is no guest-address MTRR override: RAM's
	 * memfd backing is uniformly WB; authorized MMIO gets its effective
	 * type at map time from the host. WB is the neutral MTRR constraint,
	 * not permission to map a device WB. Unknown spans remain unverified.
	 */
	if (end <= (u64)max_pfn << PAGE_SHIFT) {
		*uniform = 1;
		return MTRR_TYPE_WRBACK;
	}
	raw_spin_lock_irqsave(&mmio_lock, flags);
	list_for_each_entry(region, &regions, list) {
		if (start >= region->host.start &&
		    end <= region->host.start + region->host.length) {
			*uniform = 1;
			type = MTRR_TYPE_WRBACK;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	return type;
}

int kobox_mmio_register(const struct kobox_mmio_host *host,
			struct kobox_mmio_region **out)
{
	struct kobox_mmio_region *region, *other;
	unsigned long flags;
	u64 end;
	int result = 0;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!host || !host->context || !host->map || !host->unmap || !host->length ||
	    (!!host->read != !!host->write) ||
	    !PAGE_ALIGNED(host->start) || !PAGE_ALIGNED(host->length) ||
	    host->start < (u64)max_pfn << PAGE_SHIFT ||
	    check_add_overflow(host->start, host->length, &end) ||
	    !phys_addr_valid(end - 1))
		return -EINVAL;
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;
	region->host = *host;
	raw_spin_lock_irqsave(&mmio_lock, flags);
	list_for_each_entry(other, &regions, list) {
		if (host->start < other->host.start + other->host.length &&
		    other->host.start < end) {
			result = -EBUSY;
			goto out_unlock;
		}
	}
	list_add_tail(&region->list, &regions);
	*out = region;
out_unlock:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	if (result)
		kfree(region);
	return result;
}

int kobox_mmio_unregister(struct kobox_mmio_region **remove, size_t count)
{
	unsigned long flags;
	size_t index;
	int result = 0;

	if (!remove && count)
		return -EINVAL;
	raw_spin_lock_irqsave(&mmio_lock, flags);
	for (index = 0; index < count; index++) {
		if (!remove[index] || remove[index]->aliases) {
			result = remove[index] ? -EBUSY : -EINVAL;
			goto out;
		}
	}
	for (index = 0; index < count; index++)
		list_del(&remove[index]->list);
out:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	if (!result) {
		for (index = 0; index < count; index++) {
			kfree(remove[index]);
			remove[index] = NULL;
		}
	}
	return result;
}

static int cache_type(pte_t pte, enum kobox_mmio_cache *cache)
{
	switch (pgprot2cachemode(pte_pgprot(pte))) {
	case _PAGE_CACHE_MODE_UC: *cache = KOBOX_MMIO_UC; break;
	case _PAGE_CACHE_MODE_UC_MINUS: *cache = KOBOX_MMIO_UC_MINUS; break;
	case _PAGE_CACHE_MODE_WC: *cache = KOBOX_MMIO_WC; break;
	case _PAGE_CACHE_MODE_WB: *cache = KOBOX_MMIO_WB; break;
	case _PAGE_CACHE_MODE_WT: *cache = KOBOX_MMIO_WT; break;
	case _PAGE_CACHE_MODE_WP: *cache = KOBOX_MMIO_WP; break;
	default: return -EOPNOTSUPP;
	}
	return 0;
}

int kobox_mmio_publish(unsigned long address, pte_t pte, bool create)
{
	struct kobox_mmio_region *region;
	struct mmio_alias *alias;
	u64 physical = (u64)pte_pfn(pte) << PAGE_SHIFT;
	unsigned long flags, index = address >> PAGE_SHIFT;
	unsigned int protection = KOBOX_LINUX_MEMORY_READ;
	enum kobox_mmio_cache cache;
	int result;

	if (pte_exec(pte))
		return -EACCES;
	result = cache_type(pte, &cache);
	if (result)
		return result;
	if (pte_write(pte))
		protection |= KOBOX_LINUX_MEMORY_WRITE;
	raw_spin_lock_irqsave(&mmio_lock, flags);
	alias = xa_load(&aliases, index);
	if (alias) {
		/* Accessed/dirty bits do not change the host translation. */
		if (!((pte_val(alias->pte) ^ pte_val(pte)) &
		      ~(_PAGE_ACCESSED | _PAGE_DIRTY))) {
			result = 0;
			goto out_unlock;
		}
		result = -EOPNOTSUPP;
		goto out_unlock;
	}
	/* A TLB flush can observe not-yet-published or failed ioremap PTEs.
	 * Only the fallible publication boundary may install a new alias.
	 */
	if (!create) {
		result = 0;
		goto out_unlock;
	}
	result = -ERANGE;
	list_for_each_entry(region, &regions, list) {
		if (physical < region->host.start ||
		    physical - region->host.start > region->host.length - PAGE_SIZE)
			continue;
		alias = kmalloc(sizeof(*alias), GFP_ATOMIC);
		if (!alias) {
			result = -ENOMEM;
			break;
		}
		alias->region = region;
		alias->pte = pte;
		result = xa_err(xa_store(&aliases, index, alias, GFP_ATOMIC));
		if (!result) {
			result = kobox_host_call(region->host.map(region->host.context, (void *)address,
				physical, PAGE_SIZE, protection, cache));
			if (result)
				xa_erase(&aliases, index);
		}
		if (result)
			kfree(alias);
		else
			region->aliases++;
		break;
	}
out_unlock:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	return result;
}

int kobox_mmio_reset(unsigned long start, unsigned long end, void *context,
		     int (*reset_ram)(void *, unsigned long, unsigned long))
{
	struct mmio_alias *alias;
	unsigned long index, flags;
	int result = 0;

	if (start >= end)
		return 0;
	raw_spin_lock_irqsave(&mmio_lock, flags);
	xa_for_each_range(&aliases, index, alias, start >> PAGE_SHIFT,
			  (end - 1) >> PAGE_SHIFT) {
		unsigned long address = index << PAGE_SHIFT;

		if (start < address) {
			result = reset_ram(context, start, address);
			if (result)
				goto out;
		}
		result = kobox_host_call(alias->region->host.unmap(alias->region->host.context,
						 (void *)address, PAGE_SIZE));
		if (result)
			goto out;
		xa_erase(&aliases, index);
		alias->region->aliases--;
		kfree(alias);
		start = address + PAGE_SIZE;
	}
	if (start < end)
		result = reset_ram(context, start, end);
out:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	return result;
}
