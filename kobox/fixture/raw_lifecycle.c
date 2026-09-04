// SPDX-License-Identifier: GPL-2.0-only

#ifndef KOBOX_RAW_LIFECYCLE_STATUS
#define KOBOX_RAW_LIFECYCLE_STATUS 0
#endif

int init_module(void)
{
	return KOBOX_RAW_LIFECYCLE_STATUS;
}

void cleanup_module(void)
{
}
