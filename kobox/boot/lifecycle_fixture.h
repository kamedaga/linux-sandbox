/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_LIFECYCLE_FIXTURE_H
#define KOBOX_BOOT_LIFECYCLE_FIXTURE_H

#include "lifecycle.h"

#include <pthread.h>
#include <stdatomic.h>

/* Owns the socket on successful start; cold-process test transport only. */
struct kobox_lifecycle_fixture {
	struct kobox_linux_lifecycle port;
	pthread_t receiver;
	atomic_int terminal;
	uint64_t generation;
	int socket;
	int start;
};

int kobox_lifecycle_fixture_start(struct kobox_lifecycle_fixture *fixture,
				 int socket, uint64_t generation);
void kobox_lifecycle_fixture_close(struct kobox_lifecycle_fixture *fixture);

#endif
