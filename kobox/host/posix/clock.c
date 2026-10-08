/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include "../../arch/x86_64/fp_entry.h"

#include <errno.h>
#include <time.h>

static __attribute__((noinline, used)) int monotonic_body(uint64_t *time_out)
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

static __attribute__((noinline, used)) int realtime_body(uint64_t *time_out)
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

/* libc is allowed to clobber FP before returning even on errno paths.
 * Save at the ABI entry, before a C prologue, not only around clock_gettime. */
__attribute__((naked, noinline)) int kobox_posix_monotonic_ns(uint64_t *time_out __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(monotonic_body);
}

__attribute__((naked, noinline)) int kobox_posix_realtime_ns(uint64_t *time_out __attribute__((unused)))
{
	KOBOX_FP_ENTRY_BODY(realtime_body);
}
