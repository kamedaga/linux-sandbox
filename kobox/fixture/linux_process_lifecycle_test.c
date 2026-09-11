/* SPDX-License-Identifier: GPL-2.0-only */
#include "linux_process_lifecycle.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void quiesce_modules(void *context)
{
	int fd = *(int *)context;
	const char marker = 'Q';

	if (write(fd, &marker, sizeof(marker)) != sizeof(marker))
		_Exit(90);
}

static int read_marker(int fd)
{
	char marker = 0;
	ssize_t count;

	do {
		count = read(fd, &marker, sizeof(marker));
	} while (count < 0 && errno == EINTR);
	if (count != sizeof(marker) || marker != 'Q')
		return 1;
	do {
		count = read(fd, &marker, sizeof(marker));
	} while (count < 0 && errno == EINTR);
	return count == 0 ? 0 : 2;
}

int main(void)
{
	struct kobox_linux_process_lifecycle lifecycle;
	int pipe_fds[2];
	int status;
	pid_t child;

	kobox_linux_process_lifecycle_init(&lifecycle);
	if (!kobox_linux_process_begin_boot(&lifecycle))
		return 1;
	if (kobox_linux_process_begin_boot(&lifecycle))
		return 2;
	if (!kobox_linux_process_mark_running(&lifecycle))
		return 3;
	if (pipe(pipe_fds) != 0)
		return 4;
	child = fork();
	if (child < 0)
		return 5;
	if (child == 0) {
		close(pipe_fds[0]);
		kobox_linux_process_shutdown(
			&lifecycle, quiesce_modules, &pipe_fds[1], 37);
	}
	close(pipe_fds[1]);
	if (read_marker(pipe_fds[0]) != 0)
		return 6;
	close(pipe_fds[0]);
	if (waitpid(child, &status, 0) != child)
		return 7;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 37)
		return 8;
	return 0;
}
