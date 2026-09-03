// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/types.h>

struct vmem_altmap;

int vmemmap_populate(unsigned long start, unsigned long end, int node,
		     struct vmem_altmap *altmap)
{
	(void)start;
	(void)end;
	(void)node;
	(void)altmap;
	return -EOPNOTSUPP;
}
