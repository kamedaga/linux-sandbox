/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"

#include <errno.h>
#include <time.h>

int kobox_posix_monotonic_ns(uint64_t *time_out)
{
	struct timespec time;

	if (!time_out)
		return EINVAL;
	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return errno;
	*time_out = (uint64_t)time.tv_sec * UINT64_C(1000000000) +
		(uint64_t)time.tv_nsec;
	return 0;
}

int kobox_posix_realtime_ns(uint64_t *time_out)
{
	struct timespec time;

	if (!time_out)
		return EINVAL;
	if (clock_gettime(CLOCK_REALTIME, &time) != 0)
		return errno;
	if (time.tv_sec < 0 ||
	    (uint64_t)time.tv_sec > UINT64_MAX / UINT64_C(1000000000))
		return EOVERFLOW;
	*time_out = (uint64_t)time.tv_sec * UINT64_C(1000000000) +
		(uint64_t)time.tv_nsec;
	return 0;
}
