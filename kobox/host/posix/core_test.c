// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "core.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
	if (!(value)) { \
		fprintf(stderr, "core TLS check at %u: %s\n", __LINE__, #value); \
		return 1; \
	} \
} while (0)

static unsigned long *(*probe)(void);
static pthread_barrier_t barrier;

struct tls_thread {
	uintptr_t address;
	unsigned long value;
	int failed;
};

static void *run_thread(void *argument)
{
	struct tls_thread *thread = argument;
	unsigned long *slot = probe();
	unsigned int i;

	thread->address = (uintptr_t)slot;
	thread->failed = *slot != 0x12345678UL || thread->address % 64;
	for (i = 0; i < 1000; i++) {
		*slot = thread->value + i;
		pthread_barrier_wait(&barrier);
		if (probe() != slot || *probe() != thread->value + i)
			thread->failed = 1;
		pthread_barrier_wait(&barrier);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct kobox_posix_core *native = NULL, *duplicate = NULL;
	struct kobox_boot_core *core;
	struct tls_thread threads[2] = {{.value = 10000}, {.value = 20000}};
	pthread_t ids[2];
	unsigned long *main_slot;
	void *address;
	unsigned int i;

	CHECK(argc == 2);
	CHECK(kobox_posix_core_open(NULL, &native) == EINVAL && !native);
	CHECK(!kobox_posix_core_open(argv[1], &native));
	core = kobox_posix_core_boot(native);
	CHECK(core && core->start && core->dispatch);
	CHECK(kobox_posix_core_open(argv[1], &duplicate) != 0 && !duplicate);
	address = core->lookup(core->loader, "kobox_linux_tls_probe");
	CHECK(address);
	memcpy(&probe, &address, sizeof(probe));
	main_slot = probe();
	CHECK(*main_slot == 0x12345678UL && !((uintptr_t)main_slot % 64));
	*main_slot = 77;
	CHECK(!pthread_barrier_init(&barrier, NULL, 2));
	for (i = 0; i < 2; i++)
		CHECK(!pthread_create(&ids[i], NULL, run_thread, &threads[i]));
	for (i = 0; i < 2; i++)
		CHECK(!pthread_join(ids[i], NULL) && !threads[i].failed);
	CHECK(threads[0].address != threads[1].address &&
	      threads[0].address != (uintptr_t)main_slot &&
	      threads[1].address != (uintptr_t)main_slot);
	CHECK(probe() == main_slot && *main_slot == 77);
	CHECK(!pthread_barrier_destroy(&barrier));
	CHECK(!kobox_posix_core_close(&native) && !native);
	puts("Core loader: explicit TLS binding, initializer, alignment and thread isolation verified");
	return 0;
}
