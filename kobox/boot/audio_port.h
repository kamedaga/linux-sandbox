/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_AUDIO_PORT_H
#define KOBOX_BOOT_AUDIO_PORT_H
#include "../task/host.h"

/* Process-local PCM bridge. Only copied scalar OSS commands cross this API;
 * Linux owns PCM negotiation, buffering, DMA and the virtio-sound driver. */
struct kobox_linux_audio_port;
struct kobox_linux_audio_notify {
    void *context;
    int (*notify)(void *context);
};
#ifdef __KERNEL__
int kobox_linux_audio_port_open(struct kobox_linux_audio_port **out);
void kobox_linux_audio_port_close(struct kobox_linux_audio_port *port);
int kobox_linux_audio_open(struct kobox_linux_audio_port *port, uint32_t flags,
    const struct kobox_linux_audio_notify *notify, uint64_t *handle);
int kobox_linux_audio_close(struct kobox_linux_audio_port *port, uint64_t handle);
long kobox_linux_audio_write(struct kobox_linux_audio_port *port, uint64_t handle,
    const void *bytes, size_t size);
int kobox_linux_audio_ioctl(struct kobox_linux_audio_port *port, uint64_t handle,
    uint32_t command, void *bytes, size_t size);
int kobox_linux_audio_flags(struct kobox_linux_audio_port *port, uint64_t handle,
    uint64_t flags);
int kobox_linux_audio_poll(struct kobox_linux_audio_port *port, uint64_t handle);
#endif
#endif
