// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __GLIBC__
#error "This diagnostic uses musl's POSIX strerror_r signature"
#endif

/*
 * Opt-in userspace diagnostic, never linked into the runtime. Keep the last
 * receive in TLS: an error string may describe errno changed by cleanup after
 * recvmsg, rather than the receive itself. Do not copy payloads or repair errno.
 */
struct receive_context {
	int valid, fd, error, flags, output;
	ssize_t result;
	void *caller;
};

static _Thread_local struct receive_context last_receive;
struct yield_context {
	int valid, incoming, result, error;
	void *caller;
};

static _Thread_local struct yield_context last_yield;
static ssize_t (*next_recvmsg)(int, struct msghdr *, int);
static int (*next_sched_yield)(void);
static int (*next_strerror_r)(int, char *, size_t);
static const char *(*next_g_strerror)(int);
static unsigned int records;
static unsigned int yield_records;

__attribute__((constructor)) static void context_loaded(void)
{
	int saved = errno;
	char line[128];
	int n;

	next_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
	next_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
	next_strerror_r = dlsym(RTLD_NEXT, "strerror_r");
	next_g_strerror = dlsym(RTLD_NEXT, "g_strerror");
	n = snprintf(line, sizeof(line),
		     "IPC_CONTEXT_LOADED pid=%ld recv=%p strerror=%p yield=%p\n",
		     (long)getpid(), (void *)next_recvmsg,
		     (void *)next_strerror_r, (void *)next_sched_yield);
	if (n > 0 && (size_t)n < sizeof(line))
		(void)write(2, line, n);
	errno = saved;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
	ssize_t result;
	int saved;

	if (!next_recvmsg) {
		errno = ENOSYS;
		return -1;
	}
	result = next_recvmsg(fd, msg, flags);
	saved = errno;
	last_receive = (struct receive_context) {
		.valid = 1, .fd = fd, .error = saved, .flags = flags,
		.output = result >= 0 && msg ? msg->msg_flags : 0,
		.result = result, .caller = __builtin_return_address(0),
	};
	last_yield.valid = 0;
	errno = saved;
	return result;
}

int sched_yield(void)
{
	int incoming = errno, result, saved, n;
	char line[320];
	void *caller = __builtin_return_address(0);

	if (!next_sched_yield) {
		errno = ENOSYS;
		return -1;
	}
	result = next_sched_yield();
	saved = errno;
	/* Retain the first transition, not later failures in the same spin loop. */
	if (!last_yield.valid || incoming != ENOSYS) {
		last_yield = (struct yield_context) {
			.valid = 1, .incoming = incoming, .result = result,
			.error = saved, .caller = caller,
		};
	}
	if (result < 0 &&
	    __atomic_load_n(&yield_records, __ATOMIC_RELAXED) < 16 &&
	    __atomic_fetch_add(&yield_records, 1, __ATOMIC_RELAXED) < 16) {
		n = snprintf(line, sizeof(line),
			     "IPC_YIELD_FAILURE pid=%ld incoming=%d result=%d error=%d caller=%p recv_valid=%d recv_fd=%d recv_errno=%d\n",
			     (long)getpid(), incoming, result, saved, caller,
			     last_receive.valid, last_receive.fd,
			     last_receive.error);
		if (n > 0 && (size_t)n < sizeof(line))
			(void)write(2, line, n);
	}
	errno = saved;
	return result;
}

static void report_context(int errnum, int incoming,
			   const struct receive_context *last)
{
	char line[512];
	int n;

	if (errnum != ENOSYS ||
	    __atomic_load_n(&records, __ATOMIC_RELAXED) >= 16 ||
	    __atomic_fetch_add(&records, 1, __ATOMIC_RELAXED) >= 16)
		return;
	n = snprintf(line, sizeof(line),
		     "IPC_ERRNO_CONTEXT pid=%ld error=%d incoming=%d valid=%d fd=%d result=%ld recv_errno=%d flags=%#x output=%#x caller=%p yield_valid=%d yield_incoming=%d yield_result=%d yield_errno=%d yield_caller=%p\n",
		     (long)getpid(), errnum, incoming, last->valid, last->fd,
		     (long)last->result, last->error, last->flags,
		     last->output, last->caller, last_yield.valid,
		     last_yield.incoming, last_yield.result, last_yield.error,
		     last_yield.caller);
	if (n > 0 && (size_t)n < sizeof(line))
		(void)write(2, line, n);
}

int strerror_r(int errnum, char *buf, size_t length)
{
	struct receive_context last = last_receive;
	int incoming = errno, result, saved;

	if (!next_strerror_r)
		return ENOSYS;
	result = next_strerror_r(errnum, buf, length);
	saved = errno;
	report_context(errnum, incoming, &last);
	errno = saved;
	return result;
}

const char *g_strerror(int errnum)
{
	struct receive_context last = last_receive;
	int incoming = errno, saved;
	const char *result;

	/* GLib is optional for host tests; do not synthesize a successful string. */
	if (!next_g_strerror) {
		errno = ENOSYS;
		return NULL;
	}
	result = next_g_strerror(errnum);
	saved = errno;
	report_context(errnum, incoming, &last);
	errno = saved;
	return result;
}
