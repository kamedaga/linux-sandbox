// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "qtest.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct kobox_qtest {
	pthread_mutex_t lock;
	pid_t pid;
	int socket;
	bool failed;
};

static int ready(int descriptor)
{
	struct pollfd pollfd = {.fd = descriptor, .events = POLLIN};
	int result;

	do {
		result = poll(&pollfd, 1, 5000);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return errno;
	if (!result)
		return ETIMEDOUT;
	return pollfd.revents & POLLIN ? 0 : EPIPE;
}

int kobox_qtest_start(struct kobox_qtest **out, const char *executable,
		      const char *const *arguments, size_t count)
{
	char directory[] = ".kobox-qtest-XXXXXX", endpoint[128];
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	struct kobox_qtest *test;
	char *argv[80];
	size_t index, used = 0;
	int listener = -1, result;
	bool bound = false;

	if (!out || !executable || (count && !arguments) || count > 64)
		return EINVAL;
	*out = NULL;
	test = calloc(1, sizeof(*test));
	if (!test)
		return ENOMEM;
	test->socket = -1;
	result = pthread_mutex_init(&test->lock, NULL);
	if (result) {
		free(test);
		return result;
	}
	if (!mkdtemp(directory)) {
		result = errno;
		goto release;
	}
	snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
	snprintf(endpoint, sizeof(endpoint), "unix:%s", address.sun_path);
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0) {
		result = errno;
		goto remove;
	}
	if (bind(listener, (struct sockaddr *)&address, sizeof(address))) {
		result = errno;
		goto remove;
	}
	bound = true;
	if (listen(listener, 1)) {
		result = errno;
		goto remove;
	}
	argv[used++] = (char *)executable;
	/* qtest has no executing guest CPU. Unlike -S, it leaves device clocks
	 * enabled so clock_step can run real device timers deterministically.
	 */
	argv[used++] = "-accel";
	argv[used++] = "qtest";
	argv[used++] = "-display";
	argv[used++] = "none";
	argv[used++] = "-nodefaults";
	argv[used++] = "-no-user-config";
	argv[used++] = "-qtest";
	argv[used++] = endpoint;
	argv[used++] = "-qtest-log";
	argv[used++] = "/dev/null";
	for (index = 0; index < count; index++)
		argv[used++] = (char *)arguments[index];
	argv[used] = NULL;
	result = posix_spawnp(&test->pid, executable, NULL, NULL, argv, environ);
	if (result)
		goto remove;
	result = ready(listener);
	if (result)
		goto remove;
	test->socket = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	if (test->socket < 0)
		result = errno;
remove:
	if (listener >= 0)
		close(listener);
	if (bound)
		unlink(address.sun_path);
	rmdir(directory);
release:
	if (result) {
		int cleanup = kobox_qtest_close(test);

		return cleanup ? cleanup : result;
	}
	*out = test;
	return 0;
}

int kobox_qtest_command(struct kobox_qtest *test, char *response, size_t capacity,
			const char *format, ...)
{
	char command[256], byte;
	va_list arguments;
	size_t offset = 0, length;
	ssize_t transferred;
	int result, formatted;

	if (!test || !response || capacity < 4 || !format)
		return EINVAL;
	va_start(arguments, format);
	formatted = vsnprintf(command, sizeof(command) - 1, format, arguments);
	va_end(arguments);
	if (formatted < 0 || (size_t)formatted >= sizeof(command) - 1)
		return EOVERFLOW;
	if (strchr(command, '\n') || strchr(command, '\r'))
		return EINVAL;
	length = formatted;
	command[length++] = '\n';
	result = pthread_mutex_lock(&test->lock);
	if (result)
		return result;
	if (test->failed) {
		result = EPIPE;
		goto out;
	}
	while (offset < length) {
		transferred = send(test->socket, command + offset, length - offset, MSG_NOSIGNAL);
		if (transferred < 0 && errno == EINTR)
			continue;
		if (transferred <= 0) {
			result = transferred < 0 ? errno : EPIPE;
			goto failed;
		}
		offset += transferred;
	}
	for (offset = 0; offset < capacity - 1; offset++) {
		result = ready(test->socket);
		if (result)
			goto failed;
		do {
			transferred = recv(test->socket, &byte, 1, 0);
		} while (transferred < 0 && errno == EINTR);
		if (transferred != 1) {
			result = transferred < 0 ? errno : EPIPE;
			goto failed;
		}
		if (byte == '\n') {
			response[offset] = 0;
			result = !strcmp(response, "OK") || !strncmp(response, "OK ", 3) ? 0 : EPROTO;
			goto out;
		}
		response[offset] = byte;
	}
	result = EOVERFLOW;
failed:
	/* A partial transaction cannot be reused as a new register response. */
	test->failed = true;
out:
	if (pthread_mutex_unlock(&test->lock))
		abort();
	return result;
}

int kobox_qtest_close(struct kobox_qtest *test)
{
	int status, result;
	pid_t waited;

	if (!test)
		return EINVAL;
	if (test->pid > 0) {
		/* This PID was created here and has never been reaped or recycled. */
		if (kill(test->pid, SIGKILL) && errno != ESRCH)
			return errno;
		do {
			waited = waitpid(test->pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited != test->pid)
			return waited < 0 ? errno : ECHILD;
		test->pid = 0;
	}
	if (test->socket >= 0)
		close(test->socket);
	result = pthread_mutex_destroy(&test->lock);
	if (result)
		return result;
	free(test);
	return 0;
}
