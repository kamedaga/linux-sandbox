// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_barrier_t barrier;
static int sockets[2];

static void *worker(void *unused)
{
	char byte, text[128];
	struct iovec iov = { &byte, 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

	(void)unused;
	assert(recvmsg(sockets[1], &msg, MSG_DONTWAIT) == -1);
	assert(errno == EAGAIN);
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	errno = ENOSYS;
	assert(strerror_r(ENOSYS, text, sizeof(text)) == 0);
	assert(errno == ENOSYS && text[0]);
	return NULL;
}

int main(void)
{
	char byte = 0, text[128];
	struct iovec iov = { &byte, 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	pthread_t thread;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	assert(write(sockets[0], "x", 1) == 1);
	errno = EDOM;
	assert(recvmsg(sockets[1], &msg, 0) == 1 && byte == 'x');
	assert(errno == EDOM);
	assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
	assert(pthread_create(&thread, NULL, worker, NULL) == 0);
	pthread_barrier_wait(&barrier);
	assert(recvmsg(-1, &msg, 0) == -1 && errno == EBADF);
	errno = ENOSYS;
	assert(strerror_r(ENOSYS, text, sizeof(text)) == 0);
	assert(errno == ENOSYS && text[0]);
	pthread_barrier_wait(&barrier);
	assert(pthread_join(thread, NULL) == 0);
	for (unsigned int i = 0; i < 24; i++) {
		errno = EDOM;
		assert(strerror_r(ENOSYS, text, sizeof(text)) == 0);
		assert(errno == EDOM);
	}
	pthread_barrier_destroy(&barrier);
	close(sockets[0]);
	close(sockets[1]);
	return 0;
}
