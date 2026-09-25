// SPDX-License-Identifier: GPL-2.0-only
#include "ipc_errno_context.c"
#include <assert.h>

static int fail_yield(void)
{
	errno = ENOSYS;
	return -1;
}

static int succeed_yield(void)
{
	return 0;
}

int main(void)
{
	struct msghdr msg = { 0 };
	char text[128];

	next_sched_yield = succeed_yield;
	errno = EDOM;
	assert(sched_yield() == 0 && errno == EDOM);
	assert(yield_records == 0);
	assert(recvmsg(-1, &msg, 0) == -1 && errno == EBADF);
	assert(!last_yield.valid);
	next_sched_yield = fail_yield;
	errno = EAGAIN;
	for (int i = 0; i < 24; i++)
		assert(sched_yield() == -1 && errno == ENOSYS);
	assert(yield_records == 16);
	assert(last_yield.valid && last_yield.incoming == EAGAIN);
	assert(last_yield.result == -1 && last_yield.error == ENOSYS);
	assert(strerror_r(ENOSYS, text, sizeof(text)) == 0);
	assert(errno == ENOSYS);
	assert(recvmsg(-1, &msg, 0) == -1 && errno == EBADF);
	assert(!last_yield.valid);
	return 0;
}
