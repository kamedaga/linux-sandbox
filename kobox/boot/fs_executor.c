// SPDX-License-Identifier: GPL-2.0-only
#include "fs_executor.h"
#include "fs_service.h"
#include "../arch/x86_64/host_call.h"

#include <linux/err.h>
#include <linux/fs_struct.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/overflow.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct kobox_linux_fs_lane {
	struct list_head link, ready;
	struct kobox_linux_fs_executors *pool;
	u64 client, bytes;
	void *binding;
	unsigned int active;
	bool intake, pending, paused, fault;
};

struct fs_executor {
	struct kobox_linux_fs_executors *pool;
	struct task_struct *task;
	struct fs_struct *scope;
	struct list_head idle;
	/* Storage is detached under lock before use. Any worker may evict idle
	 * storage, so the existing byte budget covers live and cached bytes. */
	struct kobox_linux_fs_request *storage;
	size_t storage_bytes;
	bool woken;
	int cpu;
};

struct kobox_linux_fs_executors {
	struct kobox_linux_fs_service *service;
	struct kobox_linux_fs_executor_config config;
	struct task_struct *owner;
	struct fs_executor *workers;
	struct list_head lanes, ready, idle;
	spinlock_t lock;
	wait_queue_head_t drained;
	u64 bytes, cached_bytes;
	unsigned int active;
	bool closed, burst, previous_burst;
#ifdef KOBOX_FS_INTERNAL_BENCH
	u64 taken, released, wakes;
#endif
};

static bool executor_owner(struct kobox_linux_fs_executors *pool)
{
	return pool && pool->owner == current;
}

/* All lane scheduling and lease transitions share this lock. Native queue
 * locks are acquired only in callbacks outside it; neither VFS nor allocation
 * may run under the scheduler lock. */
static void executor_ready(struct kobox_linux_fs_lane *lane)
{
	struct kobox_linux_fs_executors *pool = lane->pool;

	if (!pool->closed && !lane->paused && !lane->fault && lane->pending &&
	    !lane->intake && lane->active < pool->config.client_running &&
	    list_empty(&lane->ready))
		list_add_tail(&lane->ready, &pool->ready);
}

static struct task_struct *executor_wake(struct kobox_linux_fs_executors *pool)
{
	struct fs_executor *worker, *best = NULL;
	int rank, best_rank = -1, cpu = smp_processor_id();
	bool lone = pool->active <= 1 && !pool->previous_burst && !pool->burst;

	if (list_empty(&pool->ready))
		return NULL;
	list_for_each_entry(worker, &pool->idle, idle) {
		rank = lone ? (worker->cpu == cpu ? 3 : worker->cpu >= 0 ? 2 : 1) :
			(worker->cpu == cpu ? 1 : worker->cpu >= 0 ? 3 : 2);
		if (rank > best_rank) {
			best = worker;
			best_rank = rank;
		}
	}
	if (!best)
		return NULL;
	list_del_init(&best->idle);
	WRITE_ONCE(best->woken, true);
	return best->task;
}

static struct kobox_linux_fs_lane *executor_take(
	struct kobox_linux_fs_executors *pool)
{
	struct kobox_linux_fs_lane *lane;

	if (list_empty(&pool->ready))
		return NULL;
	lane = list_first_entry(&pool->ready, struct kobox_linux_fs_lane, ready);
	list_del_init(&lane->ready);
	lane->intake = true;
	lane->pending = false;
	lane->active++;
	pool->active++;
	pool->burst |= pool->active > 1;
	return lane;
}

/* Reserve before allocation/snapshot. A cache miss can evict other idle
 * buffers, but cannot confiscate a request currently executing or publishing.
 * An over-budget consumed item gets one pre-execution error, never a retry.
 */
static int executor_reserve(struct fs_executor *worker,
	struct kobox_linux_fs_lane *lane, size_t bytes,
	struct kobox_linux_fs_request **storage)
{
	struct kobox_linux_fs_executors *pool = worker->pool;
	struct kobox_linux_fs_request *discard;
	unsigned int i;
	int error;
	bool reserved = false;

	*storage = NULL;
	for (;;) {
		discard = NULL;
		spin_lock(&pool->lock);
		if (reserved)
			error = 0;
		else if (bytes > pool->config.client_bytes || bytes > pool->config.total_bytes)
			error = -ENOMEM;
		else if (bytes > pool->config.client_bytes - lane->bytes ||
			 bytes > pool->config.total_bytes - pool->bytes)
			error = -EAGAIN;
		else
			error = 0;
		if (error) {
			spin_unlock(&pool->lock);
			return error;
		}
		if (!reserved) {
			pool->bytes += bytes;
			lane->bytes += bytes;
			reserved = true;
		}
		if (worker->storage) {
			pool->cached_bytes -= worker->storage_bytes;
			if (worker->storage_bytes == bytes)
				*storage = worker->storage;
			else
				discard = worker->storage;
			worker->storage = NULL;
			worker->storage_bytes = 0;
		}
		if (pool->cached_bytes <= pool->config.total_bytes - pool->bytes) {
			spin_unlock(&pool->lock);
			kobox_linux_fs_request_destroy(discard);
			return 0;
		}
		/* Our matching cache was removed, so keep it across evictions. */
		if (!discard)
			for (i = 0; i < pool->config.workers; i++)
				if (pool->workers[i].storage) {
					discard = pool->workers[i].storage;
					pool->cached_bytes -= pool->workers[i].storage_bytes;
					pool->workers[i].storage = NULL;
					pool->workers[i].storage_bytes = 0;
					break;
				}
		spin_unlock(&pool->lock);
		kobox_linux_fs_request_destroy(discard);
	}
}

static void executor_intake_end(struct kobox_linux_fs_lane *lane, int taken)
{
	struct kobox_linux_fs_executors *pool = lane->pool;
	struct task_struct *wake;

	spin_lock(&pool->lock);
#ifdef KOBOX_FS_INTERNAL_BENCH
	if (taken > 0)
		pool->taken++;
#endif
	lane->intake = false;
	/* kick during take must survive an empty observation. The source reports
	 * whether another item exists, without a speculative worker wake. */
	lane->pending |= taken > 1;
	lane->fault |= taken < 0;
	executor_ready(lane);
	wake = executor_wake(pool);
	spin_unlock(&pool->lock);
	if (wake)
		wake_up_process(wake);
}

static struct kobox_linux_fs_lane *executor_run_item(struct fs_executor *worker,
	struct kobox_linux_fs_lane *lane)
{
	struct kobox_linux_fs_executors *pool = worker->pool;
	struct kobox_linux_fs_lane *next;
	struct kobox_linux_fs_intake item = {};
	struct kobox_linux_fs_request *request = NULL, *storage = NULL;
	void *response = NULL;
	size_t bytes = 0, used = 0;
	int taken, error;
	bool charged = false;
	bool drained, lone;

	taken = kobox_host_call(pool->config.take(lane->binding, &item));
	if (taken <= 0) {
		executor_intake_end(lane, taken);
		goto out_active;
	}
	if (check_add_overflow(item.input_size, item.response_capacity, &bytes))
		error = -EOVERFLOW;
	else
		error = executor_reserve(worker, lane, bytes, &storage);
	if (!error) {
		charged = true;
		error = kobox_linux_fs_request_prepare_owned(pool->service,
			lane->client, item.input, item.input_size, item.response_capacity,
			storage, &request, &response);
	}
	/* The snapshot and its credential/file pins are now immutable. Return
	 * intake before blocking VFS, so one sleeping client request does not
	 * serialize that client's other requests on a single executor. */
	if (item.snapshot_done)
		kobox_host_call((item.snapshot_done(item.cookie), 0));
	executor_intake_end(lane, taken);
	if (!error)
		error = kobox_linux_fs_request_execute_scope(request, worker->scope, &used);
	/* No Linux call belongs inside the native FP bank. Both native callbacks
	 * finish before restoring it once; the private response is still pinned,
	 * and the lane ref/budget survive subsequent Linux recycling. */
	kobox_host_call((pool->config.publish(item.cookie, response, used, error),
		pool->config.release(item.cookie), 0));
	if (request)
		kobox_linux_fs_request_recycle(request);
	storage = request;
out_active:
	spin_lock(&pool->lock);
	if (charged) {
		pool->bytes -= bytes;
		lane->bytes -= bytes;
		worker->storage = storage;
		worker->storage_bytes = storage ? bytes : 0;
		pool->cached_bytes += worker->storage_bytes;
	}
#ifdef KOBOX_FS_INTERNAL_BENCH
	if (taken > 0)
		pool->released++;
#endif
	lane->active--;
	pool->active--;
	executor_ready(lane);
	lone = !pool->burst;
	next = executor_take(pool);
	if (!next) {
		WRITE_ONCE(worker->woken, false);
		list_add_tail(&worker->idle, &pool->idle);
	}
	if (!pool->active && list_empty(&pool->ready)) {
		pool->previous_burst = pool->burst;
		pool->burst = false;
	}
	drained = !lane->active || (!pool->active && list_empty(&pool->ready));
#ifdef KOBOX_FS_INTERNAL_BENCH
	/* Like the worker control, diagnostics piggyback on required locks;
	 * counting must not add an extra hosted preempt/IRQ boundary per job. */
	if (drained)
		pool->wakes++;
#endif
	spin_unlock(&pool->lock);
	/* Only an explicit management/private benchmark drain waits here; no
	 * successful data completion is handed to an owner collect/release list. */
	if (drained) {
		if (!next && lone)
			__wake_up_sync(&pool->drained, TASK_NORMAL);
		else
			wake_up_all(&pool->drained);
	}
	return next;
}

static int executor_run(void *context)
{
	struct fs_executor *worker = context;
	struct kobox_linux_fs_executors *pool = worker->pool;
	struct kobox_linux_fs_lane *lane;

	for (;;) {
		set_current_state(TASK_IDLE);
		if (!READ_ONCE(worker->woken)) {
			if (kthread_should_stop())
				break;
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		spin_lock(&pool->lock);
		lane = executor_take(pool);
		if (!lane) {
			WRITE_ONCE(worker->woken, false);
			list_add_tail(&worker->idle, &pool->idle);
		}
		spin_unlock(&pool->lock);
		for (; lane; lane = executor_run_item(worker, lane))
			;
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

int kobox_linux_fs_executors_create(struct kobox_linux_fs_service *service,
	const struct kobox_linux_fs_executor_config *config,
	struct kobox_linux_fs_executors **result)
{
	struct kobox_linux_fs_executors *pool;
	unsigned int i, cpu;
	int error = -ENOMEM;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!service || !config || config->workers < 3 || config->client_running < 2 ||
	    config->client_running >= config->workers || !config->client_bytes ||
	    config->client_bytes > config->total_bytes / 2 || config->total_bytes > SIZE_MAX ||
	    !config->take || !config->publish || !config->release)
		return -EINVAL;
	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	pool->workers = kcalloc(config->workers, sizeof(*pool->workers), GFP_KERNEL);
	if (!pool->workers)
		goto out_pool;
	pool->service = service;
	pool->config = *config;
	pool->owner = current;
	get_task_struct(pool->owner);
	INIT_LIST_HEAD(&pool->lanes);
	INIT_LIST_HEAD(&pool->ready);
	INIT_LIST_HEAD(&pool->idle);
	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->drained);
	cpu = cpumask_first(cpu_online_mask);
	for (i = 0; i < config->workers; i++) {
		struct fs_executor *worker = &pool->workers[i];

		worker->pool = pool;
		worker->cpu = -1;
		INIT_LIST_HEAD(&worker->idle);
		worker->scope = copy_fs_struct(current->fs ?: init_task.fs);
		if (!worker->scope)
			goto out_workers;
		worker->task = kthread_create(executor_run, worker, "fs-executor/%u", i);
		if (IS_ERR(worker->task)) {
			error = PTR_ERR(worker->task);
			worker->task = NULL;
			goto out_workers;
		}
		get_task_struct(worker->task);
		if (cpu < nr_cpu_ids) {
			kthread_bind(worker->task, cpu);
			worker->cpu = cpu;
			cpu = cpumask_next(cpu, cpu_online_mask);
		}
		/* Publish idle under the same lock workers use at startup. */
		spin_lock(&pool->lock);
		list_add_tail(&worker->idle, &pool->idle);
		spin_unlock(&pool->lock);
		wake_up_process(worker->task);
	}
	*result = pool;
	return 0;
out_workers:
	for (i = 0; i < config->workers; i++) {
		if (pool->workers[i].task)
			kthread_stop_put(pool->workers[i].task);
		if (pool->workers[i].scope)
			free_fs_struct(pool->workers[i].scope);
	}
	put_task_struct(pool->owner);
	kfree(pool->workers);
out_pool:
	kfree(pool);
	return error;
}

int kobox_linux_fs_executors_register(struct kobox_linux_fs_executors *pool,
	u64 client, void *binding, struct kobox_linux_fs_lane **result)
{
	struct kobox_linux_fs_lane *lane, *existing;

	if (!executor_owner(pool) || !client || !binding || !result)
		return -EINVAL;
	*result = NULL;
	/* One lane per authenticated client in this prototype: additional lanes
	 * must share the same quota object, not multiply a principal's budget. */
	list_for_each_entry(existing, &pool->lanes, link)
		if (existing->client == client)
			return -EEXIST;
	lane = kzalloc(sizeof(*lane), GFP_KERNEL);
	if (!lane)
		return -ENOMEM;
	lane->pool = pool;
	lane->client = client;
	lane->binding = binding;
	INIT_LIST_HEAD(&lane->ready);
	spin_lock(&pool->lock);
	list_add_tail(&lane->link, &pool->lanes);
	spin_unlock(&pool->lock);
	*result = lane;
	return 0;
}

int kobox_linux_fs_executors_kick(struct kobox_linux_fs_lane *lane)
{
	struct kobox_linux_fs_executors *pool;
	struct task_struct *wake;
	int error = 0;

	if (!lane)
		return -EINVAL;
	pool = lane->pool;
	spin_lock(&pool->lock);
	if (pool->closed || lane->paused || lane->fault)
		error = -ESHUTDOWN;
	else {
		lane->pending = true;
		executor_ready(lane);
	}
	wake = error ? NULL : executor_wake(pool);
	spin_unlock(&pool->lock);
	if (wake)
		wake_up_process(wake);
	return error;
}

static bool executor_drained(struct kobox_linux_fs_executors *pool,
	struct kobox_linux_fs_lane *lane)
{
	bool drained;

	spin_lock(&pool->lock);
	drained = lane ? !lane->active : !pool->active && list_empty(&pool->ready);
	spin_unlock(&pool->lock);
	return drained;
}

int kobox_linux_fs_executors_wait_idle(struct kobox_linux_fs_executors *pool)
{
	struct kobox_linux_fs_lane *lane;
	int error = 0;

	if (!executor_owner(pool))
		return -EINVAL;
	wait_event(pool->drained, executor_drained(pool, NULL));
	spin_lock(&pool->lock);
	list_for_each_entry(lane, &pool->lanes, link)
		if (lane->fault)
			error = -EIO;
	spin_unlock(&pool->lock);
	return error;
}

int kobox_linux_fs_executors_pause(struct kobox_linux_fs_lane *lane)
{
	struct kobox_linux_fs_executors *pool;

	if (!lane || !executor_owner(lane->pool))
		return -EINVAL;
	pool = lane->pool;
	spin_lock(&pool->lock);
	lane->paused = true;
	list_del_init(&lane->ready);
	spin_unlock(&pool->lock);
	wait_event(pool->drained, executor_drained(pool, lane));
	return READ_ONCE(lane->fault) ? -EIO : 0;
}

int kobox_linux_fs_executors_resume(struct kobox_linux_fs_lane *lane)
{
	if (!lane || !executor_owner(lane->pool))
		return -EINVAL;
	spin_lock(&lane->pool->lock);
	lane->paused = false;
	spin_unlock(&lane->pool->lock);
	return kobox_linux_fs_executors_kick(lane);
}

int kobox_linux_fs_executors_unregister(struct kobox_linux_fs_lane *lane)
{
	struct kobox_linux_fs_executors *pool;

	if (!lane || !executor_owner(lane->pool))
		return -EINVAL;
	pool = lane->pool;
	spin_lock(&pool->lock);
	if (!lane->paused || lane->active) {
		spin_unlock(&pool->lock);
		return -EBUSY;
	}
	list_del(&lane->link);
	spin_unlock(&pool->lock);
	kfree(lane);
	return 0;
}

int kobox_linux_fs_executors_destroy(struct kobox_linux_fs_executors *pool)
{
	unsigned int i;

	if (!executor_owner(pool))
		return -EINVAL;
	/* Explicit unregister acknowledges every source binding first. A caller
	 * cannot mistake stopped Linux tasks for retired native mappings. */
	if (!list_empty(&pool->lanes))
		return -EBUSY;
	spin_lock(&pool->lock);
	pool->closed = true;
	spin_unlock(&pool->lock);
	for (i = 0; i < pool->config.workers; i++) {
		kthread_stop_put(pool->workers[i].task);
		kobox_linux_fs_request_destroy(pool->workers[i].storage);
		free_fs_struct(pool->workers[i].scope);
	}
	kfree(pool->workers);
	put_task_struct(pool->owner);
	kfree(pool);
	return 0;
}

#ifdef KOBOX_FS_INTERNAL_BENCH
int kobox_linux_fs_executors_check_drained(struct kobox_linux_fs_executors *pool)
{
	struct kobox_linux_fs_lane *lane;
	u64 cached = 0;
	unsigned int i;
	int error = 0;

	spin_lock(&pool->lock);
	if (pool->active || pool->bytes || !list_empty(&pool->ready) || pool->taken != pool->released)
		error = -EINVAL;
	list_for_each_entry(lane, &pool->lanes, link)
		if (lane->active || lane->intake || lane->bytes)
			error = -EINVAL;
	for (i = 0; i < pool->config.workers; i++) {
		struct fs_executor *worker = &pool->workers[i];

		if (!!worker->storage != !!worker->storage_bytes)
			error = -EINVAL;
		cached += worker->storage_bytes;
	}
	if (cached != pool->cached_bytes || cached > pool->config.total_bytes)
		error = -EINVAL;
	spin_unlock(&pool->lock);
	return error;
}

u64 kobox_linux_fs_executors_switches(struct kobox_linux_fs_executors *pool)
{
	u64 switches = READ_ONCE(pool->owner->nvcsw) + READ_ONCE(pool->owner->nivcsw);
	unsigned int i;

	for (i = 0; i < pool->config.workers; i++)
		switches += READ_ONCE(pool->workers[i].task->nvcsw) +
			READ_ONCE(pool->workers[i].task->nivcsw);
	return switches;
}

void kobox_linux_fs_executors_counts(struct kobox_linux_fs_executors *pool,
	u64 *taken, u64 *released, u64 *wakes)
{
	spin_lock(&pool->lock);
	*taken = pool->taken;
	*released = pool->released;
	*wakes = pool->wakes;
	spin_unlock(&pool->lock);
}
#endif
