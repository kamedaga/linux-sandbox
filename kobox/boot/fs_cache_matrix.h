/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared workload, not a wire ABI. Both backends use this exact operation
 * sequence; cache preparation and mutation cleanup stay outside timing. */
#ifndef KOBOX_FS_CACHE_MATRIX_H
#define KOBOX_FS_CACHE_MATRIX_H
#include <kobox2/filesystem.h>

struct matrix_call {
	unsigned int op;
	uint64_t handle, flags, mode, length;
	int64_t offset;
	const char *path, *second;
	int verify;
};
struct matrix {
	void *context;
	int64_t (*call)(void *, struct matrix_call, uint64_t *);
	int (*cold)(void *);
	uint64_t (*clock)(void *);
	void (*report)(void *, const char *, const char *, unsigned int,
		uint64_t, uint64_t);
	void (*observe)(void *, unsigned int, const char *, unsigned int,
		unsigned int, int, uint64_t);
	char base[128], deep[256], deep_file[256], file[256], data[256], link[256], dir[256];
	char a[256], b[256];
	uint64_t data_fd;
};
static const char * const matrix_phases[] = {
	"stat_d0", "stat_d8", "open_close_d0", "open_close_d8",
	"create_close", "rename", "unlink", "mkdir_rmdir", "readdir_256",
	"readlink", "pread_4k", "pread_1m", "pwrite_4k", "pwrite_fsync",
};
#define MC(m, op_, ...) ((m)->call((m)->context, (struct matrix_call){ \
	.op = KB2_FILESYSTEM_OP_##op_, __VA_ARGS__ }, NULL))

static int matrix_open(struct matrix *m, const char *path, uint64_t flags,
	uint64_t *fd)
{
	return m->call(m->context, (struct matrix_call){
		.op = KB2_FILESYSTEM_OP_OPENAT2, .path = path,
		.flags = flags, .mode = flags & O_CREAT ? 0600 : 0 }, fd);
}
static int matrix_empty(struct matrix *m, const char *path)
{
	uint64_t fd;
	int error = matrix_open(m, path, O_CREAT | O_EXCL | O_RDWR, &fd);
	return error ?: MC(m, CLOSE, .handle = fd);
}
static int matrix_setup(struct matrix *m, const char *prefix)
{
	char path[256];
	uint64_t fd;
	int error;

	snprintf(m->base, sizeof(m->base), "%s/.kobox-cache-matrix", prefix);
	snprintf(m->deep, sizeof(m->deep), "%s/d/d/d/d/d/d/d/d", m->base);
	snprintf(m->deep_file, sizeof(m->deep_file), "%s/d/d/d/d/d/d/d/d/file", m->base);
	snprintf(m->file, sizeof(m->file), "%s/file", m->base);
	snprintf(m->data, sizeof(m->data), "%s/data", m->base);
	snprintf(m->link, sizeof(m->link), "%s/link", m->base);
	snprintf(m->dir, sizeof(m->dir), "%s/wide", m->base);
	snprintf(m->a, sizeof(m->a), "%s/a", m->base);
	snprintf(m->b, sizeof(m->b), "%s/b", m->base);
	/* Refuse a previous/foreign fixture rather than removing its contents. */
	error = MC(m, MKDIRAT, .path = m->base, .mode = 0700);
	if (error) return error;
	strcpy(path, m->base);
	for (unsigned int i = 0; i < 8; i++) {
		strcat(path, "/d");
		error = MC(m, MKDIRAT, .path = path, .mode = 0700);
		if (error) return error;
	}
	strcat(path, "/file");
	if ((error = matrix_empty(m, path)) || (error = matrix_empty(m, m->file)))
		return error;
	error = MC(m, SYMLINKAT, .path = "file", .second = m->link);
	if (error) return error;
	error = MC(m, MKDIRAT, .path = m->dir, .mode = 0700);
	for (unsigned int i = 0; !error && i < 256; i++) {
		snprintf(path, sizeof(path), "%s/wide/f%03u", m->base, i);
		error = matrix_empty(m, path);
	}
	if (error) return error;
	error = matrix_open(m, m->data, O_CREAT | O_EXCL | O_RDWR, &fd);
	if (error) return error;
	m->data_fd = fd;
	for (unsigned int i = 0; i < 256; i++)
		if (MC(m, PWRITE, .handle = fd, .length = 4096, .offset = i * 4096) != 4096)
			return -EIO;
	return MC(m, FSYNC, .handle = fd);
}

static int matrix_prepare(struct matrix *m, unsigned int phase)
{
	if (phase == 5 || phase == 6) return matrix_empty(m, m->a);
	if (phase == 4) return 0;
	return 0;
}
static int matrix_operation(struct matrix *m, unsigned int phase)
{
	uint64_t fd;
	int64_t result;
	int error, close_error;

	switch (phase) {
	case 0: case 1:
		return MC(m, STATX, .path = phase ? m->deep_file : m->file);
	case 2: case 3: case 4:
		error = matrix_open(m, phase == 4 ? m->a : phase == 3 ? m->deep_file : m->file,
			phase == 4 ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY, &fd);
		return error ?: MC(m, CLOSE, .handle = fd);
	case 5: return MC(m, RENAMEAT2, .path = m->a, .second = m->b);
	case 6: return MC(m, UNLINKAT, .path = m->a);
	case 7:
		error = MC(m, MKDIRAT, .path = m->a, .mode = 0700);
		return error ?: MC(m, UNLINKAT, .path = m->a, .flags = AT_REMOVEDIR);
	case 8:
		error = matrix_open(m, m->dir, O_RDONLY | O_DIRECTORY, &fd);
		if (error) return error;
		/* Count entries, not bytes: '.' and '..' are included, exactly once. */
		result = MC(m, GETDENTS64, .handle = fd, .length = 32768);
		close_error = MC(m, CLOSE, .handle = fd);
		return result < 0 ? result : result != 258 ? -EIO : close_error;
	case 9:
		result = MC(m, READLINKAT, .path = m->link, .length = 16);
		return result == 4 ? 0 : -EIO;
	case 10: case 11:
		result = MC(m, PREAD, .handle = m->data_fd,
			.length = phase == 10 ? 4096 : 1048576);
		return result == (phase == 10 ? 4096 : 1048576) ? 0 : -EIO;
	case 12: case 13:
		result = MC(m, PWRITE, .handle = m->data_fd, .length = 4096);
		if (result != 4096) return -EIO;
		return phase == 13 ? MC(m, FSYNC, .handle = m->data_fd) : 0;
	}
	return -EINVAL;
}
static int matrix_after(struct matrix *m, unsigned int phase)
{
	return phase == 4 ? MC(m, UNLINKAT, .path = m->a) :
		phase == 5 ? MC(m, UNLINKAT, .path = m->b) : 0;
}
static int matrix_measure(struct matrix *m, const char *cache, unsigned int phase,
	unsigned int trial)
{
	uint64_t total = 0, begin, elapsed;
	unsigned int samples = cache[0] == 'c' ? 12 : phase == 13 ? 24 : 1024;
	int error;

	/* Same timestamp boundary for both backends. Report raw time without
	 * subtracting a guessed timer cost. All setup, eviction and cleanup are out. */
	for (unsigned int i = 0; i < samples + (cache[0] == 'w' ? 8 : 0); i++) {
		error = matrix_prepare(m, phase);
		if (!error && cache[0] == 'c') error = m->cold(m->context);
		if (error) return error;
		if (m->observe) m->observe(m->context, phase, cache, trial, i, 1, 0);
		begin = m->clock(m->context);
		error = matrix_operation(m, phase);
		elapsed = m->clock(m->context) - begin;
		if (i >= (cache[0] == 'w' ? 8 : 0)) total += elapsed;
		if (m->observe) m->observe(m->context, phase, cache, trial, i, 0, elapsed);
		if (error) return error;
		error = matrix_after(m, phase);
		if (error) return error;
	}
	m->report(m->context, matrix_phases[phase], cache, trial, samples, total);
	return 0;
}
static int matrix_cleanup(struct matrix *m)
{
	char path[256];
	int error = MC(m, CLOSE, .handle = m->data_fd);
	if (!error) error = MC(m, UNLINKAT, .path = m->data);
	if (!error) error = MC(m, UNLINKAT, .path = m->file);
	if (!error) error = MC(m, UNLINKAT, .path = m->link);
	for (unsigned int i = 0; !error && i < 256; i++) {
		snprintf(path, sizeof(path), "%s/wide/f%03u", m->base, i);
		error = MC(m, UNLINKAT, .path = path);
	}
	if (!error) error = MC(m, UNLINKAT, .path = m->dir, .flags = AT_REMOVEDIR);
	if (!error) error = MC(m, UNLINKAT, .path = m->deep_file);
	strcpy(path, m->deep);
	for (unsigned int i = 0; !error && i < 8; i++) {
		error = MC(m, UNLINKAT, .path = path, .flags = AT_REMOVEDIR);
		*strrchr(path, '/') = 0;
	}
	return error ?: MC(m, UNLINKAT, .path = m->base, .flags = AT_REMOVEDIR);
}
static int matrix_run(struct matrix *m, const char *prefix, int disk)
{
	int error = matrix_setup(m, prefix);
	if (error) return error;
	for (unsigned int trial = 1; !error && trial <= 3; trial++)
		for (unsigned int phase = 0; !error && phase < 14; phase++)
			for (unsigned int variant = 0; !error && variant < (disk ? 2 : 1); variant++)
				error = matrix_measure(m, disk && (trial + variant) % 2 == 0 ? "cold" : "warm", phase, trial);
	/* Check the actual bytes outside timing; length-only success would hide
	 * corruption, while scanning every timed 1 MiB read would measure the test. */
	if (!error && MC(m, PREAD, .handle = m->data_fd, .length = 1048576,
		.verify = 1) != 1048576) error = -EIO;
	return error ?: matrix_cleanup(m);
}
#undef MC
#endif
