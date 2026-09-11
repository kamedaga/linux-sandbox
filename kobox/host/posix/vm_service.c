// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "vm_service.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

enum command {
	CREATE, CLONE, CLOSE, MAP, RESET, ENABLE_SYSCALLS, START, SNAPSHOT, RESTORE, WRITE_FP,
	RESUME, SYSCALL_RETURN, EVENT,
	PROBE, SYSCALL_PROBE, FAULT_CHECKPOINT, QUIESCENT, STOP,
};

struct request {
	struct request *next;
	struct kobox_posix_vm_remote *remote;
	struct kobox_posix_vm_remote *child;
	enum command command;
	const char *client;
	struct kobox_posix_memory_backing *ram;
	struct kobox_posix_vm_completion event;
	struct kobox_x86_user_regs registers;
	struct kobox_x86_fp_state fp;
	uint64_t address, value, sequence, syscall_sequence;
	uint64_t arguments[6];
	size_t offset, size;
	unsigned int protection, write;
	pid_t pid;
	bool done, share_mm;
	int result;
	int (*checkpoint)(void *);
	void *checkpoint_context;
};

struct kobox_posix_vm_remote {
	struct kobox_posix_vm_remote *next;
	struct kobox_posix_vm_service *service;
	struct kobox_posix_vm native;
	struct kobox_posix_vm_completion event;
	uint64_t sequence;
	bool pending;
	bool closed;
	bool terminal_delivered;
};

struct kobox_posix_vm_service {
	pthread_t thread, creator;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	struct request *head, **tail;
	struct kobox_posix_vm_remote *remotes;
	void (*notify)(void *context);
	void *context;
	sigset_t previous_signals;
	int commands, children;
	bool stopping, trace;
	uint64_t fault_address;
	int (*fault_checkpoint)(void *);
	void *fault_context;
};

static int execute(struct kobox_posix_vm_service *service, struct request *request)
{
	struct kobox_posix_vm_remote *remote = request->remote;
	unsigned int state;
	int result;

	if (request->command != CREATE && request->command != STOP &&
	    request->command != QUIESCENT && request->command != FAULT_CHECKPOINT &&
	    (!remote || remote->service != service))
		return EINVAL;
	switch (request->command) {
	case CREATE:
		remote = calloc(1, sizeof(*remote));
		if (!remote)
			return ENOMEM;
		result = kobox_posix_vm_create(&remote->native, request->client, request->ram,
					     request->address, request->size);
		if (result) {
			/* Failed create already drains its child and control backing. */
			if (remote->native.pid || remote->native.control ||
			    remote->native.control_backing.initialized)
				__builtin_trap();
			free(remote);
			return result;
		}
		remote->service = service;
		remote->next = service->remotes;
		service->remotes = remote;
		request->remote = remote;
		request->pid = remote->native.pid;
		return 0;
	case CLOSE:
		result = kobox_posix_vm_destroy(&remote->native);
		if (!result) {
			remote->closed = true;
			remote->pending = false;
		}
		return result;
	case CLONE:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (request->sequence != remote->sequence)
			return ESTALE;
		if (remote->pending || remote->native.running)
			return EBUSY;
		request->child = calloc(1, sizeof(*request->child));
		if (!request->child)
			return ENOMEM;
		result = kobox_posix_vm_clone(&remote->native, request->syscall_sequence, request->share_mm,
					     &request->child->native);
		if (!result) {
			result = kobox_posix_vm_read_fpregs(&request->child->native, &request->fp);
			if (result && kobox_posix_vm_destroy(&request->child->native))
				__builtin_trap();
		}
		if (result) {
			if (request->child->native.pid || request->child->native.control ||
			    request->child->native.control_backing.initialized)
				__builtin_trap();
			free(request->child);
			request->child = NULL;
			return result;
		}
		request->child->service = service;
		request->child->next = service->remotes;
		service->remotes = request->child;
		request->pid = request->child->native.pid;
		return 0;
	case MAP:
		if (service->fault_checkpoint && request->address == service->fault_address) {
			int (*checkpoint)(void *) = service->fault_checkpoint;

			service->fault_checkpoint = NULL;
			if (remote->event.event.kind != KOBOX_POSIX_VM_FAULT ||
			    remote->event.event.address != request->address ||
			    !(remote->event.event.error & 4) || remote->native.running)
				return EPROTO;
			fprintf(stderr, "Generation fault: actual user fault reached native map, pid=%d\n",
				remote->native.pid);
			checkpoint(service->fault_context);
			return EIO;
		}
		return kobox_posix_vm_map(&remote->native, request->address,
			request->offset, request->size, request->protection);
	case RESET:
		result = kobox_posix_vm_reset(&remote->native, request->address, request->size);
		if (result == ESRCH && remote->native.pid) {
			/* Death can win before waitpid observes it. Reap this exact
			 * tracked child before acknowledging revocation; never turn
			 * an unobserved ESRCH into completed invalidation.
			 */
			result = kobox_posix_vm_destroy(&remote->native);
			return result;
		}
		/* ESRCH alone is not proof. Only wait_native's reaped state is. */
		return result == ESRCH && !remote->native.pid ? 0 : result;
	case ENABLE_SYSCALLS:
		return kobox_posix_vm_enable_syscalls(&remote->native);
	case START:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (remote->sequence || remote->pending)
			return EBUSY;
		return kobox_posix_vm_start(&remote->native, &request->registers, &request->fp);
	case WRITE_FP:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (request->sequence != remote->sequence)
			return ESTALE;
		if (remote->pending || remote->native.running)
			return EBUSY;
		return kobox_posix_vm_write_fpregs(&remote->native, &request->fp);
	case SNAPSHOT:
	case RESTORE:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (request->sequence != remote->sequence)
			return ESTALE;
		if (remote->pending)
			return EBUSY;
		if (request->command == SNAPSHOT)
			return kobox_posix_vm_snapshot(&remote->native,
				&request->registers, &request->fp);
		return kobox_posix_vm_restore(&remote->native,
			&request->registers, &request->fp);
	case SYSCALL_RETURN:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (request->sequence != remote->sequence)
			return ESTALE;
		if (remote->pending || remote->native.running)
			return EBUSY;
		return kobox_posix_vm_syscall_return(&remote->native,
			request->syscall_sequence, &request->registers);
	case RESUME:
	case PROBE:
	case SYSCALL_PROBE:
		if (remote->closed || !remote->native.pid)
			return ESRCH;
		if (request->sequence != remote->sequence)
			return ESTALE;
		if (remote->pending || remote->native.running)
			return EBUSY;
		if (remote->sequence == UINT64_MAX)
			return EOVERFLOW;
		if (request->command == PROBE || request->command == SYSCALL_PROBE) {
			state = atomic_load_explicit(&remote->native.control->event, memory_order_acquire);
			if (state != KOBOX_VM_CLIENT_READY && state != KOBOX_VM_CLIENT_DONE)
				return EBUSY;
			if (request->command == SYSCALL_PROBE && !remote->native.syscalls_enabled)
				return EPERM;
			remote->native.control->address = request->address;
			remote->native.control->write = request->write;
			remote->native.control->value = request->value;
			if (request->command == SYSCALL_PROBE) {
				remote->native.control->write = 4;
				remote->native.control->syscall_number = request->value;
				memcpy(remote->native.control->syscall_arguments,
				       request->arguments, sizeof(request->arguments));
			}
		}
		result = kobox_posix_vm_resume(&remote->native);
		if (!result)
			remote->sequence++;
		return result;
	case EVENT:
		if (!remote->pending)
			return remote->closed || remote->terminal_delivered ? ESRCH : EAGAIN;
		request->event = remote->event;
		remote->pending = false;
		return 0;
	case FAULT_CHECKPOINT:
		if (!request->checkpoint || request->address % 4096)
			return EINVAL;
		if (service->fault_checkpoint)
			return EBUSY;
		service->fault_address = request->address;
		service->fault_checkpoint = request->checkpoint;
		service->fault_context = request->checkpoint_context;
		return 0;
	case QUIESCENT:
	case STOP:
		for (remote = service->remotes; remote; remote = remote->next)
			if (!remote->closed || remote->native.pid || remote->native.control ||
			    remote->native.group || remote->native.control_backing.initialized)
				return EBUSY;
		return 0;
	}
	return EINVAL;
}

static bool collect_events(struct kobox_posix_vm_service *service)
{
	struct kobox_posix_vm_remote *remote;
	bool ready = false;
	int result;

	for (remote = service->remotes; remote; remote = remote->next) {
		if (!remote->closed && !remote->pending && !remote->native.pid &&
		    !remote->terminal_delivered) {
			remote->event = (struct kobox_posix_vm_completion) {
				.event = {.kind = KOBOX_POSIX_VM_EXIT,
					.exit_status = remote->native.exit_status},
				.sequence = remote->sequence,
			};
			remote->terminal_delivered = true;
			remote->pending = true;
			ready = true;
			continue;
		}
		/* A stopped tracee can also die asynchronously. */
		if (remote->closed || remote->pending ||
		    (!remote->native.pid && remote->terminal_delivered))
			continue;
		result = kobox_posix_vm_wait(&remote->native, true, &remote->event.event);
		if (result == EAGAIN)
			continue;
		if (service->trace)
			fprintf(stderr, "VM event pid=%d kind=%u syscall=%llu arg0=%llx arg1=%llx error=%d\n",
				remote->native.pid, remote->event.event.kind,
				(unsigned long long)remote->event.event.syscall.number,
				(unsigned long long)remote->event.event.syscall.arguments[0],
				(unsigned long long)remote->event.event.syscall.arguments[1], result);
		remote->event.sequence = remote->sequence;
		remote->event.value = remote->native.control ? remote->native.control->result : 0;
		remote->event.error = result;
		if (!result && remote->event.event.kind == KOBOX_POSIX_VM_EXIT)
			remote->terminal_delivered = true;
		remote->pending = true;
		ready = true;
	}
	return ready;
}

static void *service_main(void *argument)
{
	struct kobox_posix_vm_service *service = argument;
	struct pollfd descriptors[2] = {
		{.fd = service->commands, .events = POLLIN},
		{.fd = service->children, .events = POLLIN},
	};
	struct signalfd_siginfo child;
	struct request *request;
	uint64_t counter, mask;
	int result;

	if (kobox_posix_notifications_save(&mask))
		__builtin_trap();
	for (;;) {
		do {
			result = poll(descriptors, 2, -1);
		} while (result < 0 && errno == EINTR);
		if (result < 0)
			__builtin_trap();
		while (read(service->commands, &counter, sizeof(counter)) == sizeof(counter))
			;
		while (read(service->children, &child, sizeof(child)) == sizeof(child))
			;
		for (;;) {
			if (pthread_mutex_lock(&service->lock))
				__builtin_trap();
			request = service->head;
			if (request) {
				service->head = request->next;
				if (!service->head)
					service->tail = &service->head;
			}
			if (pthread_mutex_unlock(&service->lock))
				__builtin_trap();
			if (!request)
				break;
			result = execute(service, request);
			if (pthread_mutex_lock(&service->lock))
				__builtin_trap();
			request->result = result;
			if (!result && request->command == STOP)
				service->stopping = true;
			request->done = true;
			if (pthread_cond_broadcast(&service->changed) ||
			    pthread_mutex_unlock(&service->lock))
				__builtin_trap();
		}
		if (service->stopping)
			return NULL;
		if (collect_events(service))
			service->notify(service->context);
	}
}

static int submit(struct kobox_posix_vm_service *service, struct request *request)
{
	uint64_t one = 1, mask;
	int result;

	if (!service)
		return EINVAL;
	/* A guest IRQ may itself issue a leaf command. Never let it interrupt
	 * libc synchronization on this same native thread.
	 */
	if (kobox_posix_notifications_save(&mask) || pthread_mutex_lock(&service->lock))
		__builtin_trap();
	if (service->stopping) {
		result = ESRCH;
		goto unlock;
	}
	*service->tail = request;
	service->tail = &request->next;
	if (write(service->commands, &one, sizeof(one)) != sizeof(one))
		__builtin_trap();
	while (!request->done)
		if (pthread_cond_wait(&service->changed, &service->lock))
			__builtin_trap();
	result = request->result;
unlock:
	if (pthread_mutex_unlock(&service->lock) || kobox_posix_notifications_restore(mask))
		__builtin_trap();
	return result;
}

int kobox_posix_vm_service_create(struct kobox_posix_vm_service **out,
	void (*notify)(void *), void *context)
{
	struct kobox_posix_vm_service *service;
	sigset_t children;
	int result;

	if (!out || *out || !notify)
		return EINVAL;
	service = calloc(1, sizeof(*service));
	if (!service)
		return ENOMEM;
	service->commands = -1;
	service->children = -1;
	service->notify = notify;
	service->context = context;
	service->creator = pthread_self();
	service->trace = getenv("KOBOX_VM_TRACE") != NULL;
	service->tail = &service->head;
	result = pthread_mutex_init(&service->lock, NULL);
	if (result)
		goto free;
	result = pthread_cond_init(&service->changed, NULL);
	if (result)
		goto mutex;
	sigemptyset(&children);
	sigaddset(&children, SIGCHLD);
	result = pthread_sigmask(SIG_BLOCK, &children, &service->previous_signals);
	if (result)
		goto condition;
	service->commands = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	service->children = signalfd(-1, &children, SFD_CLOEXEC | SFD_NONBLOCK);
	if (service->commands < 0 || service->children < 0) {
		result = errno;
		goto descriptors;
	}
	result = pthread_create(&service->thread, NULL, service_main, service);
	if (result)
		goto descriptors;
	*out = service;
	return 0;
descriptors:
	if (service->commands >= 0)
		(void)close(service->commands);
	if (service->children >= 0)
		(void)close(service->children);
	if (pthread_sigmask(SIG_SETMASK, &service->previous_signals, NULL))
		__builtin_trap();
condition:
	(void)pthread_cond_destroy(&service->changed);
mutex:
	(void)pthread_mutex_destroy(&service->lock);
free:
	free(service);
	return result;
}

int kobox_posix_vm_service_destroy(struct kobox_posix_vm_service *service)
{
	struct request request = {.command = STOP};
	struct kobox_posix_vm_remote *remote;
	int result;

	if (!service || !pthread_equal(service->creator, pthread_self()))
		return EINVAL;
	result = submit(service, &request);
	if (result)
		return result;
	result = pthread_join(service->thread, NULL);
	if (result)
		return result;
	while ((remote = service->remotes)) {
		service->remotes = remote->next;
		free(remote);
	}
	if (close(service->commands) || close(service->children) ||
	    pthread_cond_destroy(&service->changed) || pthread_mutex_destroy(&service->lock) ||
	    pthread_sigmask(SIG_SETMASK, &service->previous_signals, NULL))
		__builtin_trap();
	free(service);
	return 0;
}

int kobox_posix_vm_service_quiescent(struct kobox_posix_vm_service *service)
{
	struct request request = {.command = QUIESCENT};

	return service ? submit(service, &request) : EINVAL;
}

int kobox_posix_vm_remote_create(struct kobox_posix_vm_service *service,
	const char *client, struct kobox_posix_memory_backing *ram,
	uint64_t window_start, size_t window_size,
	struct kobox_posix_vm_remote **out, pid_t *pid)
{
	struct request request = {.command = CREATE, .client = client, .ram = ram,
		.address = window_start, .size = window_size};
	int result;

	if (!out || *out || !pid)
		return EINVAL;
	result = submit(service, &request);
	if (!result) {
		*out = request.remote;
		*pid = request.pid;
	}
	return result;
}

int kobox_posix_vm_remote_clone(struct kobox_posix_vm_remote *parent,
	uint64_t sequence, uint64_t syscall_sequence, bool share_mm,
	struct kobox_posix_vm_remote **out, pid_t *pid, struct kobox_x86_fp_state *fp)
{
	struct request request = {.command = CLONE, .remote = parent,
		.sequence = sequence, .syscall_sequence = syscall_sequence, .share_mm = share_mm};
	int result;

	if (!parent || !out || *out || !pid || !fp)
		return EINVAL;
	result = submit(parent->service, &request);
	if (!result) {
		*out = request.child;
		*pid = request.pid;
		*fp = request.fp;
	}
	return result;
}

int kobox_posix_vm_remote_close(struct kobox_posix_vm_remote *remote)
{
	struct request request = {.command = CLOSE, .remote = remote};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_remote_map(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t offset, size_t size, unsigned int protection)
{
	struct request request = {.command = MAP, .remote = remote, .address = address,
		.offset = offset, .size = size, .protection = protection};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_service_fault_checkpoint(struct kobox_posix_vm_service *service,
	uint64_t address, int (*checkpoint)(void *), void *context)
{
	struct request request = {.command = FAULT_CHECKPOINT, .address = address,
		.checkpoint = checkpoint, .checkpoint_context = context};

	return submit(service, &request);
}

int kobox_posix_vm_remote_reset(struct kobox_posix_vm_remote *remote,
	uint64_t address, size_t size)
{
	struct request request = {.command = RESET, .remote = remote, .address = address, .size = size};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_remote_resume(struct kobox_posix_vm_remote *remote, uint64_t sequence)
{
	struct request request = {.command = RESUME, .remote = remote, .sequence = sequence};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_remote_enable_syscalls(struct kobox_posix_vm_remote *remote)
{
	struct request request = {.command = ENABLE_SYSCALLS, .remote = remote};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_remote_start(struct kobox_posix_vm_remote *remote,
	const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp)
{
	struct request request = {.command = START, .remote = remote};

	if (!remote || !registers || !fp)
		return EINVAL;
	request.registers = *registers;
	request.fp = *fp;
	return submit(remote->service, &request);
}

int kobox_posix_vm_remote_write_fpregs(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, const struct kobox_x86_fp_state *fp)
{
	struct request request = {.command = WRITE_FP, .remote = remote, .sequence = sequence};

	if (!remote || !fp)
		return EINVAL;
	request.fp = *fp;
	return submit(remote->service, &request);
}

int kobox_posix_vm_remote_snapshot(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp)
{
	struct request request = {.command = SNAPSHOT, .remote = remote, .sequence = sequence};
	int result;

	if (!remote || !registers || !fp)
		return EINVAL;
	result = submit(remote->service, &request);
	if (!result) {
		*registers = request.registers;
		*fp = request.fp;
	}
	return result;
}

int kobox_posix_vm_remote_restore(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, const struct kobox_x86_user_regs *registers,
	const struct kobox_x86_fp_state *fp)
{
	struct request request = {.command = RESTORE, .remote = remote, .sequence = sequence};

	if (!remote || !registers || !fp)
		return EINVAL;
	request.registers = *registers;
	request.fp = *fp;
	return submit(remote->service, &request);
}

int kobox_posix_vm_remote_syscall_return(struct kobox_posix_vm_remote *remote,
	uint64_t sequence, uint64_t syscall_sequence,
	const struct kobox_x86_user_regs *registers)
{
	struct request request = {.command = SYSCALL_RETURN, .remote = remote,
		.sequence = sequence, .syscall_sequence = syscall_sequence};

	if (!remote || !registers)
		return EINVAL;
	request.registers = *registers;
	return submit(remote->service, &request);
}

int kobox_posix_vm_remote_event(struct kobox_posix_vm_remote *remote,
	struct kobox_posix_vm_completion *event)
{
	struct request request = {.command = EVENT, .remote = remote};
	int result;

	if (!remote || !event)
		return EINVAL;
	result = submit(remote->service, &request);
	if (!result)
		*event = request.event;
	return result;
}

int kobox_posix_vm_remote_probe(struct kobox_posix_vm_remote *remote,
	uint64_t address, unsigned int write, uint64_t value, uint64_t sequence)
{
	struct request request = {.command = PROBE, .remote = remote, .address = address,
		.write = write, .value = value, .sequence = sequence};

	return remote ? submit(remote->service, &request) : EINVAL;
}

int kobox_posix_vm_remote_syscall_probe(struct kobox_posix_vm_remote *remote,
	uint64_t number, const uint64_t arguments[6], uint64_t sequence)
{
	struct request request = {.command = SYSCALL_PROBE, .remote = remote,
		.sequence = sequence, .value = number};

	if (!remote || !arguments)
		return EINVAL;
	memcpy(request.arguments, arguments, sizeof(request.arguments));
	return submit(remote->service, &request);
}
