// SPDX-License-Identifier: GPL-2.0-only

#include "drm_file.h"
#include "drm_file_gate.h"

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

int kobox_linux_drm_file_gate(struct kobox_linux_drm_file *file,
			     unsigned int *checks)
{
	struct kobox_linux_drm_version *full, *shortened;
	struct kobox_linux_drm_file *invalid = NULL;
	size_t capacity[] = {128, 32, 256};
	u64 value = 0xdeadbeef;
	int result = -EINVAL;

	full = kzalloc(sizeof(*full), GFP_KERNEL);
	shortened = kzalloc(sizeof(*shortened), GFP_KERNEL);
	if (!full || !shortened) {
		result = -ENOMEM;
		goto out;
	}
	if (kobox_linux_drm_version(file, capacity, full) ||
	    !full->name_length || full->name_length >= sizeof(full->name))
		goto out;
	(*checks)++;
	capacity[0] = capacity[1] = capacity[2] = 1;
	if (kobox_linux_drm_version(file, capacity, shortened) ||
	    shortened->name_length != full->name_length ||
	    shortened->date_length != full->date_length ||
	    shortened->description_length != full->description_length ||
	    shortened->name[0] != full->name[0] ||
	    shortened->date[0] != full->date[0] ||
	    shortened->description[0] != full->description[0] ||
	    memchr_inv(shortened->name + 1, 0, sizeof(shortened->name) - 1) ||
	    memchr_inv(shortened->date + 1, 0, sizeof(shortened->date) - 1) ||
	    memchr_inv(shortened->description + 1, 0,
		       sizeof(shortened->description) - 1))
		goto out;
	(*checks)++;
	capacity[0] = capacity[1] = capacity[2] = 0;
	if (kobox_linux_drm_version(file, capacity, shortened) ||
	    shortened->name_length != full->name_length ||
	    shortened->date_length != full->date_length ||
	    shortened->description_length != full->description_length ||
	    memchr_inv(shortened->name, 0, sizeof(shortened->name)) ||
	    memchr_inv(shortened->date, 0, sizeof(shortened->date)) ||
	    memchr_inv(shortened->description, 0, sizeof(shortened->description)))
		goto out;
	(*checks)++;
	if (kobox_linux_drm_get_cap(file, U64_MAX, &value) != -EINVAL ||
	    value != 0xdeadbeef)
		goto out;
	(*checks)++;
	capacity[0] = sizeof(full->name) + 1;
	*shortened = *full;
	if (kobox_linux_drm_version(file, capacity, shortened) != -EINVAL ||
	    memcmp(shortened, full, sizeof(*full)))
		goto out;
	(*checks)++;
	if (kobox_linux_drm_open(0, &invalid) != -EINVAL || invalid ||
	    kobox_linux_drm_get_cap(NULL, 0, &value) != -EPERM ||
	    value != 0xdeadbeef)
		goto out;
	(*checks)++;
	result = 0;
out:
	kfree(shortened);
	kfree(full);
	return result;
}
