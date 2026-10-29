// SPDX-License-Identifier: GPL-2.0-only
/* Supplicant policy stays in Linux user space; this adapter owns its lifetime. */
#include "wifi_service.h"
#include "net_port.h"
#include "diagnostic.h"
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/fcntl.h>
#include <linux/hex.h>
#include <linux/jiffies.h>
#include <linux/net.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/un.h>
#include <net/net_namespace.h>

#define WIFI_CAPABILITIES (BIT_ULL(CAP_NET_ADMIN) | BIT_ULL(CAP_NET_RAW))
#define WIFI_REQUEST_MS 2000U
#define WIFI_START_MS 10000U

struct kobox_linux_wifi_service {
	struct kobox_linux_user_process *process;
	struct kobox_linux_wifi_control *control;
	struct kobox_linux_net_port *port;
	struct task_struct *owner;
	u32 pending_events;
	/* STATUS uses a separate, non-ATTACH socket. INFO never waits for a
	 * supplicant reply and cannot consume another command's response. */
	struct socket *status_socket;
	u64 association_epoch, status_epoch;
	unsigned long status_deadline;
	bool status_pending, status_needed;
	char interface[IFNAMSIZ];
	char status_request[IFNAMSIZ + 16];
	char status_reply[KOBOX_WIFI_CONTROL_BYTES];
	char request[4097];
	bool failed, bound, configuration_applied;
};

static int validate_service(struct kobox_linux_wifi_service *service)
{
	if (!service || service->owner != current || irqs_disabled() || in_atomic())
		return -EINVAL;
	return service->failed || !service->control ? -EPIPE : 0;
}

static int fail_service(struct kobox_linux_wifi_service *service, int error)
{
	service->failed = true;
	kobox_linux_net_port_wifi_authorize(service->port, false);
	return error < 0 ? error : -EPROTO;
}

static int process_alive(struct kobox_linux_wifi_service *service)
{
	struct kobox_linux_user_process_status status = {.size = sizeof(status)};
	int error = kobox_linux_user_process_status(service->process, &status);

	if (error)
		return error;
	return status.exited || status.launch_error ? (status.launch_error ?: -EPIPE) : 0;
}

static int startup_failure(struct kobox_linux_wifi_service *service,
	const char *phase, int error, bool empty_configuration)
{
	struct kobox_linux_user_process_status status = {.size = sizeof(status)};
	int observed = service->process ?
		kobox_linux_user_process_status(service->process, &status) : -ENODEV;

	kobox_linux_boot_diagnostic("wifi startup phase=%s error=%d status-query=%d exec=%u exited=%u launch=%d exit-code=%d signal=%d\n",
		phase, error, observed, status.exec_ready, status.exited,
		status.launch_error, status.exit_code, status.term_signal);
	/* Only the known empty bootstrap configuration is eligible. Never dump
	 * application output from a caller-provided network/credential config.
	 * Read once, with bounded records; diagnostics cannot prolong a timeout. */
	if (empty_configuration && service->process) {
		char bytes[1024];
		size_t length = 0, offset = 0;
		unsigned int records = 0;
		int result = kobox_linux_user_process_read_log(service->process, 0,
			bytes, sizeof(bytes), &length);

		kobox_linux_boot_diagnostic("wifi startup output-read=%d bytes=%zu\n", result, length);
		while (!result && offset < length && records++ < 8) {
			char line[161];
			size_t used = 0;

			while (offset < length && bytes[offset] != '\n') {
				unsigned char c = bytes[offset++];
				if (used < sizeof(line) - 1)
					line[used++] = c >= 32 && c < 127 ? c : '?';
			}
			if (offset < length) offset++;
			line[used] = 0;
			kobox_linux_boot_diagnostic("wifi startup output: %s\n", line);
		}
	}
	return error;
}

static int open_status_socket(struct kobox_linux_wifi_service *service)
{
	struct sockaddr_un local = {.sun_family = AF_UNIX};
	struct sockaddr_un remote = {.sun_family = AF_UNIX,
		.sun_path = "\0" KOBOX_WIFI_CONTROL_NAME};
	u8 random[16];
	int error = sock_create_kern(&init_net, AF_UNIX, SOCK_DGRAM, 0, &service->status_socket);

	if (error)
		return error;
	get_random_bytes(random, sizeof(random));
	memcpy(local.sun_path + 1, "status-", 7);
	bin2hex(local.sun_path + 8, random, sizeof(random));
	memzero_explicit(random, sizeof(random));
	/* Match upstream's padded abstract sockaddr length; mounts/fs stay put. */
	error = kernel_bind(service->status_socket, (struct sockaddr *)&local, sizeof(local));
	if (!error)
		error = kernel_connect(service->status_socket, (struct sockaddr *)&remote,
			sizeof(remote), O_NONBLOCK);
	return error;
}

static int poll_control_events(struct kobox_linux_wifi_service *service)
{
	u32 events;
	int error = kobox_linux_wifi_control_events(service->control, &events);

	if (error)
		return error;
	service->pending_events |= events;
	if (events & (KOBOX_WIFI_CONNECTED | KOBOX_WIFI_DISCONNECTED | KOBOX_WIFI_AUTH_FAILED)) {
		service->association_epoch++;
		service->status_needed = true;
		/* Down wins until a STATUS for this exact observation epoch is
		 * received. Never authorize a new association from an older reply. */
		if (events & KOBOX_WIFI_CONNECTED)
			kobox_linux_net_port_wifi_association(service->port);
		else
			kobox_linux_net_port_wifi_authorize(service->port, false);
	}
	return 0;
}

int kobox_linux_wifi_service_poll_events(struct kobox_linux_wifi_service *service)
{
	int error = validate_service(service), completed = -1;
	u64 epoch = 0;
	bool have_reply = false;

	if (error)
		return error;
	error = process_alive(service);
	if (!error)
		error = poll_control_events(service);
	if (error)
		return fail_service(service, error);
	if (!service->status_socket)
		return fail_service(service, -EPIPE);
	if (service->status_pending) {
		struct msghdr message = {};
		struct kvec vector = {.iov_base = service->status_reply,
			.iov_len = sizeof(service->status_reply)};
		int received = kernel_recvmsg(service->status_socket, &message, &vector, 1,
			sizeof(service->status_reply), MSG_DONTWAIT);

		if (received >= 0) {
			service->status_pending = false;
			if (message.msg_flags & MSG_TRUNC)
				return fail_service(service, -EMSGSIZE);
			completed = kobox_wifi_status_completed(service->status_reply, received);
			memzero_explicit(service->status_reply, sizeof(service->status_reply));
			have_reply = true;
			epoch = service->status_epoch;
		} else if (received != -EAGAIN) {
			return fail_service(service, received);
		} else if (time_after_eq(jiffies, service->status_deadline)) {
			return fail_service(service, -ETIMEDOUT);
		}
	}
	/* A disconnect/roam may arrive between STATUS processing and its reply.
	 * Recheck the monitor socket before publishing that snapshot's authority. */
	error = poll_control_events(service);
	if (error)
		return fail_service(service, error);
	if (have_reply) {
		if (epoch == service->association_epoch) {
			if (completed < 0)
				return fail_service(service, completed);
			kobox_linux_net_port_wifi_authorize(service->port,
				completed == 1 && service->configuration_applied);
		} else {
			/* An obsolete STATUS may even say FAIL during removal/roam.
			 * Consume it without confusing it with this epoch's result. */
			service->status_needed = true;
		}
	}
	if (service->status_needed && !service->status_pending) {
		struct msghdr message = {.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL};
		size_t length = strlen(service->status_request);
		struct kvec vector = {.iov_base = service->status_request, .iov_len = length};
		int sent = kernel_sendmsg(service->status_socket, &message, &vector, 1, length);

		if (sent == -EAGAIN)
			return 0;
		if (sent < 0 || sent != length)
			return fail_service(service, sent < 0 ? sent : -EIO);
		service->status_pending = true;
		service->status_needed = false;
		service->status_epoch = service->association_epoch;
		service->status_deadline = jiffies + msecs_to_jiffies(WIFI_REQUEST_MS);
	}
	return 0;
}

int kobox_linux_wifi_service_monitor(void *opaque)
{
	struct kobox_linux_wifi_service *service = opaque;
	int error = validate_service(service);

	if (error)
		return error;
	if (!service->status_pending)
		service->status_needed = true;
	return kobox_linux_wifi_service_poll_events(service);
}

int kobox_linux_wifi_service_open(struct kobox_linux_wifi_service **out,
	struct kobox_linux_net_port *port,
	const struct kobox_linux_user_process_config *input)
{
	struct kobox_linux_wifi_service *service;
	struct kobox_linux_user_process_config config;
	const char *argv[10];
	bool program = false, configuration = false, empty_configuration = false;
	size_t index, received;
	char reply[16];
	int error;

	if (!out || *out || !port || !input || input->size != sizeof(*input) ||
	    !input->files || !input->file_count || input->file_count > 64 ||
	    input->capability_mask != WIFI_CAPABILITIES || irqs_disabled() || in_atomic())
		return -EINVAL;
	for (index = 0; index < input->file_count; ++index) {
		const struct kobox_linux_user_process_file *file = &input->files[index];

		if (!file->path)
			return -EINVAL;
		if (!strcmp(file->path, KOBOX_WIFI_PROGRAM))
			program = file->mode == 0755 && file->data && file->size;
		if (!strcmp(file->path, KOBOX_WIFI_CONFIG)) {
			configuration = file->mode == 0600 && file->data && file->size &&
				file->size <= KOBOX_WIFI_CONFIG_MAX;
			empty_configuration = configuration && file->size == sizeof("update_config=0\n") - 1 &&
				!memcmp(file->data, "update_config=0\n", sizeof("update_config=0\n") - 1);
		}
	}
	if (!program || !configuration)
		return -EINVAL;
	service = kzalloc(sizeof(*service), GFP_KERNEL);
	if (!service)
		return -ENOMEM;
	service->port = port;
	service->owner = current;
	service->configuration_applied = true;
	error = kobox_linux_net_port_name(port, service->interface, sizeof(service->interface));
	if (error)
		goto free;
	error = kobox_linux_net_port_wifi_bind(port, service);
	if (error)
		goto free;
	service->bound = true;
	*out = service;
	kobox_linux_net_port_wifi_authorize(port, false);
	snprintf(service->status_request, sizeof(service->status_request),
		"IFNAME=%s STATUS", service->interface);
	argv[0] = KOBOX_WIFI_PROGRAM;
	argv[1] = "-Dnl80211";
	argv[2] = "-i";
	argv[3] = service->interface;
	argv[4] = "-c";
	argv[5] = KOBOX_WIFI_CONFIG;
	argv[6] = "-g";
	argv[7] = KOBOX_WIFI_CONTROL_ARGUMENT;
	argv[8] = NULL;
	config = *input;
	config.argv = argv;
	error = kobox_linux_user_process_spawn(&config, &service->process);
	if (error)
		return startup_failure(service, "spawn", error, empty_configuration);
	kobox_linux_boot_diagnostic("wifi startup phase=spawned\n");
	error = kobox_linux_wifi_control_open(service->process, &service->control, WIFI_START_MS);
	if (error)
		return startup_failure(service, "control-open", error, empty_configuration);
	kobox_linux_boot_diagnostic("wifi startup phase=control-ready\n");
	error = kobox_linux_wifi_control_request(service->control, "PING", 4,
		reply, sizeof(reply), &received, WIFI_REQUEST_MS);
	if (!error && (received != 5 || memcmp(reply, "PONG\n", 5)))
		error = -EPROTO;
	if (!error)
		error = open_status_socket(service);
	if (!error)
		error = kobox_linux_wifi_service_monitor(service);
	return error ? startup_failure(service, "ping-status", error, empty_configuration) : 0;
free:
	kfree(service);
	return error;
}

int kobox_linux_wifi_service_request(struct kobox_linux_wifi_service *service,
	const void *request, size_t length, void *reply, size_t capacity, size_t *received)
{
	size_t qualified_length;
	int error = validate_service(service);

	if (error)
		return error;
	error = kobox_wifi_qualify_request(service->interface, request, length,
		service->request, sizeof(service->request), &qualified_length);
	if (error)
		return error;
	if ((length == 10 && !memcmp(request, "DISCONNECT", 10)) ||
	    (length == 9 && (!memcmp(request, "TERMINATE", 9) || !memcmp(request, "RECONNECT", 9))) ||
	    (length == 11 && !memcmp(request, "REASSOCIATE", 11)))
		kobox_linux_net_port_wifi_authorize(service->port, false);
	error = kobox_linux_wifi_control_request(service->control, service->request, qualified_length,
		reply, capacity, received, WIFI_REQUEST_MS);
	memzero_explicit(service->request, sizeof(service->request));
	if (error && error != -EINVAL)
		return fail_service(service, error);
	return error ?: kobox_linux_wifi_service_poll_events(service);
}

int kobox_linux_wifi_service_configure(struct kobox_linux_wifi_service *service,
	const void *config, size_t length)
{
	char request[IFNAMSIZ + 24], reply[16];
	size_t received;
	int error = validate_service(service);

	if (error)
		return error;
	if (!config || !length || length > KOBOX_WIFI_CONFIG_MAX || memchr(config, 0, length))
		return -EINVAL;
	kobox_linux_net_port_wifi_authorize(service->port, false);
	service->configuration_applied = false;
	service->association_epoch++;
	service->status_needed = true;
	error = kobox_linux_user_process_replace_file(service->process, KOBOX_WIFI_CONFIG, config, length);
	if (error)
		return error;
	snprintf(request, sizeof(request), "IFNAME=%s RECONFIGURE", service->interface);
	error = kobox_linux_wifi_control_request(service->control, request, strlen(request),
		reply, sizeof(reply), &received, WIFI_REQUEST_MS);
	if (error)
		return fail_service(service, error);
	if (received != 3 || memcmp(reply, "OK\n", 3))
		return -EINVAL;
	service->configuration_applied = true;
	return kobox_linux_wifi_service_poll_events(service);
}

int kobox_linux_wifi_service_events(struct kobox_linux_wifi_service *service, u32 *events)
{
	int error = validate_service(service);

	if (error)
		return error;
	if (!events)
		return -EINVAL;
	error = kobox_linux_wifi_service_poll_events(service);
	if (error)
		return error;
	*events = service->pending_events;
	service->pending_events = 0;
	return 0;
}

int kobox_linux_wifi_service_close(struct kobox_linux_wifi_service *service)
{
	int error;

	if (!service || service->owner != current || irqs_disabled() || in_atomic())
		return -EINVAL;
	kobox_linux_net_port_wifi_authorize(service->port, false);
	/* Stop must prove task/binding/native drain. A failed stop preserves the
	 * control, program, port and driver modules for process-level retirement. */
	if (service->process) {
		error = kobox_linux_user_process_stop(service->process, 2000, 2000);
		if (error)
			return error;
	}
	if (service->status_socket) {
		sock_release(service->status_socket);
		service->status_socket = NULL;
	}
	if (service->control) {
		error = kobox_linux_wifi_control_close(service->control);
		if (error)
			return error;
		service->control = NULL;
	}
	if (service->process) {
		error = kobox_linux_user_process_destroy(service->process);
		if (error)
			return error;
		service->process = NULL;
	}
	if (service->bound) {
		error = kobox_linux_net_port_wifi_bind(service->port, NULL);
		if (error)
			return error;
	}
	kfree(service);
	return 0;
}
