// SPDX-License-Identifier: GPL-2.0-only
/* Standard wpa_supplicant UNIX control protocol inside its own Linux netns. */
#include "wifi_control.h"
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/hex.h>
#include <linux/jiffies.h>
#include <linux/net.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/un.h>
#include <net/net_namespace.h>

struct kobox_linux_wifi_control {
	struct kobox_linux_user_process *process;
	struct socket *socket;
	struct task_struct *owner;
	u32 events;
	/* After a timed-out request the next datagram cannot be assigned safely
	 * to another command. Reopen, rather than misreporting a stale reply. */
	bool poisoned;
	char bytes[KOBOX_WIFI_CONTROL_BYTES];
};

static int validate_owner(struct kobox_linux_wifi_control *control)
{
	struct kobox_linux_user_process_status status = {.size = sizeof(status)};
	int error;

	if (!control || control->owner != current || irqs_disabled() || in_atomic())
		return -EINVAL;
	error = kobox_linux_user_process_status(control->process, &status);
	if (error)
		return error;
	if (status.exited || status.launch_error)
		return status.launch_error ?: -EPIPE;
	return 0;
}

static bool event_message(struct kobox_linux_wifi_control *control, size_t length)
{
	const char *text = control->bytes;
	/* An attached global socket prefixes events with a priority. IFNAME may
	 * precede or follow it; never interpret an ordinary command reply as one. */
	if (length >= 7 && !memcmp(text, "IFNAME=", 7)) {
		const char *space = memchr(text, ' ', length);
		if (!space)
			return false;
		length -= space + 1 - text;
		text = space + 1;
	}
	if (length && text[0] == '<') {
		if (strnstr(text, "CTRL-EVENT-SCAN-RESULTS", length))
			control->events |= KOBOX_WIFI_SCAN_COMPLETE;
		if (strnstr(text, "CTRL-EVENT-SCAN-FAILED", length))
			control->events |= KOBOX_WIFI_SCAN_FAILED;
		if (strnstr(text, "CTRL-EVENT-CONNECTED", length))
			control->events |= KOBOX_WIFI_CONNECTED;
		if (strnstr(text, "CTRL-EVENT-DISCONNECTED", length))
			control->events |= KOBOX_WIFI_DISCONNECTED;
		if (strnstr(text, "CTRL-EVENT-SSID-TEMP-DISABLED", length))
			control->events |= KOBOX_WIFI_AUTH_FAILED;
		return true;
	}
	return false;
}

static int receive(struct kobox_linux_wifi_control *control, bool *event)
{
	struct msghdr message = {};
	struct kvec vector = {.iov_base = control->bytes, .iov_len = sizeof(control->bytes)};
	int result = kernel_recvmsg(control->socket, &message, &vector, 1,
		sizeof(control->bytes), MSG_DONTWAIT);

	if (result < 0)
		return result;
	if (message.msg_flags & MSG_TRUNC)
		return -EMSGSIZE;
	*event = event_message(control, result);
	return result;
}

__attribute__((visibility("default")))
int kobox_linux_wifi_control_request(struct kobox_linux_wifi_control *control,
	const void *request, size_t length, void *reply, size_t capacity,
	size_t *received, u32 timeout_ms)
{
	struct msghdr message = {.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL};
	struct kvec vector = {.iov_base = (void *)request, .iov_len = length};
	unsigned long deadline;
	int result = validate_owner(control);

	if (result)
		return result;
	if (!request || !length || length > 4096 || !reply || !capacity ||
	    capacity > KOBOX_WIFI_CONTROL_BYTES || !received ||
	    !timeout_ms || timeout_ms > 60000)
		return -EINVAL;
	*received = 0;
	if (control->poisoned)
		return -EPIPE;
	/* No monitor can consume a response concurrently: the Linux opening
	 * task alone owns this socket and all control operations. */
	result = kernel_sendmsg(control->socket, &message, &vector, 1, length);
	if (result < 0)
		return result;
	if (result != length) {
		control->poisoned = true;
		return -EIO;
	}
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	for (;;) {
		bool event = false;

		result = receive(control, &event);
		if (result >= 0 && !event) {
			if (result > capacity)
				return -EMSGSIZE;
			memcpy(reply, control->bytes, result);
			*received = result;
			return 0;
		}
		if (result < 0 && result != -EAGAIN)
			break;
		result = validate_owner(control);
		if (result)
			break;
		if (time_after_eq(jiffies, deadline)) {
			result = -ETIMEDOUT;
			break;
		}
		if (!event || result == -EAGAIN)
			schedule_timeout_uninterruptible(1);
	}
	control->poisoned = true;
	return result;
}

__attribute__((visibility("default")))
int kobox_linux_wifi_control_open(struct kobox_linux_user_process *process,
	struct kobox_linux_wifi_control **out, u32 timeout_ms)
{
	struct sockaddr_un local = {.sun_family = AF_UNIX};
	struct sockaddr_un remote = {.sun_family = AF_UNIX,
		.sun_path = "\0" KOBOX_WIFI_CONTROL_NAME};
	struct kobox_linux_wifi_control *control;
	unsigned long deadline;
	u8 random[16];
	size_t length;
	char reply[16];
	int result;

	if (!process || !out || *out || !timeout_ms || timeout_ms > 60000)
		return -EINVAL;
	control = kzalloc(sizeof(*control), GFP_KERNEL);
	if (!control)
		return -ENOMEM;
	control->process = process;
	control->owner = current;
	result = validate_owner(control);
	if (result)
		goto free;
	result = sock_create_kern(&init_net, AF_UNIX, SOCK_DGRAM, 0, &control->socket);
	if (result)
		goto free;
	/* Use a full padded sockaddr, matching upstream ctrl_iface_unix.c's
	 * abstract bind length exactly. Filesystem mounts are never borrowed. */
	get_random_bytes(random, sizeof(random));
	bin2hex(local.sun_path + 1, random, sizeof(random));
	result = kernel_bind(control->socket, (struct sockaddr *)&local, sizeof(local));
	if (result)
		goto release;
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	for (;;) {
		result = kernel_connect(control->socket, (struct sockaddr *)&remote,
			sizeof(remote), O_NONBLOCK);
		if (!result)
			break;
		if (result != -ECONNREFUSED && result != -ENOENT)
			goto release;
		result = validate_owner(control);
		if (result)
			goto release;
		if (time_after_eq(jiffies, deadline)) {
			result = -ETIMEDOUT;
			goto release;
		}
		schedule_timeout_uninterruptible(1);
	}
	result = kobox_linux_wifi_control_request(control, "ATTACH", 6,
		reply, sizeof(reply), &length, timeout_ms);
	if (!result && (length != 3 || memcmp(reply, "OK\n", 3)))
		result = -EPROTO;
	if (result)
		goto release;
	*out = control;
	return 0;
release:
	sock_release(control->socket);
free:
	kfree(control);
	return result;
}

__attribute__((visibility("default")))
int kobox_linux_wifi_control_events(struct kobox_linux_wifi_control *control, u32 *events)
{
	int result = validate_owner(control);
	unsigned budget;

	if (result)
		return result;
	if (!events)
		return -EINVAL;
	if (control->poisoned)
		return -EPIPE;
	for (budget = 0; budget < 32; budget++) {
		bool event;

		result = receive(control, &event);
		if (result == -EAGAIN)
			break;
		if (result < 0 || !event) {
			control->poisoned = true;
			return result < 0 ? result : -EPROTO;
		}
	}
	*events = control->events;
	control->events = 0;
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_wifi_control_close(struct kobox_linux_wifi_control *control)
{
	/* Closing an exited child's control socket remains valid. */
	if (!control || control->owner != current || irqs_disabled() || in_atomic())
		return -EINVAL;
	sock_release(control->socket);
	kfree(control);
	return 0;
}
