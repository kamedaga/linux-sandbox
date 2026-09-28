// SPDX-License-Identifier: GPL-2.0-only

#include "mmio.h"
#include "../arch/x86_64/host_call.h"

#include <linux/list.h>
#include <linux/kmsan-checks.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
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
	unsigned long address;
	size_t length;
	u64 physical;
	unsigned int protection;
	enum kobox_mmio_cache cache;
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
	if (last != alias || end >= alias->address + alias->length ||
	    (write && !(alias->protection & KOBOX_LINUX_MEMORY_WRITE))) {
		result = -EFAULT;
		goto out;
	}
	physical = alias->physical + address - alias->address;
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

/* String I/O instructions cannot be forwarded as one device transaction.
 * Keep callers unchanged and implement the x86 machine-port operations in
 * terms of the same ordered byte accessors used for ordinary registers. */
void memcpy_fromio(void *destination, const volatile void __iomem *source,
		   size_t length)
{
	u8 *out = destination;
	const volatile u8 __iomem *in = source;
	size_t index;

	for (index = 0; index < length; index++)
		out[index] = kobox_mmio_read(in + index, 1);
	kmsan_unpoison_memory(destination, length);
}

void memcpy_toio(volatile void __iomem *destination, const void *source,
		 size_t length)
{
	volatile u8 __iomem *out = destination;
	const u8 *in = source;
	size_t index;

	kmsan_check_memory(source, length);
	for (index = 0; index < length; index++)
		kobox_mmio_write(out + index, 1, in[index]);
}

void memset_io(volatile void __iomem *destination, int value, size_t length)
{
	volatile u8 __iomem *out = destination;
	size_t index;

	for (index = 0; index < length; index++)
		kobox_mmio_write(out + index, 1, (u8)value);
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
	if (result == -EBUSY)
		pr_warn("kobox-mmio: unregister busy start=%#llx length=%#llx aliases=%lu\n",
			(unsigned long long)remove[index]->host.start,
			(unsigned long long)remove[index]->host.length,
			remove[index]->aliases);
	if (!result) {
		for (index = 0; index < count; index++) {
			kfree(remove[index]);
			remove[index] = NULL;
		}
	}
	return result;
}

int kobox_mmio_retire_orphan_ioremaps(struct kobox_mmio_region *region)
{
	unsigned long flags, index, address, before, after;
	struct mmio_alias *alias;
	struct vm_struct *area;
	bool found;

	if (!region)
		return -EINVAL;
	for (;;) {
		found = false;
		address = 0;
		raw_spin_lock_irqsave(&mmio_lock, flags);
		before = region->aliases;
		if (before)
			xa_for_each(&aliases, index, alias)
				if (alias->region == region) {
					address = index << PAGE_SHIFT;
					found = true;
					break;
				}
		raw_spin_unlock_irqrestore(&mmio_lock, flags);
		if (!before)
			return 0;
		if (!found)
			return -EUCLEAN;
		area = find_vm_area((void *)address);
		if (!area || !(area->flags & VM_IOREMAP)) {
			pr_warn("kobox-mmio: orphan alias has no ioremap VMA va=%#lx aliases=%lu\n",
				address, before);
			return -EBUSY;
		}
		/* The module closure and DRM files have ended. iounmap owns the
		 * guest PTE/memtype teardown; removing only the host lease here
		 * would leave a stale guest mapping to a revoked PCI capability. */
		iounmap((void __iomem *)area->addr);
		vm_unmap_aliases();
		raw_spin_lock_irqsave(&mmio_lock, flags);
		after = region->aliases;
		raw_spin_unlock_irqrestore(&mmio_lock, flags);
		if (after >= before)
			return -EBUSY;
		pr_warn("kobox-mmio: retired orphan ioremap va=%#lx aliases=%lu->%lu\n",
			address, before, after);
	}
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

static bool alias_matches(const struct mmio_alias *alias, unsigned long address,
			  pte_t pte)
{
	pte_t expected;

	if (address < alias->address ||
	    address - alias->address >= alias->length)
		return false;
	expected = pfn_pte((alias->physical + address - alias->address) >>
			   PAGE_SHIFT, pte_pgprot(alias->pte));
	return !((pte_val(expected) ^ pte_val(pte)) &
		 ~(_PAGE_ACCESSED | _PAGE_DIRTY));
}

static int install_alias(struct mmio_alias *alias, bool *host_refused)
{
	unsigned long index = alias->address >> PAGE_SHIFT;
	unsigned long pages = alias->length >> PAGE_SHIFT;
	unsigned long installed = 0;
	int result;

	for (; installed < pages; installed++) {
		if (xa_load(&aliases, index + installed)) {
			result = -EBUSY;
			goto rollback;
		}
		result = xa_err(xa_store(&aliases, index + installed, alias,
					 GFP_ATOMIC));
		if (result)
			goto rollback;
	}
	result = kobox_host_call(alias->region->host.map(
		alias->region->host.context, (void *)alias->address,
		alias->physical, alias->length, alias->protection,
		alias->cache));
	if (result) {
		if (host_refused)
			*host_refused = true;
		goto rollback;
	}
	alias->region->aliases += pages;
	return 0;
rollback:
	while (installed)
		xa_erase(&aliases, index + --installed);
	return result;
}

static int publish_region(unsigned long address, size_t length, pte_t pte,
			struct kobox_mmio_region *region,
			unsigned int protection, enum kobox_mmio_cache cache,
			bool *host_refused)
{
	struct mmio_alias *alias;
	int result;

	alias = kmalloc(sizeof(*alias), GFP_ATOMIC);
	if (!alias)
		return -ENOMEM;
	*alias = (struct mmio_alias) {
		.region = region, .address = address, .length = length,
		.physical = (u64)pte_pfn(pte) << PAGE_SHIFT,
		.protection = protection, .cache = cache, .pte = pte,
	};
	result = install_alias(alias, host_refused);
	if (result)
		kfree(alias);
	return result;
}

int kobox_mmio_publish_range(unsigned long start, unsigned long end, pte_t first)
{
	struct kobox_mmio_region *region;
	u64 physical = (u64)pte_pfn(first) << PAGE_SHIFT;
	unsigned long flags, address;
	size_t failed_length = 0;
	unsigned int protection = KOBOX_LINUX_MEMORY_READ;
	enum kobox_mmio_cache cache;
	bool host_refused = false;
	int result = 0;

	if (start >= end || !PAGE_ALIGNED(start) || !PAGE_ALIGNED(end) ||
	    end - start > U64_MAX - physical)
		return -EINVAL;
	if (pte_exec(first))
		return -EACCES;
	result = cache_type(first, &cache);
	if (result)
		return result;
	if (pte_write(first))
		protection |= KOBOX_LINUX_MEMORY_WRITE;
	raw_spin_lock_irqsave(&mmio_lock, flags);
	for (address = start; address < end; ) {
		size_t length = 0;

		physical = ((u64)pte_pfn(first) << PAGE_SHIFT) +
			address - start;
		list_for_each_entry(region, &regions, list) {
			if (physical < region->host.start ||
			    physical - region->host.start >= region->host.length)
				continue;
			length = min_t(u64, end - address,
				region->host.length - (physical - region->host.start));
			break;
		}
		if (!length) {
			result = -ERANGE;
			break;
		}
		result = publish_region(address, length,
			pfn_pte(physical >> PAGE_SHIFT, pte_pgprot(first)),
			region, protection, cache, &host_refused);
		if (result) {
			failed_length = length;
			break;
		}
		address += length;
	}
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	/* The host returns a generic mapping error to ioremap. Keep the exact
	 * request in RAM logs, but never print while holding the MMIO spinlock. */
	if (host_refused)
		pr_err("kobox-mmio: host map refused va=%#lx phys=%#llx bytes=%zu cache=%u prot=%u status=%d\n",
		       address, (unsigned long long)physical, failed_length,
		       cache, protection, result);
	return result;
}

int kobox_mmio_publish(unsigned long address, pte_t pte, bool create)
{
	struct mmio_alias *alias;
	unsigned long flags;
	int result;

	if (create)
		return kobox_mmio_publish_range(address, address + PAGE_SIZE, pte);
	/* A TLB flush can see a failed ioremap's still-present PTE. Only the
	 * fallible ioremap boundary may create a new host lease.
	 */
	raw_spin_lock_irqsave(&mmio_lock, flags);
	alias = xa_load(&aliases, address >> PAGE_SHIFT);
	result = alias && !alias_matches(alias, address, pte) ? -EOPNOTSUPP : 0;
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
	while (start < end) {
		unsigned long last, first, lease_end, cut_end;
		unsigned long address;
		struct mmio_alias *left = NULL, *right = NULL;

		index = start >> PAGE_SHIFT;
		alias = xa_find(&aliases, &index, (end - 1) >> PAGE_SHIFT,
				XA_PRESENT);
		if (!alias)
			break;
		address = index << PAGE_SHIFT;

		if (start < address) {
			result = reset_ram(context, start, address);
			if (result)
				goto out;
		}
		first = alias->address;
		lease_end = first + alias->length;
		cut_end = min(end, lease_end);
		/* A TLB flush may invalidate only part of an ioremap. Revoke the
		 * whole host lease before restoring each still-live fragment.
		 */
		if (first < address) {
			left = kmalloc(sizeof(*left), GFP_ATOMIC);
			if (!left) {
				result = -ENOMEM;
				goto out;
			}
			*left = *alias;
			left->length = address - first;
		}
		if (cut_end < lease_end) {
			right = kmalloc(sizeof(*right), GFP_ATOMIC);
			if (!right) {
				kfree(left);
				result = -ENOMEM;
				goto out;
			}
			*right = *alias;
			right->address = cut_end;
			right->length = lease_end - cut_end;
			right->physical += cut_end - first;
			right->pte = pfn_pte(right->physical >> PAGE_SHIFT,
					     pte_pgprot(alias->pte));
		}
		result = kobox_host_call(alias->region->host.unmap(alias->region->host.context,
						 (void *)first, alias->length));
		if (result) {
			kfree(right);
			kfree(left);
			goto out;
		}
		for (last = first; last < lease_end; last += PAGE_SIZE)
			xa_erase(&aliases, last >> PAGE_SHIFT);
		alias->region->aliases -= alias->length >> PAGE_SHIFT;
		kfree(alias);
		if (left) {
			result = install_alias(left, NULL);
			if (result)
				goto out;
		}
		if (right) {
			result = install_alias(right, NULL);
			if (result)
				goto out;
		}
		start = cut_end;
	}
	if (start < end)
		result = reset_ram(context, start, end);
out:
	raw_spin_unlock_irqrestore(&mmio_lock, flags);
	return result;
}
