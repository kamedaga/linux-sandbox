/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_NET_PORT_H
#define KOBOX_BOOT_NET_PORT_H

#include "../task/host.h"

/* The hosted driver owns sk_buffs and the device queues. This process-local
 * port copies complete Ethernet frames across the OS boundary; no Linux
 * pointer, DMA address, or net_device layout is exposed to the host. */
#define KOBOX_NET_FRAME_MAX 2048U

struct kobox_linux_net_frame {
	uint16_t length;
	uint8_t bytes[KOBOX_NET_FRAME_MAX];
};

struct kobox_linux_net_info {
	uint64_t sequence;
	/* Changes on WiFi authorization transitions and each observed CONNECTED.
	 * Native L3 must retire its old policy before forwarding a new epoch. */
	uint64_t association_sequence;
	uint64_t rx_drops;
	uint32_t mtu;
	uint8_t mac[6];
	uint8_t carrier;
	uint8_t reserved;
};

#ifdef __KERNEL__
struct pci_dev;
struct kobox_linux_net_port;

int kobox_linux_net_port_open(struct kobox_linux_net_port **out,
			     struct pci_dev *pci,
			     void (*progress)(void *context, unsigned phase,
					      size_t module_index, int status),
			     void *progress_context);
void kobox_linux_net_port_close(struct kobox_linux_net_port *port);
int kobox_linux_net_port_info(struct kobox_linux_net_port *port,
			     struct kobox_linux_net_info *info);
int kobox_linux_net_port_read(struct kobox_linux_net_port *port,
			     struct kobox_linux_net_frame *frames,
			     size_t capacity, size_t *count);
int kobox_linux_net_port_write(struct kobox_linux_net_port *port,
			      const void *frame, size_t length);
int kobox_linux_net_port_name(struct kobox_linux_net_port *port,
			     char *name, size_t capacity);
int kobox_linux_net_wifi_request(struct kobox_linux_net_port *port,
	const void *request, size_t length, void *reply, size_t capacity, size_t *received);
int kobox_linux_net_wifi_configure(struct kobox_linux_net_port *port,
	const void *config, size_t length);
int kobox_linux_net_wifi_events(struct kobox_linux_net_port *port, uint32_t *events);

struct kobox_linux_wifi_service;
/* Opening-task-only binding; a failed stop keeps this binding and the port
 * live. Packet delivery uses the separately locked authorization gate. */
int kobox_linux_net_port_wifi_bind(struct kobox_linux_net_port *port,
	struct kobox_linux_wifi_service *service);
void kobox_linux_net_port_wifi_authorize(struct kobox_linux_net_port *port, bool authorized);
void kobox_linux_net_port_wifi_association(struct kobox_linux_net_port *port);
#endif

#endif
