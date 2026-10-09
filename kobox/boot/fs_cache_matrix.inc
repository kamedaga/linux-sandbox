/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/blkdev.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#if defined(KOBOX_FS_CACHE_DIAGNOSTIC) || defined(KOBOX_FS_RECYCLE_CONTROL)
#include <linux/part_stat.h>
#endif
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
#include "../../fs/ext4/ext4.h"
#endif
#include "fs_cache_matrix.h"

#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
struct matrix_counts { u64 cpu, switches, reads, writes, read_ns, write_ns, read_sectors, write_sectors; };
struct matrix_sample {
	struct matrix_counts counts;
	u64 wall;
	unsigned int phase, trial, sample;
	bool cold;
	s64 checkpoint_approx, checkpoint_sum;
};
#endif
#ifdef KOBOX_FS_CACHE_STAGES
struct matrix_stages {
	u64 calls, operations, ticks[5];
};
#endif
#ifdef KOBOX_FS_RECYCLE_CONTROL
struct matrix_recycle_counts { u64 reads, writes, switches; };
struct matrix_recycle_control {
	u64 operations, ns, max_ns, tail_ops, tail_ns;
	struct matrix_recycle_counts all, tail;
};
#endif
struct matrix_backend {
	struct fs_bench *bench;
	struct vfsmount *root;
	const char *fs;
	u64 checkpoints;
#ifdef KOBOX_FS_TAIL_TRACE
	struct matrix_tail_event {
		u64 begin, end, ns;
		unsigned int phase, trial, sample, scrub, cpu, end_cpu;
	} *tail_events;
	u64 tail_begin;
	unsigned int tail_cpu;
	unsigned int tail_count, tail_dropped;
#endif
#ifdef KOBOX_FS_RECYCLE_CONTROL
	struct matrix_recycle_control (*control)[2];
	struct matrix_recycle_counts control_previous;
#endif
#ifdef KOBOX_FS_CACHE_STAGES
	struct matrix_stages *stages, *active;
#endif
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
	struct matrix_counts previous;
	struct matrix_sample *samples;
	unsigned int sample_count, variant;
	s64 checkpoint_approx, checkpoint_sum;
#endif
};

#ifdef KOBOX_FS_RECYCLE_CONTROL
static bool matrix_recycle_io_probe(bool disk, bool cold, unsigned int phase)
{
	return disk && !cold && ((phase >= 4 && phase <= 7) || phase == 13);
}

static struct matrix_recycle_counts matrix_recycle_counts(struct matrix_backend *b)
{
	struct block_device *device = b->root->mnt_sb->s_bdev;

	/* Read-only counters: task_sched_runtime would take scheduler/IRQ locks
	 * and itself advance deferred work before the measured request. */
	return (struct matrix_recycle_counts){
		.reads = part_stat_read(device, ios[STAT_READ]),
		.writes = part_stat_read(device, ios[STAT_WRITE]),
		.switches = kobox_linux_fs_workers_switches(b->bench->workers),
	};
}

static void matrix_recycle_observe(void *context, unsigned int phase,
	const char *cache, unsigned int trial, unsigned int sample, int before, u64 ns)
{
	struct matrix_backend *b = context;
	bool disk = !strcmp(b->fs, "ext4"), cold = cache[0] == 'c';
	unsigned int index = disk ? (trial - 1) * 28 + phase * 2 + cold :
		84 + (trial - 1) * 14 + phase;
	unsigned int measured = cold ? sample : sample >= 8 ? sample - 8 : 0;
	unsigned int block = cold || phase == 13 ? 1 : 32;
	unsigned int scrub = (((measured / block + 1) / 2) + trial + phase) % 2;
	struct matrix_recycle_control *c = &b->control[index][scrub];
	bool probe = matrix_recycle_io_probe(disk, cold, phase);
#ifdef KOBOX_FS_TAIL_TRACE
	bool trace = disk && !cold && phase >= 4 && phase <= 6;
	u64 tail_end = before ? 0 : bench_ticks();

	if (trace && before && !sample) {
		kobox_task_tail_window(true);
		pr_info("FS_TAIL_WINDOW begin=1 phase=%s trial=%u\n", matrix_phases[phase], trial);
	}
#endif

	/* Both policies share code, fixture, cache and clock. Balanced blocks and
	 * ABBA blocks avoid assigning later journal progress or an odd/even
	 * request effect to one policy. Accounting stays outside the timer. */
	if (before) {
		kobox_linux_fs_request_recycle_control(scrub);
		if (probe) b->control_previous = matrix_recycle_counts(b);
#ifdef KOBOX_FS_TAIL_TRACE
		if (trace) { b->tail_cpu = raw_smp_processor_id(); b->tail_begin = bench_ticks(); }
#endif
	} else if (cold || sample >= 8) {
		bool tail = ns >= 100000;

		c->operations++;
		c->ns += ns;
		c->max_ns = max(c->max_ns, ns);
		if (tail) { c->tail_ops++; c->tail_ns += ns; }
#ifdef KOBOX_FS_TAIL_TRACE
		if (trace && tail) {
			if (b->tail_count < 64)
				b->tail_events[b->tail_count++] = (struct matrix_tail_event){
					.begin = b->tail_begin, .end = tail_end, .ns = ns,
					.phase = phase, .trial = trial, .sample = sample, .scrub = scrub,
					.cpu = b->tail_cpu, .end_cpu = raw_smp_processor_id()};
			else b->tail_dropped++;
		}
#endif
		if (probe) {
			struct matrix_recycle_counts now = matrix_recycle_counts(b);

#define CONTROL_DELTA(field) do { \
	u64 delta = now.field - b->control_previous.field; \
	c->all.field += delta; \
	if (tail) c->tail.field += delta; \
} while (0)
			CONTROL_DELTA(reads);
			CONTROL_DELTA(writes);
			CONTROL_DELTA(switches);
#undef CONTROL_DELTA
		}
	}
#ifdef KOBOX_FS_TAIL_TRACE
	if (trace && !before && sample == 1031) {
		kobox_task_tail_window(false);
		pr_info("FS_TAIL_WINDOW begin=0 phase=%s trial=%u\n", matrix_phases[phase], trial);
	}
#endif
}

static void matrix_recycle_dump(struct matrix_backend *b)
{
	for (unsigned int i = 0; i < 126; i++) {
		bool disk = i < 84;
		unsigned int local = disk ? i : i - 84;

		for (unsigned int scrub = 0; scrub < 2; scrub++) {
			struct matrix_recycle_control *c = &b->control[i][scrub];

			pr_info("FS_RECYCLE_CONTROL fs=%s phase=%s cache=%s trial=%u scrub=%u ops=%llu ns=%llu max_ns=%llu tail_ops=%llu tail_ns=%llu io_probed=%u reads=%llu writes=%llu switches=%llu tail_reads=%llu tail_writes=%llu tail_switches=%llu\n",
				disk ? "ext4" : "tmpfs", matrix_phases[disk ? local % 28 / 2 : local % 14],
				disk && local % 2 ? "cold" : "warm", local / (disk ? 28 : 14) + 1,
				scrub, c->operations, c->ns, c->max_ns, c->tail_ops, c->tail_ns,
				matrix_recycle_io_probe(disk, disk && local % 2, disk ? local % 28 / 2 : local % 14),
				c->all.reads, c->all.writes, c->all.switches,
				c->tail.reads, c->tail.writes, c->tail.switches);
		}
	}
}
#endif

#ifdef KOBOX_FS_CACHE_STAGES
static void matrix_stage_observe(void *context, unsigned int phase,
	const char *cache, unsigned int trial, unsigned int sample, int before, u64 ns)
{
	struct matrix_backend *b = context;
	bool disk = !strcmp(b->fs, "ext4");
	unsigned int index = disk ? (trial - 1) * 28 + phase * 2 + (cache[0] == 'c') :
		84 + (trial - 1) * 14 + phase;
	bool measured = cache[0] == 'c' || sample >= 8;

	/* Exclude fixture work, eviction and warmup, including cleanup calls
	 * made after the outer operation timer stops. No hot-path locks or logs. */
	b->active = before && measured ? &b->stages[index] : NULL;
	if (!before && measured)
		b->stages[index].operations++;
}

static void matrix_stage_dump(struct matrix_backend *b)
{
	for (unsigned int i = 0; i < 126; i++) {
		bool disk = i < 84;
		unsigned int local = disk ? i : i - 84;
		struct matrix_stages *s = &b->stages[i];

		pr_info("FS_CACHE_STAGES fs=%s phase=%s cache=%s trial=%u ops=%llu calls=%llu encode=%llu submit=%llu wait=%llu inspect=%llu recycle=%llu\n",
			disk ? "ext4" : "tmpfs", matrix_phases[disk ? local % 28 / 2 : local % 14],
			disk && local % 2 ? "cold" : "warm", local / (disk ? 28 : 14) + 1,
			s->operations, s->calls, s->ticks[0], s->ticks[1], s->ticks[2],
			s->ticks[3], s->ticks[4]);
	}
}
#endif

#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
static void matrix_sandbox_observe(void *context, unsigned int phase,
	const char *cache, unsigned int trial, unsigned int sample, int before, u64 ns)
{
	struct matrix_backend *b = context;
	struct block_device *device = b->root->mnt_sb->s_bdev;
	typeof(b->previous) now;

	if (strcmp(b->fs, "ext4") || (phase != 7 && phase != 12) ||
	    (cache[0] == 'w' && (sample < 8 || sample >= 12))) return;
	/* Counters bracket the timer, never sit inside VFS or its pool lock.
	 * Disk counters include background work on this root partition; they
	 * distinguish extra I/O from a no-I/O stall, not per-request attribution. */
	/* Reading task_sched_runtime takes runqueue/IRQ locks and can advance
	 * deferred work before the timer starts. This pass uses read-only disk
	 * and switch counters instead; runtime_probed=0 makes that explicit. */
	now.cpu = 0;
	now.switches = kobox_linux_fs_workers_switches(b->bench->workers);
	now.reads = part_stat_read(device, ios[STAT_READ]);
	now.writes = part_stat_read(device, ios[STAT_WRITE]);
	now.read_ns = part_stat_read(device, nsecs[STAT_READ]);
	now.write_ns = part_stat_read(device, nsecs[STAT_WRITE]);
	now.read_sectors = part_stat_read(device, sectors[STAT_READ]);
	now.write_sectors = part_stat_read(device, sectors[STAT_WRITE]);
	if (before) {
		journal_t *journal = EXT4_SB(b->root->mnt_sb)->s_journal;
		/* An approximate zero can hide per-CPU checkpoint references from
		 * the shrinker. Capture both counters outside the operation timer. */
		b->checkpoint_approx = journal ? percpu_counter_read_positive(&journal->j_checkpoint_jh_count) : -1;
		b->checkpoint_sum = journal ? percpu_counter_sum_positive(&journal->j_checkpoint_jh_count) : -1;
		b->previous = now;
		return;
	}
	BUG_ON(b->sample_count >= 96);
	struct matrix_sample *r = &b->samples[b->sample_count++];
	*r = (struct matrix_sample){.phase = phase, .trial = trial, .sample = sample,
		.cold = cache[0] == 'c', .wall = ns,
		.checkpoint_approx = b->checkpoint_approx, .checkpoint_sum = b->checkpoint_sum};
#define MATRIX_DELTA(field) r->counts.field = now.field - b->previous.field
	MATRIX_DELTA(cpu); MATRIX_DELTA(switches); MATRIX_DELTA(reads); MATRIX_DELTA(writes);
	MATRIX_DELTA(read_ns); MATRIX_DELTA(write_ns); MATRIX_DELTA(read_sectors); MATRIX_DELTA(write_sectors);
#undef MATRIX_DELTA
}
static void matrix_sandbox_dump(struct matrix_backend *b)
{
	/* UART writes between requests change IRQ/scheduler progress. Retain the
	 * bounded samples privately and print only after the entire probe run. */
	for (unsigned int i = 0; i < b->sample_count; i++) {
		struct matrix_sample *r = &b->samples[i];
		struct matrix_counts c = r->counts;
		pr_info("FS_COLD_DIAG runtime_probed=0 phase=%s cache=%s trial=%u sample=%u wall_ns=%llu cpu_ns=%llu switches=%llu reads=%llu writes=%llu read_ns=%llu write_ns=%llu read_sectors=%llu write_sectors=%llu checkpoint_approx=%lld checkpoint_sum=%lld\n",
			matrix_phases[r->phase], r->cold ? "cold" : "warm", r->trial, r->sample, r->wall,
			c.cpu, c.switches, c.reads, c.writes, c.read_ns, c.write_ns, c.read_sectors, c.write_sectors,
			r->checkpoint_approx, r->checkpoint_sum);
	}
}
#endif

static int64_t matrix_sandbox_call(void *context, struct matrix_call call, uint64_t *handle)
{
	struct matrix_backend *backend = context;
	struct fs_bench *bench = backend->bench;
	kb2_fs_request_t r = { .opcode = call.op, .generation = 1,
		.sequence = ++bench->sequence, .handle = call.handle,
		.flags = call.flags, .mode = call.mode, .length = call.length,
		.offset = call.offset, .mask = STATX_BASIC_STATS };
	kb2_fs_response_t reply;
	struct kobox_linux_fs_work *work;
	const void *response;
	void *cookie;
	size_t used, size = KB2_FILESYSTEM_REQUEST_SIZE;
	size_t capacity = KB2_FILESYSTEM_RESPONSE_SIZE + 4096;
	int error, operation;
#ifdef KOBOX_FS_CACHE_STAGES
	u64 stamps[6];
	struct matrix_stages *stages = backend->active;

	if (stages) stamps[0] = bench_ticks();
#endif

	if (call.path) {
		r.path = (kb2_fs_span_t){size, strlen(call.path) + 1};
		memcpy(bench->input + size, call.path, r.path.length);
		size += r.path.length;
	}
	if (call.second) {
		r.second_path = (kb2_fs_span_t){size, strlen(call.second) + 1};
		memcpy(bench->input + size, call.second, r.second_path.length);
		size += r.second_path.length;
	}
	if (call.op == KB2_FILESYSTEM_OP_PWRITE) {
		r.data = (kb2_fs_span_t){size, call.length};
		memset(bench->input + size, 0x5a, call.length);
		size += call.length;
	}
	error = kb2_fs_request_encode(bench->input, size, size, &r);
	if ((call.op == KB2_FILESYSTEM_OP_PREAD ||
	     call.op == KB2_FILESYSTEM_OP_GETDENTS64 ||
	     call.op == KB2_FILESYSTEM_OP_READLINKAT) && call.length > 4096)
		capacity = KB2_FILESYSTEM_RESPONSE_SIZE + call.length;
#ifdef KOBOX_FS_CACHE_STAGES
	if (stages) stamps[1] = bench_ticks();
#endif
	if (!error) error = kobox_linux_fs_workers_submit(bench->workers, 1,
		bench->input, size, capacity, NULL);
	if (error) return error;
#ifdef KOBOX_FS_CACHE_STAGES
	if (stages) stamps[2] = bench_ticks();
#endif
	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		error = kobox_linux_fs_workers_collect(bench->workers, &work,
			&cookie, &response, &used, &operation);
		if (error == -EAGAIN) { schedule(); __set_current_state(TASK_RUNNING); continue; }
		__set_current_state(TASK_RUNNING);
		break;
	}
	if (error) return error;
#ifdef KOBOX_FS_CACHE_STAGES
	if (stages) stamps[3] = bench_ticks();
#endif
	error = operation ?: kb2_fs_response_decode(response, used, &reply);
	if (!error && !kb2_fs_response_matches(&reply, &r)) error = -EIO;
	if (!error && reply.data_length > used - KB2_FILESYSTEM_RESPONSE_SIZE) error = -EIO;
	if (!error && reply.result >= 0 && call.verify &&
	    (reply.data_length != call.length ||
	     memchr_inv(response + KB2_FILESYSTEM_RESPONSE_SIZE, 0x5a, call.length)))
		error = -EIO;
	if (!error && reply.result >= 0 && call.op == KB2_FILESYSTEM_OP_GETDENTS64) {
		const u8 *data = response + KB2_FILESYSTEM_RESPONSE_SIZE;
		size_t offset = 0;
		unsigned int entries = 0;

		while (offset < reply.data_length) {
			u16 length;
			if (reply.data_length - offset < 20) { error = -EIO; break; }
			memcpy(&length, data + offset + 16, 2);
			if (length < 20 || length > reply.data_length - offset) { error = -EIO; break; }
			offset += length;
			entries++;
		}
		reply.result = entries;
	}
	if (!error && handle) *handle = reply.handle;
#ifdef KOBOX_FS_CACHE_STAGES
	if (stages) stamps[4] = bench_ticks();
#endif
	operation = kobox_linux_fs_workers_release(bench->workers, work);
#ifdef KOBOX_FS_CACHE_STAGES
	if (stages) {
		stamps[5] = bench_ticks();
		stages->calls++;
		for (unsigned int i = 0; i < 5; i++)
			stages->ticks[i] += stamps[i + 1] - stamps[i];
	}
#endif
	return error ?: operation ?: reply.result;
}

static void matrix_drop_pages(struct super_block *sb, void *unused)
{
	struct inode *inode, *previous = NULL;

	/* Same clean-page eviction policy as upstream drop_caches. Keep a ref
	 * across the unlocked invalidation and never iput while holding the list. */
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		/* Touching empty inodes would re-mark them referenced and distort
		 * the following slab reclaim. Match drop_caches' skip policy too. */
		if ((inode->i_state & (I_FREEING | I_WILL_FREE | I_NEW)) ||
		    (mapping_empty(inode->i_mapping) && !need_resched())) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
		iput(previous);
		previous = inode;
		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(previous);
}
static int matrix_sandbox_cold(void *context)
{
	struct matrix_backend *backend = context;
	struct super_block *sb = backend->root->mnt_sb;
	int error;

	/* sync_filesystem requires s_umount even with a pinned mount: the
	 * reference keeps it alive, but does not exclude a concurrent remount. */
	down_read(&sb->s_umount);
	error = sync_filesystem(sb);
	up_read(&sb->s_umount);
	if (error) return error;
	/* Commit completion can leave checkpoint references pinning metadata.
	 * Use the same full freeze/thaw as the native benchmark, outside timing;
	 * no pool request is in flight here and no normal service policy changes. */
	error = freeze_super(sb, FREEZE_HOLDER_USERSPACE, NULL);
	if (error) return error;
	error = thaw_super(sb, FREEZE_HOLDER_USERSPACE, NULL);
	if (error) return error;
	backend->checkpoints++;
	/* A reclaim pass can only clear the recently-referenced bit and free
	 * nothing, which makes drop_slab stop early in this small instance. Both
	 * benchmark backends repeat the same policy, outside operation timing. */
	for (unsigned int pass = 0; pass < 4; pass++) {
		lru_add_drain_all();
		iterate_supers(matrix_drop_pages, NULL);
		drop_slab();
	}
	return 0;
}
static uint64_t matrix_sandbox_clock(void *unused) { return ktime_get_ns(); }
static void matrix_sandbox_report(void *context, const char *phase, const char *cache,
	unsigned int trial, uint64_t ops, uint64_t ns)
{
	struct matrix_backend *b = context;
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
	if (b->variant) {
		pr_info("FS_CACHE_DIAGNOSTIC variant=%u fs=%s phase=%s cache=%s trial=%u ops=%llu ns=%llu\n",
			b->variant, b->fs, phase, cache, trial, ops, ns);
		return;
	}
#endif
	pr_info("FS_CACHE_MATRIX fs=%s phase=%s cache=%s trial=%u ops=%llu ns=%llu\n",
		b->fs, phase, cache, trial, ops, ns);
}
static int matrix_sandbox(struct fs_bench *bench, struct vfsmount *root)
{
	struct matrix_backend backend = { .bench = bench, .root = root, .fs = "ext4" };
	struct matrix *m = kzalloc(sizeof(*m), GFP_KERNEL);
	int error;
#ifdef KOBOX_FS_CACHE_STAGES
	u64 stage_ns, stage_ticks;
#endif

	if (!m) return -ENOMEM;
#ifdef KOBOX_FS_TAIL_TRACE
	backend.tail_events = kcalloc(64, sizeof(*backend.tail_events), GFP_KERNEL);
	if (!backend.tail_events) { kfree(m); return -ENOMEM; }
#endif
#ifdef KOBOX_FS_RECYCLE_CONTROL
	backend.control = kcalloc(126, sizeof(*backend.control), GFP_KERNEL);
	if (!backend.control) {
#ifdef KOBOX_FS_TAIL_TRACE
		kfree(backend.tail_events);
#endif
		kfree(m); return -ENOMEM;
	}
#endif
#ifdef KOBOX_FS_CACHE_STAGES
	backend.stages = kcalloc(126, sizeof(*backend.stages), GFP_KERNEL);
	if (!backend.stages) { kfree(m); return -ENOMEM; }
#endif
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
	backend.samples = kcalloc(96, sizeof(*backend.samples), GFP_KERNEL);
	if (!backend.samples) { kfree(m); return -ENOMEM; }
#endif
	*m = (struct matrix){.context = &backend, .call = matrix_sandbox_call,
		.cold = matrix_sandbox_cold, .clock = matrix_sandbox_clock,
		.report = matrix_sandbox_report};
#ifdef KOBOX_FS_RECYCLE_CONTROL
	m->observe = matrix_recycle_observe;
#endif
#ifdef KOBOX_FS_CACHE_STAGES
	m->observe = matrix_stage_observe;
	stage_ns = ktime_get_ns();
	stage_ticks = bench_ticks();
#endif
	pr_info("FS_CACHE_POLICY cold=checkpoint_then_drop drop_passes=4\n");
	error = matrix_run(m, "", 1);
	if (error) pr_err("FS_CACHE_MATRIX_FAIL fs=ext4 error=%d\n", error);
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
	if (!error) {
		backend.variant = 1;
		m->observe = matrix_sandbox_observe;
		error = matrix_run(m, "", 1);
		matrix_sandbox_dump(&backend);
	}
	if (!error) {
		backend.variant = 2;
		m->observe = NULL;
		error = matrix_run(m, "", 1);
	}
	backend.variant = 0;
#endif
	if (!error) {
		backend.fs = "tmpfs";
		error = matrix_run(m, "/tmp", 0);
		if (error) pr_err("FS_CACHE_MATRIX_FAIL fs=tmpfs error=%d\n", error);
	}
#ifdef KOBOX_FS_CACHE_DIAGNOSTIC
	kfree(backend.samples);
#endif
#ifdef KOBOX_FS_CACHE_STAGES
	if (!error) {
		/* Calibrate elapsed TSC ticks over the whole run, not CPU cycles
		 * or a guessed clock rate; no extra callbacks enter timed calls. */
		stage_ticks = bench_ticks() - stage_ticks;
		stage_ns = ktime_get_ns() - stage_ns;
		pr_info("FS_CACHE_STAGE_CLOCK ticks=%llu ns=%llu\n", stage_ticks, stage_ns);
		matrix_stage_dump(&backend);
	}
	kfree(backend.stages);
#endif
	pr_info("FS_CACHE_CHECKPOINT prepared=%llu\n", backend.checkpoints);
#ifdef KOBOX_FS_TAIL_TRACE
	for (unsigned int i = 0; i < backend.tail_count; i++) {
		struct matrix_tail_event *t = &backend.tail_events[i];
		pr_info("FS_TAIL_REQUEST phase=%s trial=%u sample=%u scrub=%u begin=%llu end=%llu ns=%llu cpu=%u end_cpu=%u\n",
			matrix_phases[t->phase], t->trial, t->sample, t->scrub, t->begin, t->end, t->ns, t->cpu, t->end_cpu);
	}
	pr_info("FS_TAIL_REQUEST_DONE count=%u dropped=%u\n", backend.tail_count, backend.tail_dropped);
	kobox_task_tail_window(false);
	kobox_task_tail_dump();
	kfree(backend.tail_events);
#endif
#ifdef KOBOX_FS_RECYCLE_CONTROL
	kobox_linux_fs_request_recycle_control(false);
	if (!error) matrix_recycle_dump(&backend);
	kfree(backend.control);
#endif
	kfree(m);
	return error;
}
