// SPDX-License-Identifier: GPL-2.0-only
#include "fs_bench.h"

#ifdef KOBOX_FS_INTERNAL_BENCH
#include "fs_service.h"
#include "fs_worker.h"
#include "fs_executor.h"
#include "../task/diagnostic.h"
#include "../arch/x86_64/host_call.h"

#include <kobox2/filesystem.h>
#include <linux/cred.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stat.h>
#include <linux/task_work.h>

#ifndef KOBOX_FS_HANDOFF_BENCH_ONLY
#ifdef KOBOX_FS_PUBLICATION_BENCH
struct fs_bench_publication {
	u8 input[KB2_FILESYSTEM_REQUEST_SIZE + 4096];
	size_t input_size;
	size_t capacity;
	bool snapshotted, released;
	bool poison;
	u8 response[KB2_FILESYSTEM_RESPONSE_SIZE + 4096];
	u64 start, visible, copy_ticks;
	size_t used;
	int error;
	bool published;
};
#ifdef KOBOX_FS_EXECUTOR_BENCH
static void bench_snapshot_done(void *cookie);

struct fs_bench_source {
	struct fs_bench_publication *items[4];
	unsigned int head, tail;
	bool fault;
};
#endif
#endif
struct fs_bench {
	struct kobox_linux_fs_service *service;
	struct kobox_linux_fs_workers *workers;
	u8 *input, *response;
	u64 handles[2], sequence;
	const char *path;
	size_t input_size;
	u32 opcode;
#ifdef KOBOX_FS_PUBLICATION_BENCH
	struct kobox_linux_fs_workers *owner_workers, *direct_workers;
	struct fs_bench_publication *tickets;
	u64 publication_ticks, copy_ticks, retirement_ticks, worker_publications;
	bool direct;
#ifdef KOBOX_FS_EXECUTOR_BENCH
	struct kobox_linux_fs_executors *executors;
	struct kobox_linux_fs_lane *lanes[2];
	struct fs_bench_source sources[2];
#endif
#endif
};
#endif

static u64 bench_ticks(void)
{
	return kobox_fs_bench_ticks();
}

#ifdef KOBOX_FS_HANDOFF_BENCH_ONLY
struct handoff_pair {
	struct completion to_peer, to_source, done;
	struct task_struct *peer;
	unsigned int cpu;
	bool stop;
};

static u64 handoff_switches(struct task_struct *task)
{
	return READ_ONCE(task->nvcsw) + READ_ONCE(task->nivcsw);
}

static int handoff_peer(void *argument)
{
	struct handoff_pair *pair = argument;

	for (;;) {
		wait_for_completion(&pair->to_peer);
		if (READ_ONCE(pair->stop))
			return 0;
		complete(&pair->to_source);
	}
}

static int handoff_source(void *argument)
{
	struct handoff_pair *pair = argument;
	unsigned int trial, i;

	for (trial = 1; trial <= 3; trial++) {
		u64 start, elapsed, ticks, switches, rounds = 0;

		for (i = 0; i < 32; i++) {
			complete(&pair->to_peer);
			wait_for_completion(&pair->to_source);
		}
		switches = handoff_switches(current) + handoff_switches(pair->peer);
		start = ktime_get_ns();
		ticks = bench_ticks();
		do {
			for (i = 0; i < 128; i++) {
				complete(&pair->to_peer);
				wait_for_completion(&pair->to_source);
				rounds++;
			}
			cond_resched();
			elapsed = ktime_get_ns() - start;
		} while (elapsed < NSEC_PER_SEC);
		ticks = bench_ticks() - ticks;
		switches = handoff_switches(current) + handoff_switches(pair->peer) - switches;
		/* One round is two task transfers plus completion/wait bookkeeping.
		 * No FS, admission, payload copy, per-op counter or timestamp is used.
		 */
		pr_info("FS_INTERNAL_HANDOFF cpu=%u trial=%u rounds=%llu ticks=%llu elapsed_ns=%llu switches=%llu\n",
			pair->cpu, trial, rounds, ticks, elapsed, switches);
	}
	complete(&pair->done);
	return 0;
}

static int bench_handoff(void)
{
	struct handoff_pair pair = { .cpu = cpumask_first(cpu_online_mask) };
	struct task_struct *source;
	int error;

	init_completion(&pair.to_peer);
	init_completion(&pair.to_source);
	init_completion(&pair.done);
	pair.peer = kthread_create(handoff_peer, &pair, "bench-handoff-peer");
	if (IS_ERR(pair.peer))
		return PTR_ERR(pair.peer);
	/* The source may finish before its creator resumes. Pin both actual
	 * task objects through kthread_stop before retiring their shared stack.
	 */
	get_task_struct(pair.peer);
	kthread_bind(pair.peer, pair.cpu);
	source = kthread_create(handoff_source, &pair, "bench-handoff-source");
	if (IS_ERR(source)) {
		error = PTR_ERR(source);
		WRITE_ONCE(pair.stop, true);
		complete(&pair.to_peer);
		kthread_stop(pair.peer);
		put_task_struct(pair.peer);
		return error;
	}
	get_task_struct(source);
	kthread_bind(source, pair.cpu);
	wake_up_process(pair.peer);
	wake_up_process(source);
	wait_for_completion(&pair.done);
	error = kthread_stop(source);
	put_task_struct(source);
	WRITE_ONCE(pair.stop, true);
	complete(&pair.to_peer);
	if (kthread_stop(pair.peer) && !error)
		error = -EINVAL;
	put_task_struct(pair.peer);
	return error;
}
#endif

#ifndef KOBOX_FS_HANDOFF_BENCH_ONLY
#ifndef KOBOX_FS_PUBLICATION_BENCH
static noinline int bench_native_noop(void)
{
	/* Keep an actual ABI call inside the full FP boundary, without services. */
	asm volatile("" ::: "memory");
	return 0;
}

static int bench_guards(void)
{
	static const char * const names[] = { "preempt", "irq", "clock", "fp" };
	unsigned int trial, kind, i;

	/* Measure primitive boundary cost independently of VFS and pool scheduling.
	 * Restore every iteration; do not turn this into a long IRQ-off section.
	 */
	if (irqs_disabled())
		return -EINVAL;
	for (trial = 1; trial <= 3; trial++)
		for (kind = 0; kind < ARRAY_SIZE(names); kind++) {
			u64 start, ticks, runtime, elapsed, ops = 0;

			runtime = task_sched_runtime(current);
			start = ktime_get_ns();
			ticks = bench_ticks();
			do {
				for (i = 0; i < 256; i++) {
					unsigned long flags;

					if (kind == 3) {
						kobox_host_call(bench_native_noop());
					} else if (kind == 2) {
						sched_clock();
					} else if (kind == 1) {
						flags = kobox_provider_irq_save();
						kobox_provider_irq_restore(flags);
					} else {
						flags = kobox_provider_preempt_save();
						kobox_provider_preempt_restore(flags);
					}
					ops++;
				}
				cond_resched();
				elapsed = ktime_get_ns() - start;
			} while (elapsed < NSEC_PER_SEC);
			ticks = bench_ticks() - ticks;
			runtime = task_sched_runtime(current) - runtime;
			pr_info("FS_INTERNAL_GUARD kind=%s trial=%u ops=%llu ticks=%llu elapsed_ns=%llu task_ns=%llu\n",
				names[kind], trial, ops, ticks, elapsed, runtime);
		}
	return 0;
}
#endif

static int bench_encode(struct fs_bench *bench, unsigned int client)
{
	kb2_fs_request_t r = {
		.opcode = bench->opcode, .generation = 1,
		.sequence = ++bench->sequence, .handle = bench->handles[client],
		.length = 4096, .mask = STATX_BASIC_STATS,
	};

	bench->input_size = KB2_FILESYSTEM_REQUEST_SIZE;
	if (r.opcode == KB2_FILESYSTEM_OP_STATX) {
		r.handle = 0;
		r.path = (kb2_fs_span_t) {
			KB2_FILESYSTEM_REQUEST_SIZE, strlen(bench->path) + 1,
		};
		memcpy(bench->input + r.path.offset, bench->path, r.path.length);
		bench->input_size += r.path.length;
	} else if (r.opcode == KB2_FILESYSTEM_OP_PWRITE) {
		r.data = (kb2_fs_span_t) { KB2_FILESYSTEM_REQUEST_SIZE, 4096 };
		memset(bench->input + r.data.offset, 0x5a, 4096);
		bench->input_size += 4096;
	}
	return kb2_fs_request_encode(bench->input, bench->input_size,
		bench->input_size, &r);
}

static int bench_check(struct fs_bench *bench, const void *bytes, size_t used)
{
	kb2_fs_response_t r;

	if (kb2_fs_response_decode(bytes, used, &r) || r.opcode != bench->opcode ||
	    r.result != (bench->opcode == KB2_FILESYSTEM_OP_STATX ? 0 : 4096))
		return -EINVAL;
	if (bench->opcode == KB2_FILESYSTEM_OP_PREAD &&
	    (r.data_length != 4096 || used < KB2_FILESYSTEM_RESPONSE_SIZE + 4096 ||
	     memchr_inv(bytes + KB2_FILESYSTEM_RESPONSE_SIZE, 0x5a, 4096)))
		return -EIO;
	return 0;
}

#ifdef KOBOX_FS_PUBLICATION_BENCH
static void bench_publish(void *cookie, const void *response, size_t used, int error)
{
	struct fs_bench_publication *ticket = cookie;
	u64 copy_start = bench_ticks();

	/* A host-compatible private sink: no Linux calls, native SEND, ring or
	 * consumer task. Both variants copy the same bytes behind the full FP
	 * boundary. The timestamp denotes publication, not peer observation. */
	if (used > sizeof(ticket->response))
		error = -EOVERFLOW;
	if (!error)
		memcpy(ticket->response, response, used);
	ticket->used = used;
	ticket->error = error;
	ticket->visible = bench_ticks();
	ticket->copy_ticks = ticket->visible - copy_start;
	smp_store_release(&ticket->published, true);
}
#endif

#ifdef KOBOX_FS_EXECUTOR_BENCH
/* Single producer, serialized intake consumer per lane. Release/acquire on
 * tail publishes immutable ticket bytes; wait_idle pins all tickets through
 * callback return and private budget refund, not merely response visibility. */
static int bench_take(void *binding, struct kobox_linux_fs_intake *item)
{
	struct fs_bench_source *source = binding;
	unsigned int tail = smp_load_acquire(&source->tail), head = source->head;
	struct fs_bench_publication *ticket;

	if (source->fault)
		return -EIO;
	if (head == tail)
		return 0;
	ticket = source->items[head % ARRAY_SIZE(source->items)];
	source->head = head + 1;
	*item = (struct kobox_linux_fs_intake) {
		.input = ticket->input, .input_size = ticket->input_size,
		.response_capacity = ticket->capacity, .cookie = ticket,
		.snapshot_done = ticket->poison ? bench_snapshot_done : NULL,
	};
	return head + 1 == tail ? 1 : 2;
}

static void bench_snapshot_done(void *cookie)
{
	struct fs_bench_publication *ticket = cookie;

	/* Functional-only poisoning must not add a one-sided 4 KiB memset to
	 * the timed comparison. Execution must use the private snapshot. */
	if (ticket->poison)
		memset(ticket->input, 0xa5, ticket->input_size);
	ticket->snapshotted = true;
}

static void bench_release(void *cookie)
{
	struct fs_bench_publication *ticket = cookie;

	ticket->released = true;
}

static int bench_executor_batch(struct fs_bench *bench, unsigned int depth)
{
	unsigned int index, client, tail;
	int error;

	for (index = 0; index < depth; index++) {
		struct fs_bench_publication *ticket = &bench->tickets[index];
		struct fs_bench_source *source;

		client = index % 2;
		source = &bench->sources[client];
		{
			u8 *input = bench->input;

			bench->input = ticket->input;
			error = bench_encode(bench, client);
			bench->input = input;
		}
		if (error)
			return error;
		ticket->input_size = bench->input_size;
		ticket->capacity = sizeof(ticket->response);
		ticket->published = ticket->snapshotted = ticket->released = false;
		ticket->start = bench_ticks();
		tail = source->tail;
		if (tail - READ_ONCE(source->head) >= ARRAY_SIZE(source->items))
			return -EOVERFLOW;
		source->items[tail % ARRAY_SIZE(source->items)] = ticket;
		smp_store_release(&source->tail, tail + 1);
	}
	for (client = 0; client < min(depth, 2U); client++) {
		error = kobox_linux_fs_executors_kick(bench->lanes[client]);
		if (error)
			return error;
	}
	error = kobox_linux_fs_executors_wait_idle(bench->executors);
	if (error)
		return error;
	for (index = 0; index < depth; index++) {
		struct fs_bench_publication *ticket = &bench->tickets[index];

		if (!ticket->published || !ticket->released ||
		    (ticket->poison && !ticket->snapshotted))
			return -EIO;
		error = ticket->error ?: bench_check(bench, ticket->response, ticket->used);
		if (error)
			return error;
		bench->publication_ticks += ticket->visible - ticket->start;
		bench->copy_ticks += ticket->copy_ticks;
		bench->retirement_ticks += bench_ticks() - ticket->visible;
		bench->worker_publications++;
	}
	return 0;
}

static int bench_executor_error(struct fs_bench *bench, size_t input_size,
	size_t capacity, int transport, s64 result)
{
	struct fs_bench_publication *ticket = &bench->tickets[0];
	struct fs_bench_source *source = &bench->sources[0];
	kb2_fs_response_t reply;
	unsigned int tail = source->tail;
	int error;

	ticket->input_size = input_size;
	ticket->capacity = capacity;
	ticket->poison = false;
	ticket->published = ticket->snapshotted = ticket->released = false;
	ticket->start = bench_ticks();
	source->items[tail % ARRAY_SIZE(source->items)] = ticket;
	smp_store_release(&source->tail, tail + 1);
	error = kobox_linux_fs_executors_kick(bench->lanes[0]);
	if (!error)
		error = kobox_linux_fs_executors_wait_idle(bench->executors);
	if (error)
		return error;
	if (!ticket->published || !ticket->released ||
	    ticket->error != transport)
		return -EINVAL;
	if (!transport && (kb2_fs_response_decode(ticket->response, ticket->used, &reply) ||
			   reply.result != result))
		return -EINVAL;
	return 0;
}

static int bench_executor_checks(struct fs_bench *bench, unsigned int fs)
{
	static const u32 operations[] = { KB2_FILESYSTEM_OP_STATX,
		KB2_FILESYSTEM_OP_PREAD, KB2_FILESYSTEM_OP_PWRITE };
	struct fs_bench_publication *ticket = &bench->tickets[0];
	struct kobox_linux_fs_lane *duplicate;
	kb2_fs_request_t request;
	u8 *input = bench->input;
	int error;

	if (kobox_linux_fs_executors_destroy(bench->executors) != -EBUSY ||
	    kobox_linux_fs_executors_unregister(bench->lanes[0]) != -EBUSY ||
	    kobox_linux_fs_executors_register(bench->executors, 1, &bench->sources[0],
		&duplicate) != -EEXIST)
		return -EINVAL;
	bench->direct = true;
	for (unsigned int i = 0; i < 8; i++)
		bench->tickets[i].poison = true;
	for (unsigned int i = 0; i < ARRAY_SIZE(operations); i++) {
		bench->opcode = operations[i];
		error = bench_executor_batch(bench, 8);
		if (error)
			return error;
	}
	for (unsigned int i = 0; i < 8; i++)
		bench->tickets[i].poison = false;
	bench->input = ticket->input;
	bench->opcode = KB2_FILESYSTEM_OP_STATX;
	error = bench_encode(bench, 0);
	bench->input = input;
	if (error)
		return error;
	error = kobox_linux_fs_executors_pause(bench->lanes[0]);
	if (error || kobox_linux_fs_executors_kick(bench->lanes[0]) != -ESHUTDOWN)
		return error ?: -EINVAL;
	error = kobox_linux_fs_service_credentials(bench->service, 1, 2 + 2 * fs,
		current_cred(), 0);
	if (!error)
		error = kobox_linux_fs_executors_resume(bench->lanes[0]);
	if (!error)
		error = bench_executor_error(bench, bench->input_size, sizeof(ticket->response), 0, -EACCES);
	if (error)
		return error;
	error = kobox_linux_fs_executors_pause(bench->lanes[0]);
	if (!error)
		error = kobox_linux_fs_service_credentials(bench->service, 1, 3 + 2 * fs,
			current_cred(), KB2_FILESYSTEM_RIGHTS_VALID_MASK);
	if (!error)
		error = kobox_linux_fs_executors_resume(bench->lanes[0]);
	if (error)
		return error;
	/* A capacity larger than the usual transport allocation remains legal. */
	error = bench_executor_error(bench, bench->input_size, 900 * 1024, 0, 0);
	if (!error)
		error = bench_executor_error(bench, bench->input_size, 2 << 20, -ENOMEM, 0);
	if (!error)
		error = bench_executor_error(bench, SIZE_MAX, sizeof(ticket->response), -EOVERFLOW, 0);
	if (!error)
		error = bench_executor_error(bench, 8, sizeof(ticket->response), -EINVAL, 0);
	if (error)
		return error;
	/* Shared identity claims cannot select the other binding's authority. */
	if (kb2_fs_request_decode(ticket->input, bench->input_size, bench->input_size, &request))
		return -EINVAL;
	request.client = 2;
	error = kb2_fs_request_encode(ticket->input, bench->input_size, bench->input_size, &request);
	if (!error)
		error = bench_executor_error(bench, bench->input_size, sizeof(ticket->response), -EACCES, 0);
	if (error)
		return error;
	bench->input = ticket->input;
	bench->opcode = KB2_FILESYSTEM_OP_PREAD;
	error = bench_encode(bench, 0);
	bench->input = input;
	if (!error)
		error = kb2_fs_request_decode(ticket->input, bench->input_size, bench->input_size, &request);
	request.offset = 4096;
	if (!error)
		error = kb2_fs_request_encode(ticket->input, bench->input_size, bench->input_size, &request);
	if (!error)
		error = bench_executor_error(bench, bench->input_size, sizeof(ticket->response), 0, 0);
	if (!error && ticket->used != KB2_FILESYSTEM_RESPONSE_SIZE)
		error = -EIO;
	if (error)
		return error;
	bench->opcode = KB2_FILESYSTEM_OP_PREAD;
	error = bench_executor_batch(bench, 8);
	if (!error)
		error = kobox_linux_fs_executors_check_drained(bench->executors);
	if (!error) {
		struct fs_bench_source failed = { .fault = true };
		struct kobox_linux_fs_lane *lane;

		error = kobox_linux_fs_executors_register(bench->executors, 3 + fs, &failed, &lane);
		if (!error) {
			if (kobox_linux_fs_executors_kick(lane) ||
			    kobox_linux_fs_executors_wait_idle(bench->executors) != -EIO ||
			    kobox_linux_fs_executors_pause(lane) != -EIO)
				error = -EINVAL;
			/* Even a failed assertion must drain before its stack binding
			 * goes away; a source fault is not an acknowledged success. */
			kobox_linux_fs_executors_pause(lane);
			kobox_linux_fs_executors_unregister(lane);
		}
	}
	bench->direct = false;
	pr_info("FS_EXECUTOR_CHECKS fs=%s status=%d\n", fs ? "tmpfs" : "ext4", error);
	return error;
}
#endif

static int bench_batch(struct fs_bench *bench, unsigned int depth)
{
	unsigned int submitted = 0, completed = 0;
	int error;

#ifdef KOBOX_FS_EXECUTOR_BENCH
	if (bench->direct && depth)
		return bench_executor_batch(bench, depth);
#endif

	if (!depth) {
		size_t used;

		error = bench_encode(bench, 0);
		if (!error)
			error = kobox_linux_fs_service_dispatch(bench->service, 1,
				bench->input, bench->input_size, bench->response,
				KB2_FILESYSTEM_RESPONSE_SIZE + 4096, &used);
		return error ?: bench_check(bench, bench->response, used);
	}
	while (completed < depth) {
		struct kobox_linux_fs_work *work;
		const void *response;
		void *cookie;
		size_t used;
		int operation;

		if (submitted < depth) {
			unsigned int client = submitted % 2;
			void *submit_cookie = (void *)(unsigned long)(submitted + 1);

			error = bench_encode(bench, client);
#ifdef KOBOX_FS_PUBLICATION_BENCH
			submit_cookie = &bench->tickets[submitted];
			WRITE_ONCE(bench->tickets[submitted].published, false);
			bench->tickets[submitted].start = bench_ticks();
#endif
			if (!error)
				error = kobox_linux_fs_workers_submit(bench->workers,
					client + 1, bench->input, bench->input_size,
					KB2_FILESYSTEM_RESPONSE_SIZE + 4096,
					submit_cookie);
			if (!error)
				submitted++;
			else if (error != -EAGAIN)
				return error;
		}
		/* Arm before inspecting completion, like the production owner.
		 * No timer sleep or polling interval may inflate handoff latency.
		 */
		set_current_state(TASK_UNINTERRUPTIBLE);
		error = kobox_linux_fs_workers_collect(bench->workers, &work,
			&cookie, &response, &used, &operation);
		if (error == -EAGAIN) {
			if (submitted == depth)
				schedule();
			__set_current_state(TASK_RUNNING);
			continue;
		}
		__set_current_state(TASK_RUNNING);
		if (error)
			return error;
#ifdef KOBOX_FS_PUBLICATION_BENCH
		{
			struct fs_bench_publication *ticket = cookie;
			bool from_worker = smp_load_acquire(&ticket->published);

			if (!from_worker)
				kobox_host_call((bench_publish(ticket, response, used, operation), 0));
			error = ticket->error ?: bench_check(bench, ticket->response, ticket->used);
			operation = kobox_linux_fs_workers_release(bench->workers, work);
			bench->publication_ticks += ticket->visible - ticket->start;
			bench->copy_ticks += ticket->copy_ticks;
			bench->retirement_ticks += bench_ticks() - ticket->visible;
			bench->worker_publications += from_worker;
		}
#else
		error = operation ?: bench_check(bench, response, used);
		operation = kobox_linux_fs_workers_release(bench->workers, work);
#endif
		if (error || operation)
			return error ?: operation;
		completed++;
	}
	return 0;
}

static int bench_phase(struct fs_bench *bench, const char *fs, u32 opcode,
	unsigned int depth, unsigned int trial)
{
#ifdef KOBOX_FS_INTERNAL_PROFILE
	struct kobox_task_bench_counts before, after;
#endif
	u64 start, ticks, runtime, switches, elapsed, ops = 0;
#ifdef KOBOX_FS_PUBLICATION_BENCH
	u64 wakes, batches, wakes_after, batches_after;
#endif
	unsigned int i;
	int error;

	bench->opcode = opcode;
	for (i = 0; i < 32; i++) {
		error = bench_batch(bench, depth);
		if (error)
			return error;
	}
#ifdef KOBOX_FS_PUBLICATION_BENCH
	bench->publication_ticks = bench->copy_ticks = bench->retirement_ticks = 0;
	bench->worker_publications = 0;
#ifdef KOBOX_FS_EXECUTOR_BENCH
	if (bench->direct)
		kobox_linux_fs_executors_counts(bench->executors, &batches, &batches_after, &wakes);
	else
#endif
	kobox_linux_fs_workers_completion_counts(bench->workers, &wakes, &batches);
#endif
#ifdef KOBOX_FS_INTERNAL_PROFILE
	kobox_linux_fs_profile_reset(bench->service);
	kobox_linux_fs_workers_profile_reset(bench->workers);
	pr_info("FS_NATIVE_BEGIN\n");
#endif
	/* Read upstream task counters outside the timed loop; no hot-path
	 * profiling atomics or a different wake policy should bias this count.
	 */
	switches =
#ifdef KOBOX_FS_EXECUTOR_BENCH
		bench->direct && depth ? kobox_linux_fs_executors_switches(bench->executors) :
#endif
		depth ? kobox_linux_fs_workers_switches(bench->workers) :
		READ_ONCE(current->nvcsw) + READ_ONCE(current->nivcsw);
	runtime = depth ? kobox_linux_fs_workers_runtime(bench->workers) :
		task_sched_runtime(current);
	/* Total-time mode must not retain task counters just to print zeros. */
#ifdef KOBOX_FS_INTERNAL_PROFILE
	if (depth)
		kobox_linux_fs_workers_boundaries(bench->workers, &before);
	else
		kobox_task_bench_read(current, &before);
#endif
#ifdef KOBOX_FS_BOUNDARY_CALLERS
	/* Call sites for one trial of the same-task and single-worker rows. */
	if (trial == 1 && depth <= 1) {
		kobox_task_bench_sites_reset();
		if (depth)
			kobox_linux_fs_workers_trace(bench->workers, true);
		else
			kobox_task_bench_trace(current, true);
	}
#endif
	start = ktime_get_ns();
	ticks = bench_ticks();
	do {
		for (i = 0; i < 32; i++) {
			error = bench_batch(bench, depth);
			if (error)
				return error;
			ops += depth ?: 1;
		}
		cond_resched();
		elapsed = ktime_get_ns() - start;
	} while (elapsed < NSEC_PER_SEC);
	ticks = bench_ticks() - ticks;
#ifdef KOBOX_FS_BOUNDARY_CALLERS
	if (trial == 1 && depth <= 1) {
		char label[32];

		if (depth)
			kobox_linux_fs_workers_trace(bench->workers, false);
		else
			kobox_task_bench_trace(current, false);
		snprintf(label, sizeof(label), "%s-%u-%u", fs, opcode, depth);
		kobox_task_bench_sites_report(label, ops);
	}
#endif
#ifdef KOBOX_FS_INTERNAL_PROFILE
	if (depth)
		kobox_linux_fs_workers_boundaries(bench->workers, &after);
	else
		kobox_task_bench_read(current, &after);
#endif
	runtime = (depth ? kobox_linux_fs_workers_runtime(bench->workers) :
		task_sched_runtime(current)) - runtime;
	switches = (
#ifdef KOBOX_FS_EXECUTOR_BENCH
		bench->direct && depth ? kobox_linux_fs_executors_switches(bench->executors) :
#endif
		depth ? kobox_linux_fs_workers_switches(bench->workers) :
		READ_ONCE(current->nvcsw) + READ_ONCE(current->nivcsw)) - switches;
#ifdef KOBOX_FS_INTERNAL_PROFILE
	pr_info("FS_NATIVE_END\n");
#endif
#ifdef KOBOX_FS_PUBLICATION_BENCH
#ifdef KOBOX_FS_EXECUTOR_BENCH
	if (bench->direct) {
		u64 taken, released;

		kobox_linux_fs_executors_counts(bench->executors, &taken, &released, &wakes_after);
		if (taken - batches != ops || released - batches_after != ops)
			return -EIO;
		batches = batches_after = 0;
	} else
#endif
	kobox_linux_fs_workers_completion_counts(bench->workers, &wakes_after, &batches_after);
	pr_info("FS_INTERNAL_PUBLICATION fs=%s opcode=%u depth=%u trial=%u mode=%s ops=%llu ticks=%llu elapsed_ns=%llu publication_ticks=%llu copy_ticks=%llu retirement_ticks=%llu worker_publications=%llu switches=%llu\n",
		fs, opcode, depth, trial,
#ifdef KOBOX_FS_EXECUTOR_BENCH
		bench->direct ? "executor" : "worker",
#else
		bench->direct ? "worker" : "owner",
#endif
		ops, ticks, elapsed, bench->publication_ticks, bench->copy_ticks,
		bench->retirement_ticks, bench->worker_publications, switches);
	/* Native boot log writes are bounded. Keep counts in a separate record
	 * instead of silently losing a whole long measurement at that boundary. */
	pr_info("FS_INTERNAL_COMPLETION wake_calls=%llu batches=%llu\n",
		wakes_after - wakes, batches_after - batches);
#else
	pr_info("FS_INTERNAL_BENCH fs=%s opcode=%u depth=%u trial=%u ops=%llu ticks=%llu elapsed_ns=%llu task_ns=%llu\n",
		fs, opcode, depth, trial, ops, ticks, elapsed, runtime);
#endif
	pr_info("FS_INTERNAL_SWITCHES switches=%llu\n", switches);
#ifdef KOBOX_FS_INTERNAL_PROFILE
	pr_info("FS_INTERNAL_BOUNDARIES preempt=%llu irq=%llu clock=%llu handoff=%llu\n",
		after.calls[KOBOX_TASK_BENCH_PREEMPT] - before.calls[KOBOX_TASK_BENCH_PREEMPT],
		after.calls[KOBOX_TASK_BENCH_IRQ] - before.calls[KOBOX_TASK_BENCH_IRQ],
		after.calls[KOBOX_TASK_BENCH_CLOCK] - before.calls[KOBOX_TASK_BENCH_CLOCK],
		after.calls[KOBOX_TASK_BENCH_HANDOFF] - before.calls[KOBOX_TASK_BENCH_HANDOFF]);
	kobox_linux_fs_profile_report(bench->service);
	if (depth)
		kobox_linux_fs_workers_profile_report(bench->workers);
#endif
	return 0;
}

static int bench_file(struct fs_bench *bench, bool open)
{
	kb2_fs_request_t r = {
		.generation = 1, .sequence = ++bench->sequence,
		.opcode = open ? KB2_FILESYSTEM_OP_OPENAT2 : KB2_FILESYSTEM_OP_UNLINKAT,
		.flags = open ? O_CREAT | O_EXCL | O_RDWR : 0,
		.mode = open ? 0600 : 0,
		.path = { KB2_FILESYSTEM_REQUEST_SIZE, strlen(bench->path) + 1 },
	};
	kb2_fs_response_t reply;
	size_t used, size = KB2_FILESYSTEM_REQUEST_SIZE + r.path.length;
	int error;

	memcpy(bench->input + r.path.offset, bench->path, r.path.length);
	error = kb2_fs_request_encode(bench->input, size, size, &r);
	if (!error)
		error = kobox_linux_fs_service_dispatch(bench->service, 1,
			bench->input, size, bench->response,
			KB2_FILESYSTEM_RESPONSE_SIZE + 4096, &used);
	if (!error)
		error = kb2_fs_response_decode(bench->response, used, &reply);
	if (!error && reply.result < 0)
		error = reply.result;
	if (!error && open)
		bench->handles[0] = reply.handle;
	return error;
}

#endif /* !KOBOX_FS_HANDOFF_BENCH_ONLY */

int kobox_linux_fs_benchmark(struct vfsmount *root)
{
#ifdef KOBOX_FS_HANDOFF_BENCH_ONLY
	int result = bench_handoff();

	pr_info("FS_INTERNAL_HANDOFF_DONE status=%d\n", result);
	return result;
#else
	struct kobox_linux_fs_worker_config config = {
		.workers = 4, .client_running = 2,
		.client_requests = 8, .total_requests = 16,
		.client_bytes = 1 << 20, .total_bytes = 2 << 20,
		.control_bytes = 4096,
	};
	static const u32 opcodes[] = { KB2_FILESYSTEM_OP_STATX,
		KB2_FILESYSTEM_OP_PREAD, KB2_FILESYSTEM_OP_PWRITE };
#ifdef KOBOX_FS_PUBLICATION_BENCH
	static const unsigned int depths[] = { 1, 8 };
#else
	static const unsigned int depths[] = { 0, 1, 8 };
#endif
	static const char * const paths[] = {
		"/.kobox-fs-internal-bench", "/tmp/.kobox-fs-internal-bench",
	};
	struct fs_bench bench = {};
	unsigned int fs, trial, op, mode;
	int error = -ENOMEM, cleanup;
	bool created = false;

#ifndef KOBOX_FS_PUBLICATION_BENCH
	error = bench_guards();
	if (error)
		goto out;
#endif
	error = -ENOMEM;
	bench.input = kmalloc(KB2_FILESYSTEM_REQUEST_SIZE + 4096, GFP_KERNEL);
	bench.response = kmalloc(KB2_FILESYSTEM_RESPONSE_SIZE + 4096, GFP_KERNEL);
	if (!bench.input || !bench.response)
		goto out;
#ifdef KOBOX_FS_PUBLICATION_BENCH
	bench.tickets = kcalloc(8, sizeof(*bench.tickets), GFP_KERNEL);
	if (!bench.tickets)
		goto out;
#endif
	bench.service = kobox_linux_fs_service_create(root, 1);
	if (IS_ERR(bench.service)) {
		error = PTR_ERR(bench.service);
		bench.service = NULL;
		goto out;
	}
	for (unsigned int client = 1; client <= 2; client++) {
		error = kobox_linux_fs_service_register(bench.service, client, 1,
			current_cred(), KB2_FILESYSTEM_RIGHTS_VALID_MASK);
		if (error)
			goto out;
	}
#ifdef KOBOX_FS_EXECUTOR_BENCH
	config.publish = bench_publish;
#endif
	error = kobox_linux_fs_workers_create(bench.service, &config, &bench.workers);
	if (error)
		goto out;
#ifdef KOBOX_FS_PUBLICATION_BENCH
	bench.owner_workers = bench.workers;
	config.publish = bench_publish;
#ifdef KOBOX_FS_EXECUTOR_BENCH
	/* Both sides use workers for every opcode. Inline is not broadened, and
	 * the direct executor has no privileged same-task benchmark shortcut. */
	{
		struct kobox_linux_fs_executor_config executor_config = {
			.workers = config.workers, .client_running = config.client_running,
			.client_bytes = config.client_bytes, .total_bytes = config.total_bytes,
			.take = bench_take,
			.publish = bench_publish, .release = bench_release,
		};

		error = kobox_linux_fs_executors_create(bench.service, &executor_config, &bench.executors);
		for (unsigned int client = 0; !error && client < 2; client++)
			error = kobox_linux_fs_executors_register(bench.executors, client + 1,
				&bench.sources[client], &bench.lanes[client]);
		if (error)
			goto out;
	}
#endif
	error = kobox_linux_fs_workers_create(bench.service, &config, &bench.direct_workers);
	if (error)
		goto out;
#endif
	for (fs = 0; fs < ARRAY_SIZE(paths); fs++) {
		bench.path = paths[fs];
		error = bench_file(&bench, true);
		if (error)
			goto out;
		created = true;
		error = kobox_linux_fs_service_transfer(bench.service, 1, 2,
			bench.handles[0], &bench.handles[1]);
		bench.opcode = KB2_FILESYSTEM_OP_PWRITE;
		if (!error)
			error = bench_batch(&bench, 0);
		if (error)
			goto out;
#ifdef KOBOX_FS_EXECUTOR_BENCH
		error = bench_executor_checks(&bench, fs);
		if (error)
			goto out;
#endif
		for (trial = 1; trial <= 3; trial++)
			for (mode = 0; mode < ARRAY_SIZE(depths); mode++)
				for (op = 0; op < ARRAY_SIZE(opcodes); op++) {
#ifdef KOBOX_FS_PUBLICATION_BENCH
					/* Adjacent pairs, reversed each trial, on the same core.
					 * Idle tasks from the other pool never execute a job. */
					for (unsigned int variant = 0; variant < 2; variant++) {
						bench.direct = (trial + variant) % 2 == 0;
						bench.workers = bench.direct ? bench.direct_workers : bench.owner_workers;
						error = bench_phase(&bench, fs ? "tmpfs" : "ext4",
							opcodes[op], depths[mode], trial);
						if (error)
							goto out;
					}
#else
					error = bench_phase(&bench, fs ? "tmpfs" : "ext4",
						opcodes[op], depths[mode], trial);
					if (error)
						goto out;
#endif
				}
		error = bench_file(&bench, false);
		if (error)
			goto out;
		created = false;
	}
out:
	/* Failed diagnostics still quiesce every accepted job before freeing its
	 * shared input/output or unmounting. Do not hide the original failure.
	 */
#ifdef KOBOX_FS_PUBLICATION_BENCH
#ifdef KOBOX_FS_EXECUTOR_BENCH
	if (bench.executors) {
		for (unsigned int client = 0; client < 2; client++)
			if (bench.lanes[client]) {
				kobox_linux_fs_executors_pause(bench.lanes[client]);
				kobox_linux_fs_executors_unregister(bench.lanes[client]);
			}
		cleanup = kobox_linux_fs_executors_destroy(bench.executors);
		if (!error)
			error = cleanup;
	}
#endif
	for (unsigned int pool = 0; pool < 2; pool++) {
		bench.workers = pool ? bench.direct_workers : bench.owner_workers;
#endif
	if (bench.workers) {
		kobox_linux_fs_workers_close(bench.workers);
		for (;;) {
			struct kobox_linux_fs_work *work;
			const void *response;
			void *cookie;
			size_t used;
			int status;

			set_current_state(TASK_UNINTERRUPTIBLE);
			cleanup = kobox_linux_fs_workers_collect(bench.workers,
				&work, &cookie, &response, &used, &status);
			if (!cleanup)
				kobox_linux_fs_workers_release(bench.workers, work);
			__set_current_state(TASK_RUNNING);
			if (!cleanup)
				continue;
			cleanup = kobox_linux_fs_workers_destroy(bench.workers);
			if (cleanup != -EBUSY)
				break;
			cond_resched();
		}
		if (!error)
			error = cleanup;
	}
#ifdef KOBOX_FS_PUBLICATION_BENCH
	}
	kfree(bench.tickets);
#endif
	if (created)
		bench_file(&bench, false);
	kobox_linux_fs_service_destroy(bench.service);
	task_work_run();
	kfree(bench.response);
	kfree(bench.input);
	pr_info("FS_INTERNAL_BENCH_DONE status=%d\n", error);
	return error;
#endif
}
#endif /* KOBOX_FS_INTERNAL_BENCH */
