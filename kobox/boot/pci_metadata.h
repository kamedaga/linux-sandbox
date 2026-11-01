/* SPDX-License-Identifier: MIT */
#ifndef KOBOX_BOOT_PCI_METADATA_H
#define KOBOX_BOOT_PCI_METADATA_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* A current-boot, owner-read description of BAR-less companion functions.
 * These bytes convey identification/configuration state, never MMIO, DMA,
 * interrupts or permission to change another function's configuration. */
#define KOBOX_PCI_METADATA_MAX 2u
struct kobox_linux_pci_metadata {
    uint32_t devfn;
    uint8_t config[256];
};
struct kobox_linux_pci_topology {
    uint32_t segment, bus, devfn, metadata_count;
    struct kobox_linux_pci_metadata metadata[KOBOX_PCI_METADATA_MAX];
};

static inline int kobox_pci_metadata_valid(const struct kobox_linux_pci_topology *t)
{
    if (!t || t->segment > 65535 || t->bus > 255 || t->devfn > 255 ||
        t->metadata_count > KOBOX_PCI_METADATA_MAX)
        return 0;
    for (uint32_t i = 0; i < t->metadata_count; ++i) {
        const struct kobox_linux_pci_metadata *m = &t->metadata[i];
        const uint8_t *c = m->config;
        if (m->devfn > 255 || (m->devfn & 7) || m->devfn == t->devfn ||
            (c[0] == 255 && c[1] == 255) || (!c[0] && !c[1]) ||
            (c[14] & 127) || c[11] != 6 || c[9] || c[10] > 1)
            return 0;
        /* No hidden resource may become accessible through this view. */
        for (uint32_t b = 16; b < 40; ++b)
            if (c[b]) return 0;
        for (uint32_t b = 48; b < 52; ++b)
            if (c[b]) return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (m->devfn == t->metadata[j].devfn ||
                c[10] == t->metadata[j].config[10])
                return 0;
    }
    return 1;
}

#endif
