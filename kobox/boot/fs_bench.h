/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_BENCH_H
#define KOBOX_BOOT_FS_BENCH_H

#include <linux/types.h>

struct vfsmount;

#ifdef KOBOX_FS_INTERNAL_BENCH
static inline u64 kobox_fs_bench_ticks(void)
{
	u32 lo, hi;

	asm volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
	return ((u64)hi << 32) | lo;
}

int kobox_linux_fs_benchmark(struct vfsmount *root);
#else
static inline int kobox_linux_fs_benchmark(struct vfsmount *root)
{
	return 0;
}
#endif

#endif /* KOBOX_BOOT_FS_BENCH_H */
