/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_QTEST_H
#define KOBOX_POSIX_QTEST_H

#include <stddef.h>

struct kobox_qtest;

/* Host-local connection to an owned QEMU process. QTest operates hardware
 * registers and RAM; it does not implement Linux or DRM calls. Additional
 * arguments configure devices, not another guest OS. No guest CPU is run.
 * Transactions are serialized and close requires all callers to be joined.
 */
int kobox_qtest_start(struct kobox_qtest **out, const char *executable,
		      const char *const *arguments, size_t count);
int kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			const char *format, ...) __attribute__((format(printf, 4, 5)));
int kobox_qtest_close(struct kobox_qtest *test);

#endif
