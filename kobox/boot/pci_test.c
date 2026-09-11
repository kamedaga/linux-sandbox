// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "boot_test.h"
#include "pci_config_fixture.h"

#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fixture_close(void *context)
{
	struct kobox_pci_config_fixture *fixture = context;

	fprintf(stderr, "PCI host: reads=%u writes=%u probes=%u bad_probes=%u maps=%u cache_mask=%x\n",
		fixture->reads, fixture->writes, fixture->sizing, fixture->bad_sizing,
		fixture->map_calls, fixture->cache_seen);
	if (fixture->transaction_memory) {
		if (fixture->transaction_reads != 15 || fixture->transaction_writes != 15 ||
		    munmap(fixture->transaction_memory, 0x5000))
			abort();
	}
	if (fixture->bad_sizing || fixture->sizing_pending || fixture->sizing != 12 ||
	    fixture->map_calls != fixture->unmap_calls ||
	    fixture->cache_seen != ((1U << KOBOX_MMIO_UC_MINUS) |
				   (1U << KOBOX_MMIO_WC)) || close(fixture->backing))
		abort();
}

int main(int argc, char **argv)
{
	struct kobox_pci_config_fixture fixture;
	struct kobox_linux_pci_host host;
	struct kobox_boot_test_resources resources = {
		.pci = &host, .close = fixture_close, .context = &fixture,
	};

	kobox_pci_config_fixture_init(&fixture, &host);
	fixture.backing = memfd_create("PCI BAR conformance", MFD_CLOEXEC);
	if (fixture.backing < 0 || ftruncate(fixture.backing, host.windows[0].length + 4096))
		return 1;
	if (argc == 3 && !strcmp(argv[2], "--transactions")) {
		fixture.transaction_memory = mmap(NULL, 0x5000, PROT_READ | PROT_WRITE,
						  MAP_SHARED, fixture.backing, 0);
		if (fixture.transaction_memory == MAP_FAILED) {
			close(fixture.backing);
			return 1;
		}
		kobox_pci_config_fixture_transactions(&host);
		argc--;
	}
	return kobox_boot_test_run(argc, argv, &resources);
}
