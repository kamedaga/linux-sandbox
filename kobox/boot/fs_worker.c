// SPDX-License-Identifier: GPL-2.0-only
#include "fs_worker.h"
#include "fs_service.h"
#include "fs_bench.h"
#include "../task/diagnostic.h"
#include "../arch/x86_64/host_call.h"

#include <linux/err.h>
#include <linux/fs_struct.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/cputime.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct fs_worker_client {
	struct list_head link;
	struct list_head ready;
	struct list_head queued;
	u64 id;
	u64 bytes;
	unsigned int admitted, running;
	bool revoked;
};

/* A data worker is either woken (running jobs, or about to look for one) or
 * on the pool's idle list. The owner wakes an idle worker for a new job; a
 * worker that finishes takes the next fair job itself, so no task ever scans
 * for work.
 *
 * A woken worker pulls whatever job is runnable when it gets to run; the
 * owner never binds a job to it. Waking a logical CPU whose host thread is
 * idle takes far longer than a job, and a job bound to such a worker would
 * wait for it even while another worker finishes and has nothing to do.
 */
struct fs_worker_context {
	struct kobox_linux_fs_workers *pool;
	struct fs_struct *scope;
	struct task_struct *task;
	bool woken;	/* off the idle list; written under pool->lock */
	struct list_head idle;
	int cpu;	/* bound logical CPU of a resident, -1 if migratable */
};

#ifdef KOBOX_RUNTIME_GATES
static bool fs_worker_gate_fail;

void kobox_linux_fs_workers_gate_fail_scope(void)
{
	WRITE_ONCE(fs_worker_gate_fail, true);
}
#endif

struct kobox_linux_fs_work {
	struct list_head link;
	struct kobox_linux_fs_workers *pool;
	struct fs_worker_client *client;
	struct kobox_linux_fs_request *request;
	struct kobox_linux_fs_request *storage;
	void *response, *cookie;
	size_t bytes, used, storage_bytes;
	int error;
	bool collected;
#ifdef KOBOX_FS_INTERNAL_PROFILE
	u64 completed_tick;
#endif
};

struct kobox_linux_fs_workers {
	struct kobox_linux_fs_service *service;
	struct kobox_linux_fs_worker_config config;
	struct task_struct *owner;
	struct task_struct **tasks;
	struct fs_worker_context *contexts;
	/* The owner's own scratch scope for inline (nowait) execution. */
	struct fs_struct *owner_scope;
	struct task_struct *control_task;
	struct kobox_linux_fs_work *control_queued;
	struct list_head clients, runnable, completed, idle;
	struct list_head free_clients, free_works;
	spinlock_t lock;
	wait_queue_head_t control_ready;
	wait_queue_head_t owner_ready;
	wait_queue_entry_t owner_wait;
	u64 bytes, cached_bytes;
	unsigned int admitted;
	unsigned int control_admitted;
	/* An interval runs from the first admission to the drain back to zero.
	 * burst: this interval has had more than one request admitted.
	 * previous_burst: the last drained interval had. Every burst begins
	 * with a single admission, so only the history tells it from a lone
	 * request.
	 */
	bool burst, previous_burst;
	/* Workers woken for a job that have not yet looked at runnable. */
	unsigned int waking;
	bool closed;
#ifdef KOBOX_FS_INTERNAL_BENCH
	u64 completion_wake_calls, collection_batches;
#endif
#ifdef KOBOX_FS_INTERNAL_PROFILE
	u64 submit_calls, submit_ticks, release_ticks;
	u64 ready_wake_ticks, collect_calls, completion_to_collect_ticks;
	u64 new_work, reused_work, new_client, reused_client;
	u64 single_submits, empty_wakes;
#endif
#if defined(KOBOX_FS_INTERNAL_PROFILE) || defined(KOBOX_RUNTIME_GATES)
	u64 new_buffer, reused_buffer;
	u64 inline_hits, inline_punts;
#endif
#ifdef KOBOX_RUNTIME_GATES
	/* Gate only: keep the worker-path scenarios on workers. */
	bool gate_no_inline;
#endif
#ifdef KOBOX_RUNTIME_GATES
	u64 evicted_buffers;
#endif
};

static bool fs_worker_owner(struct kobox_linux_fs_workers *pool)
{
	return pool && pool->owner == current;
}

#ifdef KOBOX_FS_INTERNAL_PROFILE
void kobox_linux_fs_workers_boundaries(struct kobox_linux_fs_workers *pool,
	struct kobox_task_bench_counts *counts)
{
	struct kobox_task_bench_counts worker;
	unsigned int i, kind;

	kobox_task_bench_read(pool->owner, counts);
	for (i = 0; i < pool->config.workers; i++) {
		kobox_task_bench_read(pool->tasks[i], &worker);
		for (kind = 0; kind < KOBOX_TASK_BENCH_COUNT; kind++)
			counts->calls[kind] += worker.calls[kind];
	}
}

#ifdef KOBOX_FS_BOUNDARY_CALLERS
void kobox_linux_fs_workers_trace(struct kobox_linux_fs_workers *pool, bool enabled)
{
	unsigned int i;

	kobox_task_bench_trace(pool->owner, enabled);
	for (i = 0; i < pool->config.workers; i++)
		kobox_task_bench_trace(pool->tasks[i], enabled);
}
#endif

#endif /* KOBOX_FS_INTERNAL_PROFILE */

#ifdef KOBOX_FS_INTERNAL_BENCH
static u64 fs_worker_switches(struct task_struct *task)
{
	return READ_ONCE(task->nvcsw) + READ_ONCE(task->nivcsw);
}

u64 kobox_linux_fs_workers_switches(struct kobox_linux_fs_workers *pool)
{
	u64 total = fs_worker_switches(pool->owner);
	unsigned int i;

	for (i = 0; i < pool->config.workers; i++)
		total += fs_worker_switches(pool->tasks[i]);
	return total;
}

u64 kobox_linux_fs_workers_runtime(struct kobox_linux_fs_workers *pool)
{
	u64 total = task_sched_runtime(pool->owner);
	unsigned int i;

	for (i = 0; i < pool->config.workers; i++)
		total += task_sched_runtime(pool->tasks[i]);
	return total;
}

void kobox_linux_fs_workers_completion_counts(struct kobox_linux_fs_workers *pool,
	u64 *wakes, u64 *batches)
{
	spin_lock(&pool->lock);
	*wakes = pool->completion_wake_calls;
	*batches = pool->collection_batches;
	spin_unlock(&pool->lock);
}

#endif /* KOBOX_FS_INTERNAL_BENCH */

#ifdef KOBOX_FS_INTERNAL_PROFILE
void kobox_linux_fs_workers_profile_reset(struct kobox_linux_fs_workers *pool)
{
	pool->submit_calls = pool->submit_ticks = pool->release_ticks = 0;
	pool->ready_wake_ticks = pool->collect_calls =
		pool->completion_to_collect_ticks = 0;
	pool->new_work = pool->reused_work = 0;
	pool->new_client = pool->reused_client = 0;
	pool->single_submits = pool->empty_wakes = 0;
	pool->inline_hits = pool->inline_punts = 0;
	pool->new_buffer = pool->reused_buffer = 0;
}

void kobox_linux_fs_workers_profile_report(struct kobox_linux_fs_workers *pool)
{
	pr_info("FS_WORKER_PROFILE calls=%llu submit=%llu release=%llu\n",
		pool->submit_calls, pool->submit_ticks, pool->release_ticks);
	/* Wake can switch tasks before returning. These are inclusive elapsed
	 * intervals, not CPU shares; the completion interval can overlap wake.
	 * Only the owner aggregates counters after acquiring the published job.
	 */
	pr_info("FS_WORKER_PATH calls=%llu submit_nonwake=%llu ready_wake=%llu completion_to_collect=%llu\n",
		pool->collect_calls, pool->submit_ticks - pool->ready_wake_ticks,
		pool->ready_wake_ticks, pool->completion_to_collect_ticks);
	pr_info("FS_WORKER_META new_work=%llu reused_work=%llu new_client=%llu reused_client=%llu\n",
		pool->new_work, pool->reused_work, pool->new_client, pool->reused_client);
	/* inline: completed on the owner; woken: woke an idle worker at submit;
	 * queued: no worker was idle. The three add up to admissions. empty: a
	 * woken worker found its job already taken by a finishing worker. */
	pr_info("FS_WORKER_PLACEMENT inline=%llu woken=%llu queued=%llu empty=%llu\n",
		pool->inline_hits, pool->single_submits,
		pool->submit_calls - pool->single_submits - pool->inline_hits,
		pool->empty_wakes);
	pr_info("FS_WORKER_BUFFER new=%llu reused=%llu\n",
		pool->new_buffer, pool->reused_buffer);
}
#endif

/* Only the owner recycles unlinked, fully released metadata. A worker never
 * dereferences work/client after publishing completion. Payload leases and
 * authority refs are not cached. Taking a free object before allocating bounds
 * cached+live objects by the data admission count, without another fixed cap.
 */
static struct fs_worker_client *fs_worker_client_alloc(
	struct kobox_linux_fs_workers *pool, u64 id)
{
	struct fs_worker_client *client;

	if (list_empty(&pool->free_clients)) {
		client = kzalloc(sizeof(*client), GFP_KERNEL);
#ifdef KOBOX_FS_INTERNAL_PROFILE
		if (client)
			pool->new_client++;
#endif
	} else {
		client = list_first_entry(&pool->free_clients,
			struct fs_worker_client, link);
		list_del(&client->link);
#ifdef KOBOX_FS_INTERNAL_PROFILE
		pool->reused_client++;
#endif
	}
	if (!client)
		return NULL;
	client->id = id;
	INIT_LIST_HEAD(&client->link);
	INIT_LIST_HEAD(&client->ready);
	INIT_LIST_HEAD(&client->queued);
	return client;
}

static void fs_worker_client_free(struct kobox_linux_fs_workers *pool,
	struct fs_worker_client *client)
{
	if (pool->closed) {
		kfree(client);
		return;
	}
	/* Forget identity and revocation before reusing storage for another
	 * client; an empty free object is not a registry or an authority cache.
	 */
	memset(client, 0, sizeof(*client));
	list_add(&client->link, &pool->free_clients);
}

static struct kobox_linux_fs_work *fs_worker_work_alloc(
	struct kobox_linux_fs_workers *pool, size_t bytes)
{
	struct kobox_linux_fs_work *work, *candidate;

	if (list_empty(&pool->free_works)) {
		work = kzalloc(sizeof(*work), GFP_KERNEL);
#ifdef KOBOX_FS_INTERNAL_PROFILE
		if (work)
			pool->new_work++;
#endif
	} else {
		work = list_first_entry(&pool->free_works,
			struct kobox_linux_fs_work, link);
		/* Match capacity, never client identity or opcode. Mixed workloads
		 * can reuse released storage without retaining over-sized slack.
		 */
		list_for_each_entry(candidate, &pool->free_works, link)
			if (candidate->storage_bytes == bytes) {
				work = candidate;
				break;
			}
		list_del(&work->link);
		pool->cached_bytes -= work->storage_bytes;
		if (work->storage_bytes != bytes) {
			kobox_linux_fs_request_destroy(work->storage);
			work->storage = NULL;
			work->storage_bytes = 0;
		}
#ifdef KOBOX_FS_INTERNAL_PROFILE
		pool->reused_work++;
#endif
	}
	return work;
}

static void fs_worker_work_free(struct kobox_linux_fs_workers *pool,
	struct kobox_linux_fs_work *work)
{
	struct kobox_linux_fs_request *storage = work->storage;
	size_t bytes = work->storage_bytes;

	if (pool->closed) {
		kobox_linux_fs_request_destroy(storage);
		kfree(work);
		return;
	}
	/* No request, cookie, response pointer or collected state may survive
	 * into another job. Only scrubbed storage and its capacity survive.
	 */
	memset(work, 0, sizeof(*work));
	work->storage = storage;
	work->storage_bytes = bytes;
	pool->cached_bytes += bytes;
	list_add(&work->link, &pool->free_works);
}

static void fs_worker_cache_trim(struct kobox_linux_fs_workers *pool,
	size_t incoming)
{
	struct kobox_linux_fs_work *work;
	u64 available = pool->config.total_bytes - pool->bytes - incoming;

	/* Idle storage shares the existing byte budget with live leases. Evict
	 * it before allocating, so caching cannot reject a previously admissible
	 * request or turn the transport's usual size into a filesystem limit.
	 */
	list_for_each_entry(work, &pool->free_works, link) {
		if (pool->cached_bytes <= available)
			break;
		pool->cached_bytes -= work->storage_bytes;
#ifdef KOBOX_RUNTIME_GATES
		if (work->storage)
			pool->evicted_buffers++;
#endif
		kobox_linux_fs_request_destroy(work->storage);
		work->storage = NULL;
		work->storage_bytes = 0;
	}
}

static void fs_worker_cache_free(struct kobox_linux_fs_workers *pool)
{
	struct fs_worker_client *client, *next_client;
	struct kobox_linux_fs_work *work, *next_work;

	list_for_each_entry_safe(client, next_client, &pool->free_clients, link) {
		list_del(&client->link);
		kfree(client);
	}
	list_for_each_entry_safe(work, next_work, &pool->free_works, link) {
		list_del(&work->link);
		kobox_linux_fs_request_destroy(work->storage);
		kfree(work);
	}
	pool->cached_bytes = 0;
}

#ifdef KOBOX_RUNTIME_GATES
int kobox_linux_fs_workers_gate_storage(struct kobox_linux_fs_workers *pool,
	u64 *bytes, u64 *reused, u64 *evicted)
{
	if (!fs_worker_owner(pool) || !bytes || !reused || !evicted)
		return -EINVAL;
	*bytes = pool->cached_bytes;
	*reused = pool->reused_buffer;
	*evicted = pool->evicted_buffers;
	return 0;
}

void kobox_linux_fs_workers_gate_inline(struct kobox_linux_fs_workers *pool,
	bool enabled, u64 *hits, u64 *punts)
{
	pool->gate_no_inline = !enabled;
	*hits = pool->inline_hits;
	*punts = pool->inline_punts;
}

int kobox_linux_fs_workers_gate_cached(struct kobox_linux_fs_workers *pool,
	unsigned int *works, unsigned int *clients)
{
	struct fs_worker_client *client;
	struct kobox_linux_fs_work *work;
	u64 bytes = 0;

	if (!fs_worker_owner(pool) || !works || !clients)
		return -EINVAL;
	*works = *clients = 0;
	list_for_each_entry(client, &pool->free_clients, link) {
		if (client->id || client->bytes || client->admitted ||
		    client->running || client->revoked)
			return -EINVAL;
		(*clients)++;
	}
	list_for_each_entry(work, &pool->free_works, link) {
		if (work->pool || work->client || work->request || work->response ||
		    work->cookie || work->bytes || work->used || work->error || work->collected)
			return -EINVAL;
		if (work->storage ? kobox_linux_fs_request_gate_recycled(work->storage,
			work->storage_bytes) : work->storage_bytes != 0)
			return -EINVAL;
		bytes += work->storage_bytes;
		(*works)++;
	}
	if (bytes != pool->cached_bytes ||
	    bytes > pool->config.total_bytes - pool->bytes)
		return -EINVAL;
	return 0;
}
#endif

/* Only the owner adds/removes clients. Workers use this list under the lock,
 * but allocation, snapshotting, VFS and freeing never hold that lock.
 */
static struct fs_worker_client *fs_worker_client_find(
	struct kobox_linux_fs_workers *pool, u64 id)
{
	struct fs_worker_client *client;

	list_for_each_entry(client, &pool->clients, link)
		if (client->id == id)
			return client;
	return NULL;
}

static void fs_worker_client_ready(struct kobox_linux_fs_workers *pool,
	struct fs_worker_client *client)
{
	if (list_empty(&client->ready) && !list_empty(&client->queued) &&
	    client->running < pool->config.client_running)
		list_add_tail(&client->ready, &pool->runnable);
}

/* Caller holds pool->lock. */
static struct kobox_linux_fs_work *fs_worker_take(
	struct kobox_linux_fs_workers *pool)
{
	struct fs_worker_client *client;
	struct kobox_linux_fs_work *work = NULL;

	if (!list_empty(&pool->runnable)) {
		client = list_first_entry(&pool->runnable, struct fs_worker_client, ready);
		work = list_first_entry(&client->queued,
				       struct kobox_linux_fs_work, link);
		list_del_init(&work->link);
		client->running++;
		/* Rotate one eligible client, without scanning idle/blocked clients
		 * or draining every queued request of a greedy producer.
		 */
		list_del_init(&client->ready);
		fs_worker_client_ready(pool, client);
	}
	return work;
}

/* Caller holds pool->lock. A lone request goes to this CPU's resident: the
 * owner is about to wait, so the wake is a local handoff. With other requests
 * in flight the owner keeps admitting, so a local worker would only preempt it
 * and serialize the burst: prefer another CPU's resident, then a migratable
 * worker, and this CPU's resident last. The first request of a burst is the
 * only one admitted when it is placed, so a request counts as lone only
 * when the last drained interval was lone too; one lone interval restores
 * the local handoff.
 */
static struct fs_worker_context *fs_worker_idle(struct kobox_linux_fs_workers *pool)
{
	struct fs_worker_context *worker, *best = NULL;
	const int cpu = smp_processor_id();
	const bool lone = pool->admitted == 1 && !pool->previous_burst;
	int best_rank = -1;

	list_for_each_entry(worker, &pool->idle, idle) {
		const bool local = worker->cpu == cpu;
		const int rank = lone ? (local ? 3 : worker->cpu >= 0 ? 2 : 1) :
			(local ? 1 : worker->cpu >= 0 ? 3 : 2);

		if (rank > best_rank) {
			best = worker;
			best_rank = rank;
			if (rank == 3)
				break;
		}
	}
	return best;
}

/* Caller holds pool->lock. Wake one idle worker per runnable job; return the
 * workers to wake once the lock is dropped. Each runnable job is matched by a
 * woken or running worker that will look at runnable before it goes idle.
 * About a third of burst wakes find their job already taken by a finishing
 * worker. Not waking while as many workers are on their way as jobs are
 * queued removed those, but lost more burst throughput than they cost.
 */
static unsigned int fs_worker_dispatch(struct kobox_linux_fs_workers *pool,
	struct task_struct **wake, unsigned int capacity)
{
	struct fs_worker_context *worker;
	unsigned int count = 0;

	while (count < capacity && !list_empty(&pool->runnable) &&
	       (worker = fs_worker_idle(pool))) {
		list_del_init(&worker->idle);
		WRITE_ONCE(worker->woken, true);
		pool->waking++;
		wake[count++] = worker->task;
	}
	return count;
}

static void fs_worker_owner_wake(struct kobox_linux_fs_workers *pool,
	bool sync)
{
	/* An idle completion worker is about to sleep. Let Linux prefer a
	 * same-CPU handoff instead of waking the owner on another idle CPU.
	 * This is a placement hint, not affinity or a substitute for wakeup.
	 */
	if (sync)
		__wake_up_sync(&pool->owner_ready, TASK_NORMAL);
	else
		/* The owner reference is pinned through every worker join.
		 * Backlog needs no synchronous placement hint or wait-list lock;
		 * upstream wakeup retains the arm-before-collect state barrier.
		 */
		wake_up_process(pool->owner);
}

static int fs_worker_run(void *context)
{
	struct fs_worker_context *worker = context;
	struct kobox_linux_fs_workers *pool = worker->pool;
	struct kobox_linux_fs_work *work, *next;
	bool lone;

	for (;;) {
		/* The owner marks this task woken and then wakes it; arming the
		 * state first means that wake cannot be lost. A claimed job runs
		 * even if stop races, so its admission lease is never stranded.
		 */
		set_current_state(TASK_IDLE);
		if (!READ_ONCE(worker->woken)) {
			if (kthread_should_stop())
				break;
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		spin_lock(&pool->lock);
		pool->waking--;
		/* A worker that finished first may already have taken the job this
		 * wake was for; then go back to idle, runnable being empty. */
		work = fs_worker_take(pool);
		if (!work) {
			WRITE_ONCE(worker->woken, false);
			list_add(&worker->idle, &pool->idle);
#ifdef KOBOX_FS_INTERNAL_PROFILE
			pool->empty_wakes++;
#endif
		}
		spin_unlock(&pool->lock);
		for (; work; work = next) {
			work->error = kobox_linux_fs_request_execute_scope(work->request,
				worker->scope, &work->used);
			/* The consumer can observe used before the owner is scheduled.
			 * Keep the job off completed until the host publisher has stopped
			 * using its response, cookie, mapping and notification endpoint. */
			if (pool->config.publish)
				kobox_host_call((pool->config.publish(work->cookie,
					work->response, work->used, work->error), 0));
			spin_lock(&pool->lock);
			work->client->running--;
			fs_worker_client_ready(pool, work->client);
#ifdef KOBOX_FS_INTERNAL_PROFILE
			/* Record before publication: after waking the owner, the job may
			 * already be collected and freed, so workers must not touch it.
			 */
			work->completed_tick = kobox_fs_bench_ticks();
#endif
			list_add_tail(&work->link, &pool->completed);
#ifdef KOBOX_FS_INTERNAL_BENCH
			pool->completion_wake_calls++;
#endif
			lone = !pool->burst;
			/* Fill the running slot this completion released ourselves
			 * rather than waking another worker to race for it, so no worker
			 * ever wakes another (whose task the owner may already have
			 * stopped). A worker goes idle only with runnable empty, every
			 * submission while one is idle wakes one, and a completion makes
			 * at most one client runnable again and takes a job itself. So
			 * runnable work beside an idle worker always has a woken worker
			 * still on its way to take it: no job is stranded.
			 */
			next = fs_worker_take(pool);
			if (!next) {
				WRITE_ONCE(worker->woken, false);
				list_add(&worker->idle, &pool->idle);
			}
			WARN_ON_ONCE(!list_empty(&pool->runnable) &&
				     !list_empty(&pool->idle) && !pool->waking);
			spin_unlock(&pool->lock);
			/* A synchronous wake pulls the owner toward this CPU. That suits a
			 * lone request (this worker is about to sleep), not a burst whose
			 * other jobs keep the owner admitting and collecting elsewhere.
			 * Even the last job of a burst counts as part of that burst.
			 */
			fs_worker_owner_wake(pool, !next && lone);
		}
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int fs_worker_control_run(void *context)
{
	struct fs_worker_context *worker = context;
	struct kobox_linux_fs_workers *pool = worker->pool;
	struct kobox_linux_fs_work *work;

	for (;;) {
		wait_event_idle(pool->control_ready, kthread_should_stop() ||
			READ_ONCE(pool->control_queued));
		if (kthread_should_stop())
			break;
		spin_lock(&pool->lock);
		work = pool->control_queued;
		pool->control_queued = NULL;
		spin_unlock(&pool->lock);
		if (!work)
			continue;
		work->error = kobox_linux_fs_request_execute_scope(work->request,
			worker->scope, &work->used);
		spin_lock(&pool->lock);
		list_add_tail(&work->link, &pool->completed);
#ifdef KOBOX_FS_INTERNAL_BENCH
		pool->completion_wake_calls++;
#endif
		spin_unlock(&pool->lock);
		fs_worker_owner_wake(pool, true);
	}
	return 0;
}

static void fs_worker_contexts_free(struct kobox_linux_fs_workers *pool)
{
	size_t i;

	if (!pool->contexts)
		return;
	/* Only before tasks start or after every task has joined. Last-operation
	 * root/pwd refs must not outlive the service/mount retirement boundary.
	 */
	for (i = 0; i <= pool->config.workers; i++)
		if (pool->contexts[i].scope)
			free_fs_struct(pool->contexts[i].scope);
	kfree(pool->contexts);
	if (pool->owner_scope)
		free_fs_struct(pool->owner_scope);
}

int kobox_linux_fs_workers_create(struct kobox_linux_fs_service *service,
	const struct kobox_linux_fs_worker_config *config,
	struct kobox_linux_fs_workers **workers)
{
	struct kobox_linux_fs_workers *pool;
	unsigned int i, cpu;
	int error;

	if (!workers)
		return -EINVAL;
	*workers = NULL;
	if (!service || !config || config->workers < 3 ||
	    config->client_running < 2 || config->client_running >= config->workers ||
	    config->client_requests < config->client_running ||
	    config->client_requests > config->total_requests / 2 ||
	    !config->client_bytes || config->client_bytes > config->total_bytes / 2 ||
	    config->total_bytes > SIZE_MAX || !config->control_bytes ||
	    config->control_bytes > SIZE_MAX - config->total_bytes)
		return -EINVAL;
	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	pool->tasks = kcalloc(config->workers, sizeof(*pool->tasks), GFP_KERNEL);
	if (!pool->tasks) {
		kfree(pool);
		return -ENOMEM;
	}
	pool->service = service;
	pool->config = *config;
	pool->owner = current;
	get_task_struct(pool->owner);
	INIT_LIST_HEAD(&pool->clients);
	INIT_LIST_HEAD(&pool->runnable);
	INIT_LIST_HEAD(&pool->completed);
	INIT_LIST_HEAD(&pool->idle);
	INIT_LIST_HEAD(&pool->free_clients);
	INIT_LIST_HEAD(&pool->free_works);
	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->control_ready);
	init_waitqueue_head(&pool->owner_ready);
	/* Keep one non-autoremove entry until all workers join. Owner waiters
	 * already arm task state before collect; a running owner needs no wake.
	 * The existing owner task reference pins this entry's private pointer.
	 */
	init_waitqueue_entry(&pool->owner_wait, pool->owner);
	add_wait_queue_exclusive(&pool->owner_ready, &pool->owner_wait);
	pool->contexts = kcalloc((size_t)config->workers + 1,
		sizeof(*pool->contexts), GFP_KERNEL);
	if (!pool->contexts) {
		error = -ENOMEM;
		goto out_contexts;
	}
	for (size_t slot = 0; slot <= config->workers; slot++) {
		pool->contexts[slot].pool = pool;
		pool->contexts[slot].cpu = -1;
		INIT_LIST_HEAD(&pool->contexts[slot].idle);
#ifdef KOBOX_RUNTIME_GATES
		/* Fail after a retained context exists, so the gate exercises the
		 * partial-construction reference unwind, not just the first alloc.
		 */
		if (slot == 1 && xchg(&fs_worker_gate_fail, false)) {
			error = -ENOMEM;
			goto out_contexts;
		}
#endif
		pool->contexts[slot].scope =
			copy_fs_struct(current->fs ?: init_task.fs);
		if (!pool->contexts[slot].scope) {
			error = -ENOMEM;
			goto out_contexts;
		}
	}
	pool->owner_scope = copy_fs_struct(current->fs ?: init_task.fs);
	if (!pool->owner_scope) {
		error = -ENOMEM;
		goto out_contexts;
	}
	cpu = cpumask_first(cpu_online_mask);
	for (i = 0; i < config->workers; i++) {
		pool->tasks[i] = kthread_create(fs_worker_run,
			&pool->contexts[i], "fs-worker/%u", i);
		if (!IS_ERR(pool->tasks[i])) {
			/* One local resident per online logical CPU, not one private
			 * VFS or mount per CPU. Remaining workers are migratable and
			 * keep progress when residents sleep in blocking VFS calls.
			 */
			if (cpu < nr_cpu_ids) {
				kthread_bind(pool->tasks[i], cpu);
				pool->contexts[i].cpu = cpu;
				cpu = cpumask_next(cpu, cpu_online_mask);
			}
			/* The owner wakes this task after dropping the pool lock and
			 * stops it at destroy; hold it like kthread_stop_put expects. */
			get_task_struct(pool->tasks[i]);
			pool->contexts[i].task = pool->tasks[i];
			list_add_tail(&pool->contexts[i].idle, &pool->idle);
			wake_up_process(pool->tasks[i]);
			continue;
		}
		error = PTR_ERR(pool->tasks[i]);
		while (i)
			kthread_stop_put(pool->tasks[--i]);
		goto out_contexts;
	}
	pool->control_task = kthread_run(fs_worker_control_run,
		&pool->contexts[config->workers], "fs-control");
	if (IS_ERR(pool->control_task)) {
		error = PTR_ERR(pool->control_task);
		for (i = 0; i < config->workers; i++)
			kthread_stop_put(pool->tasks[i]);
		goto out_contexts;
	}
	*workers = pool;
	return 0;
out_contexts:
	fs_worker_contexts_free(pool);
	remove_wait_queue(&pool->owner_ready, &pool->owner_wait);
	put_task_struct(pool->owner);
	kfree(pool->tasks);
	kfree(pool);
	return error;
}

static int fs_worker_submit(struct kobox_linux_fs_workers *pool,
	u64 id, const void *input, size_t input_size, size_t response_capacity,
	void *cookie)
{
	struct fs_worker_client *client;
	struct kobox_linux_fs_work *work;
	struct kobox_linux_fs_request *storage;
	size_t bytes;
	struct task_struct *wake[1];
	bool new_client = false, inline_done;
	unsigned int count;
	int error;

	if (!fs_worker_owner(pool) || !id || !input)
		return -EINVAL;
	if (pool->closed)
		return -ESHUTDOWN;
	if (check_add_overflow(input_size, response_capacity, &bytes))
		return -EOVERFLOW;
	if (bytes > pool->config.client_bytes || bytes > pool->config.total_bytes)
		return -ENOMEM;
	spin_lock(&pool->lock);
	client = fs_worker_client_find(pool, id);
	if (client && client->revoked)
		error = -EACCES;
	else if (pool->admitted >= pool->config.total_requests ||
		 bytes > pool->config.total_bytes - pool->bytes ||
		 (client && (client->admitted >= pool->config.client_requests ||
		 bytes > pool->config.client_bytes - client->bytes)))
		error = -EAGAIN;
	else
		error = 0;
	spin_unlock(&pool->lock);
	if (error)
		return error;
	if (!client) {
		client = fs_worker_client_alloc(pool, id);
		if (!client)
			return -ENOMEM;
		new_client = true;
	}
	work = fs_worker_work_alloc(pool, bytes);
	if (!work) {
		error = -ENOMEM;
		goto out_client;
	}
	fs_worker_cache_trim(pool, bytes);
	storage = work->storage;
	work->storage = NULL;
	work->storage_bytes = 0;
	error = kobox_linux_fs_request_prepare_owned(pool->service, id, input,
		input_size, response_capacity, storage, &work->request, &work->response);
	if (error == -ENOMEM && pool->cached_bytes) {
		/* This failure precedes VFS execution. Reclaim idle storage and
		 * retry allocation once; never replay an executed filesystem op.
		 */
		fs_worker_cache_trim(pool, pool->config.total_bytes - pool->bytes);
		storage = NULL;
		error = kobox_linux_fs_request_prepare_owned(pool->service, id, input,
			input_size, response_capacity, NULL,
			&work->request, &work->response);
	}
	if (error)
		goto out_work;
#if defined(KOBOX_FS_INTERNAL_PROFILE) || defined(KOBOX_RUNTIME_GATES)
	if (storage)
		pool->reused_buffer++;
	else
		pool->new_buffer++;
#endif
	work->pool = pool;
	work->client = client;
	work->cookie = cookie;
	work->bytes = bytes;
	/* A Linux task switch costs microseconds here: try the request on the
	 * owner first. Only operations that complete without sleeping finish
	 * inline (see kobox_linux_fs_request_try_nowait); -EAGAIN leaves the
	 * job untouched for a worker, so blocking VFS still never stalls the
	 * owner or other clients. Admission and budgets are charged either way.
	 */
#ifdef KOBOX_RUNTIME_GATES
	if (pool->gate_no_inline)
		error = -EAGAIN;
	else
#endif
	error = kobox_linux_fs_request_try_nowait(work->request, pool->owner_scope,
		&work->used);
	/* Executed, not the return value, says whether the job is done: an
	 * encoding failure after execution must still be published. */
	inline_done = kobox_linux_fs_request_executed(work->request);
	if (inline_done)
		work->error = error;
#if defined(KOBOX_FS_INTERNAL_PROFILE) || defined(KOBOX_RUNTIME_GATES)
	if (inline_done)
		pool->inline_hits++;
	else
		pool->inline_punts++;
#endif
	spin_lock(&pool->lock);
	if (new_client)
		list_add_tail(&client->link, &pool->clients);
	client->admitted++;
	client->bytes += bytes;
	pool->admitted++;
	if (pool->admitted > 1)
		pool->burst = true;
	pool->bytes += bytes;
	if (inline_done) {
		/* The owner collects its own completion; nobody needs a wake. */
#ifdef KOBOX_FS_INTERNAL_PROFILE
		work->completed_tick = kobox_fs_bench_ticks();
#endif
		list_add_tail(&work->link, &pool->completed);
		spin_unlock(&pool->lock);
		return 0;
	}
	list_add_tail(&work->link, &client->queued);
	fs_worker_client_ready(pool, client);
	/* An idle worker takes it now; otherwise a finishing worker will. */
	count = fs_worker_dispatch(pool, wake, ARRAY_SIZE(wake));
#ifdef KOBOX_FS_INTERNAL_PROFILE
	pool->single_submits += count;
#endif
	spin_unlock(&pool->lock);
#ifdef KOBOX_FS_INTERNAL_PROFILE
	{
		u64 start = kobox_fs_bench_ticks();

		if (count)
			wake_up_process(wake[0]);
		pool->ready_wake_ticks += kobox_fs_bench_ticks() - start;
	}
#else
	if (count)
		wake_up_process(wake[0]);
#endif
	return 0;

out_work:
	fs_worker_work_free(pool, work);
out_client:
	if (new_client)
		fs_worker_client_free(pool, client);
	return error;
}

int kobox_linux_fs_workers_submit(struct kobox_linux_fs_workers *pool,
	u64 id, const void *input, size_t input_size, size_t response_capacity,
	void *cookie)
{
#ifdef KOBOX_FS_INTERNAL_PROFILE
	u64 start = kobox_fs_bench_ticks();
	int error = fs_worker_submit(pool, id, input, input_size,
		response_capacity, cookie);

	if (!error) {
		pool->submit_calls++;
		pool->submit_ticks += kobox_fs_bench_ticks() - start;
	}
	return error;
#else
	return fs_worker_submit(pool, id, input, input_size,
		response_capacity, cookie);
#endif
}

int kobox_linux_fs_workers_submit_control(struct kobox_linux_fs_workers *pool,
	const void *input, size_t input_size, size_t response_capacity, void *cookie)
{
	struct kobox_linux_fs_work *work;
	size_t bytes;
	int error;

	if (!fs_worker_owner(pool) || !input)
		return -EINVAL;
	if (pool->closed)
		return -ESHUTDOWN;
	if (check_add_overflow(input_size, response_capacity, &bytes))
		return -EOVERFLOW;
	if (bytes > pool->config.control_bytes)
		return -ENOMEM;
	if (pool->control_admitted)
		return -EAGAIN;
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	work->response = kvmalloc(response_capacity, GFP_KERNEL);
	if (!work->response) {
		error = -ENOMEM;
		goto out_work;
	}
	error = kobox_linux_fs_request_prepare_control(pool->service, input,
		input_size, work->response, response_capacity, &work->request);
	if (error)
		goto out_response;
	work->pool = pool;
	work->cookie = cookie;
	work->bytes = bytes;
	pool->control_admitted = 1;
	spin_lock(&pool->lock);
	pool->control_queued = work;
	spin_unlock(&pool->lock);
	wake_up(&pool->control_ready);
	return 0;

out_response:
	kvfree(work->response);
out_work:
	kfree(work);
	return error;
}

int kobox_linux_fs_workers_collect(struct kobox_linux_fs_workers *pool,
	struct kobox_linux_fs_work **job, void **cookie, const void **response,
	size_t *used, int *error)
{
	struct kobox_linux_fs_work *work;

	if (!fs_worker_owner(pool) || !job || !cookie || !response || !used || !error)
		return -EINVAL;
	*job = NULL;
	*cookie = NULL;
	*response = NULL;
	*used = 0;
	*error = 0;
	spin_lock(&pool->lock);
	if (list_empty(&pool->completed)) {
		spin_unlock(&pool->lock);
		return -EAGAIN;
	}
	work = list_first_entry(&pool->completed, struct kobox_linux_fs_work, link);
	list_del_init(&work->link);
	work->collected = true;
#ifdef KOBOX_FS_INTERNAL_BENCH
	pool->collection_batches++;
#endif
	spin_unlock(&pool->lock);
#ifdef KOBOX_FS_INTERNAL_PROFILE
	if (work->client) {
		pool->collect_calls++;
		pool->completion_to_collect_ticks +=
			kobox_fs_bench_ticks() - work->completed_tick;
	}
#endif
	*job = work;
	*cookie = work->cookie;
	*response = work->response;
	*used = work->used;
	*error = work->error;
	return 0;
}

static int fs_worker_release(struct kobox_linux_fs_workers *pool,
	struct kobox_linux_fs_work *work)
{
	struct fs_worker_client *client;
	bool remove;

	if (!fs_worker_owner(pool) || !work || work->pool != pool || !work->collected)
		return -EINVAL;
	client = work->client;
	spin_lock(&pool->lock);
	if (client) {
		client->admitted--;
		client->bytes -= work->bytes;
		pool->admitted--;
		if (!pool->admitted) {
			pool->previous_burst = pool->burst;
			pool->burst = false;
		}
		pool->bytes -= work->bytes;
	} else {
		pool->control_admitted = 0;
	}
	/* The registry owns revocation. Remove an empty active group; its cleared
	 * allocation can be reused without retaining an identity tombstone.
	 */
	remove = client && !client->admitted;
	if (remove)
		list_del(&client->link);
	spin_unlock(&pool->lock);
	/* Data responses belong to their packed request. Control requests still
	 * borrow the pool's separate response allocation.
	 */
	if (!client) {
		kobox_linux_fs_request_destroy(work->request);
		kvfree(work->response);
		kfree(work);
	} else {
		if (pool->closed)
			kobox_linux_fs_request_destroy(work->request);
		else {
			work->storage_bytes =
				kobox_linux_fs_request_recycle(work->request);
			work->storage = work->request;
		}
		fs_worker_work_free(pool, work);
	}
	if (remove)
		fs_worker_client_free(pool, client);
	return 0;
}

int kobox_linux_fs_workers_release(struct kobox_linux_fs_workers *pool,
	struct kobox_linux_fs_work *work)
{
#ifdef KOBOX_FS_INTERNAL_PROFILE
	u64 start = kobox_fs_bench_ticks();
	int error = fs_worker_release(pool, work);

	if (!error)
		pool->release_ticks += kobox_fs_bench_ticks() - start;
	return error;
#else
	return fs_worker_release(pool, work);
#endif
}

int kobox_linux_fs_workers_revoke(struct kobox_linux_fs_workers *pool, u64 id)
{
	struct fs_worker_client *client;

	if (!fs_worker_owner(pool) || !id)
		return -EINVAL;
	spin_lock(&pool->lock);
	client = fs_worker_client_find(pool, id);
	if (client)
		client->revoked = true;
	spin_unlock(&pool->lock);
	/* Registry removal must accompany revoke; an idle client has no pool
	 * entry, so the service's non-reusable identities reject it permanently.
	 */
	return kobox_linux_fs_service_unregister(pool->service, id);
}

int kobox_linux_fs_workers_client_busy(struct kobox_linux_fs_workers *pool, u64 id)
{
	struct fs_worker_client *client;
	bool busy;

	if (!fs_worker_owner(pool) || !id)
		return -EINVAL;
	spin_lock(&pool->lock);
	client = fs_worker_client_find(pool, id);
	busy = client && client->admitted;
	spin_unlock(&pool->lock);
	return busy;
}

int kobox_linux_fs_workers_close(struct kobox_linux_fs_workers *pool)
{
	if (!fs_worker_owner(pool))
		return -EINVAL;
	pool->closed = true;
	/* Stop retaining idle storage immediately. Draining releases free later
	 * metadata directly too, even if another VFS request remains blocked.
	 */
	fs_worker_cache_free(pool);
	return 0;
}

int kobox_linux_fs_workers_destroy(struct kobox_linux_fs_workers *pool)
{
	struct fs_worker_client *client, *next;
	unsigned int i;

	if (!fs_worker_owner(pool) || !pool->closed)
		return -EINVAL;
	if (pool->admitted || pool->control_admitted)
		return -EBUSY;
	for (i = 0; i < pool->config.workers; i++)
		kthread_stop_put(pool->tasks[i]);
	kthread_stop(pool->control_task);
	remove_wait_queue(&pool->owner_ready, &pool->owner_wait);
	fs_worker_contexts_free(pool);
	fs_worker_cache_free(pool);
	list_for_each_entry_safe(client, next, &pool->clients, link) {
		list_del(&client->link);
		kfree(client);
	}
	put_task_struct(pool->owner);
	kfree(pool->tasks);
	kfree(pool);
	return 0;
}
