// SPDX-License-Identifier: GPL-2.0-only

#include "user_process.h"
#include "../arch/x86_64/host_call.h"
#include "../arch/x86_64/user_layout.h"
#include "../mm/port.h"
#include "../task/user.h"

#include <linux/binfmts.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/ptrace.h>
#include <linux/resource.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/securebits.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/vmalloc.h>
#include <uapi/linux/mount.h>
#include "../../fs/internal.h"

#define PROCESS_FILES_MAX 64U
#define PROCESS_BYTES_MAX (64U << 20)
#define PROCESS_LOG_MAX (8U << 20)
#define PROCESS_VECTOR_MAX 64U
#define PROCESS_STRING_MAX 4096U
#define PROCESS_STRINGS_MAX (64U << 10)
#define PROCESS_CAPABILITIES (BIT_ULL(CAP_NET_ADMIN) | BIT_ULL(CAP_NET_RAW))

struct kobox_linux_user_process {
	struct kobox_linux_vm_host_operations operations;
	void *host;
	unsigned long start, length;
	struct kobox_linux_user_process_file *files;
	size_t file_count;
	char **argv, **envp;
	u64 capabilities;
	struct task_struct *owner, *task;
	struct mutex lock;
	pid_t pid;
	/* Published by the child; the owner retains these until final drain. */
	struct vfsmount *mount;
	struct file *log;
	struct mm_struct *mm; /* mm_count observer only, never mm_users */
	struct kobox_vm_space *orphan;
	bool bound, exec_ready, reaped, host_closed;
	int launch_error, wait_status;
	u64 replacement_sequence;
};

static bool valid_path(const char *path)
{
	size_t length, start = 1, i;

	if (!path || path[0] != '/')
		return false;
	length = strnlen(path, PROCESS_STRING_MAX + 1);
	if (length < 2 || length > PROCESS_STRING_MAX)
		return false;
	for (i = 1; i <= length; i++) {
		if (path[i] == '/' || !path[i]) {
			size_t size = i - start;

			if (!size || size > NAME_MAX ||
			    (size == 1 && path[start] == '.') ||
			    (size == 2 && path[start] == '.' && path[start + 1] == '.'))
				return false;
			start = i + 1;
		}
	}
	return true;
}

static bool reserved_path(const char *path)
{
	return !strcmp(path, "/sys") || !strncmp(path, "/sys/", 5) ||
	       !strcmp(path, "/dev") || !strncmp(path, "/dev/", 5);
}

static void free_vector(char **vector)
{
	size_t i;

	if (!vector)
		return;
	for (i = 0; vector[i]; i++)
		kfree(vector[i]);
	kfree(vector);
}

static int copy_vector(const char *const *input, char ***out, size_t *total)
{
	char **vector;
	size_t i;

	vector = kcalloc(PROCESS_VECTOR_MAX + 1, sizeof(*vector), GFP_KERNEL);
	if (!vector)
		return -ENOMEM;
	for (i = 0; input && input[i]; i++) {
		size_t length;

		if (i == PROCESS_VECTOR_MAX)
			goto too_big;
		length = strnlen(input[i], PROCESS_STRING_MAX + 1);
		if (length > PROCESS_STRING_MAX ||
		    length + 1 > PROCESS_STRINGS_MAX - *total)
			goto too_big;
		vector[i] = kstrndup(input[i], length, GFP_KERNEL);
		if (!vector[i]) {
			free_vector(vector);
			return -ENOMEM;
		}
		*total += length + 1;
	}
	*out = vector;
	return 0;
too_big:
	free_vector(vector);
	return -E2BIG;
}

static void free_inputs(struct kobox_linux_user_process *process)
{
	size_t i;

	for (i = 0; i < process->file_count; i++) {
		kfree(process->files[i].path);
		/* Private configuration may contain WPA credentials. Erase the
		 * supervisor's copy before returning it to the general allocator. */
		if (process->files[i].mode == 0600 && process->files[i].data)
			memzero_explicit((void *)process->files[i].data, process->files[i].size);
		kvfree(process->files[i].data);
	}
	kfree(process->files);
	free_vector(process->argv);
	free_vector(process->envp);
}

static int copy_inputs(struct kobox_linux_user_process *process,
	const struct kobox_linux_user_process_config *config)
{
	size_t total = 0, strings = 0, i, j;
	bool executable = false;
	int error;

	error = copy_vector(config->argv, &process->argv, &strings);
	if (error)
		return error;
	error = copy_vector(config->envp, &process->envp, &strings);
	if (error)
		return error;
	if (!process->argv[0] || !valid_path(process->argv[0]))
		return -EINVAL;
	process->files = kcalloc(config->file_count, sizeof(*process->files), GFP_KERNEL);
	if (!process->files)
		return -ENOMEM;
	for (i = 0; i < config->file_count; i++) {
		const struct kobox_linux_user_process_file *file = &config->files[i];
		struct kobox_linux_user_process_file *copy = &process->files[i];

		if (!valid_path(file->path) || reserved_path(file->path) ||
		    (file->mode != 0600 && file->mode != 0644 && file->mode != 0755) ||
		    (file->size && !file->data) || file->size > PROCESS_BYTES_MAX - total)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(file->path, process->files[j].path))
				return -EEXIST;
		copy->path = kstrdup(file->path, GFP_KERNEL);
		process->file_count = i + 1;
		if (!copy->path)
			return -ENOMEM;
		if (file->size) {
			copy->data = kvmemdup(file->data, file->size, GFP_KERNEL);
			if (!copy->data)
				return -ENOMEM;
		}
		copy->size = file->size;
		copy->mode = file->mode;
		total += file->size;
		if (!strcmp(file->path, process->argv[0]) && file->mode == 0755 && file->size)
			executable = true;
	}
	return executable ? 0 : -ENOEXEC;
}

/* All VFS setup runs on the child after mount/fs unsharing. No operation
 * changes the supervisor's fs_struct, mount namespace, files or credentials.
 */
static int mount_at(const char *name, const char *type,
	unsigned long flags, const char *options)
{
	struct path path;
	char *data = NULL;
	int error = kern_path(name, LOOKUP_DIRECTORY, &path);

	if (error)
		return error;
	if (options) {
		/* path_mount may write the last byte, even for short option strings. */
		data = (char *)get_zeroed_page(GFP_KERNEL);
		if (!data) {
			path_put(&path);
			return -ENOMEM;
		}
		strscpy(data, options, PAGE_SIZE);
	}
	error = path_mount(NULL, &path, type, flags, data);
	if (data)
		free_page((unsigned long)data);
	path_put(&path);
	return error;
}

static int make_directory(struct kobox_linux_user_process *process, const char *name)
{
	struct dentry *parent = dget(process->mount->mnt_root);
	const char *start = name;
	int error = 0;

	while (*start) {
		const char *slash = strchr(start, '/');
		struct qstr part = QSTR_INIT(start, slash ? slash - start : strlen(start));
		struct dentry *child;

		inode_lock(d_inode(parent));
		child = lookup_one(mnt_idmap(process->mount), &part, parent);
		if (!IS_ERR(child) && d_really_is_negative(child))
			child = vfs_mkdir(mnt_idmap(process->mount), d_inode(parent), child, 0755);
		inode_unlock(d_inode(parent));
		if (IS_ERR(child)) {
			error = PTR_ERR(child);
			break;
		}
		if (!d_is_dir(child)) {
			dput(child);
			error = -ENOTDIR;
			break;
		}
		dput(parent);
		parent = child;
		if (!slash)
			break;
		start = slash + 1;
	}
	dput(parent);
	return error;
}

static int install_file(struct kobox_linux_user_process *process,
	const struct kobox_linux_user_process_file *input)
{
	const char *name = input->path + 1, *slash = strrchr(name, '/');
	struct file *file;
	loff_t offset = 0;
	int error = 0;

	if (slash) {
		char *parent = kstrndup(name, slash - name, GFP_KERNEL);

		if (!parent)
			return -ENOMEM;
		error = make_directory(process, parent);
		kfree(parent);
		if (error)
			return error;
	}
	file = file_open_root_mnt(process->mount, name,
		O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, input->mode);
	if (IS_ERR(file))
		return PTR_ERR(file);
	while ((size_t)offset < input->size) {
		ssize_t written = kernel_write(file, (const char *)input->data + offset,
			input->size - (size_t)offset, &offset);

		if (written <= 0) {
			error = written < 0 ? written : -EIO;
			break;
		}
	}
	/* Finish deny_write_access teardown before upstream exec opens the ELF. */
	__fput_sync(file);
	return error;
}

static int install_device(struct kobox_linux_user_process *process,
	const char *name, unsigned int minor)
{
	struct file *directory;
	struct dentry *dentry;
	struct inode *parent;
	struct qstr part = QSTR_INIT(name, strlen(name));
	int error;

	directory = file_open_root_mnt(process->mount, "dev", O_PATH | O_DIRECTORY, 0);
	if (IS_ERR(directory))
		return PTR_ERR(directory);
	parent = file_inode(directory);
	inode_lock(parent);
	dentry = lookup_one(mnt_idmap(process->mount), &part, directory->f_path.dentry);
	if (IS_ERR(dentry)) {
		error = PTR_ERR(dentry);
	} else {
		error = vfs_mknod(mnt_idmap(process->mount), parent, dentry,
			S_IFCHR | 0666, MKDEV(1, minor));
		dput(dentry);
	}
	inode_unlock(parent);
	__fput_sync(directory);
	return error;
}

static int setup_root(struct kobox_linux_user_process *process)
{
	struct path root;
	size_t i;
	int error;

	error = ksys_unshare(CLONE_NEWNS | CLONE_FS);
	if (error)
		return error;
	error = unshare_fs_struct();
	if (error)
		return error;
	current->fs->umask = 0;
	error = mount_at("/", NULL, MS_REC | MS_PRIVATE, NULL);
	if (error)
		return error;
	error = kern_path("/", LOOKUP_DIRECTORY, &root);
	if (error)
		return error;
	error = mount_at("/", "tmpfs", MS_NOSUID, "size=64m,mode=0755");
	if (!error && !follow_down_one(&root))
		error = -EIO;
	if (error) {
		path_put(&root);
		return error;
	}
	process->mount = mntget(root.mnt);
	set_fs_root(current->fs, &root);
	set_fs_pwd(current->fs, &root);
	path_put(&root);
	error = make_directory(process, "sys");
	if (!error)
		error = make_directory(process, "dev");
	if (!error)
		error = make_directory(process, "run");
	if (!error)
		error = make_directory(process, "tmp");
	if (!error)
		error = mount_at("/sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (!error)
		error = install_device(process, "null", 3);
	if (!error)
		error = install_device(process, "urandom", 9);
	if (!error)
		error = install_device(process, "random", 8);
	for (i = 0; !error && i < process->file_count; i++)
		error = install_file(process, &process->files[i]);
	return error;
}

static int setup_files(struct kobox_linux_user_process *process)
{
	struct files_struct *files = current->files;
	struct file *log, *input;
	unsigned int next = 0;
	int error;

	if (atomic_read(&files->count) != 1)
		return -EBUSY;
	for (;;) {
		struct fdtable *table;
		unsigned int fd;

		spin_lock(&files->file_lock);
		table = files_fdtable(files);
		fd = find_next_bit(table->open_fds, table->max_fds, next);
		if (fd >= table->max_fds) {
			spin_unlock(&files->file_lock);
			break;
		}
		spin_unlock(&files->file_lock);
		close_fd(fd);
		next = fd + 1;
	}
	input = file_open_root_mnt(process->mount, "dev/null", O_RDONLY, 0);
	if (IS_ERR(input))
		return PTR_ERR(input);
	error = f_dupfd(0, input, 0);
	__fput_sync(input);
	if (error != 0)
		return error < 0 ? error : -EIO;
	log = file_open_root_mnt(process->mount, ".", O_TMPFILE | O_RDWR, 0600);
	if (IS_ERR(log))
		return PTR_ERR(log);
	smp_store_release(&process->log, log);
	error = f_dupfd(1, log, 0);
	if (error != 1)
		return error < 0 ? error : -EIO;
	error = f_dupfd(2, log, 0);
	if (error != 2)
		return error < 0 ? error : -EIO;
	current->signal->rlim[RLIMIT_FSIZE] = (struct rlimit){ PROCESS_LOG_MAX, PROCESS_LOG_MAX };
	return 0;
}

static int setup_credentials(struct kobox_linux_user_process *process)
{
	struct cred *cred = prepare_creds();
	struct group_info *groups;

	if (!cred)
		return -ENOMEM;
	groups = groups_alloc(0);
	if (!groups) {
		abort_creds(cred);
		return -ENOMEM;
	}
	set_groups(cred, groups);
	put_group_info(groups);
	cred->uid = cred->euid = cred->suid = cred->fsuid = GLOBAL_ROOT_UID;
	cred->gid = cred->egid = cred->sgid = cred->fsgid = GLOBAL_ROOT_GID;
	cred->cap_permitted = cred->cap_effective = cred->cap_inheritable =
		cred->cap_ambient = cred->cap_bset = (kernel_cap_t){ process->capabilities };
	/* NOROOT prevents uid 0 from regaining the bounding set implicitly.
	 * Ambient carries only the explicitly requested bits across ordinary exec.
	 * Lock all privilege-changing securebits; KEEP_CAPS itself remains clear.
	 */
	cred->securebits = SECBIT_NOROOT | SECBIT_NOROOT_LOCKED |
		SECBIT_NO_SETUID_FIXUP | SECBIT_NO_SETUID_FIXUP_LOCKED |
		SECBIT_KEEP_CAPS_LOCKED | SECBIT_NO_CAP_AMBIENT_RAISE |
		SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED;
	commit_creds(cred);
	task_set_no_new_privs(current);
	return 0;
}

static int launch(struct kobox_linux_user_process *process)
{
	struct kobox_vm_space *space;
	const struct cred *cred;
	unsigned long flags;
	int error;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	flush_signal_handlers(current, 1);
	sigemptyset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	error = setup_root(process);
	if (!error)
		error = setup_files(process);
	if (!error)
		error = setup_credentials(process);
	if (error)
		return error;
	error = kernel_execve(process->argv[0],
		(const char *const *)process->argv, (const char *const *)process->envp);
	if (error)
		return error;
	cred = current_cred();
	if (cred->cap_effective.val != process->capabilities ||
	    cred->cap_permitted.val != process->capabilities ||
	    cred->cap_inheritable.val != process->capabilities ||
	    cred->cap_ambient.val != process->capabilities ||
	    cred->cap_bset.val != process->capabilities ||
	    !current->mm || current->flags & PF_KTHREAD ||
	    current->mm->task_size != process->start + process->length ||
	    current_pt_regs()->ip < process->start ||
	    current_pt_regs()->ip >= process->start + process->length ||
	    current_pt_regs()->sp < process->start ||
	    current_pt_regs()->sp >= process->start + process->length)
		return -EPERM;
	mmgrab(current->mm);
	smp_store_release(&process->mm, current->mm);
	space = kobox_vm_space_bind(current->mm, process->host, &process->operations,
		process->start, process->length);
	if (IS_ERR(space))
		return PTR_ERR(space);
	WRITE_ONCE(process->bound, true);
	smp_store_release(&process->exec_ready, true);
	error = kobox_user_start(space, process->host);
	/* Successful entry never returns and exit_thread releases the binding.
	 * On a failed pre-entry handoff, retain it until the dead task lost its mm.
	 */
	WRITE_ONCE(process->orphan, space);
	return error;
}

static int child_entry(void *argument)
{
	struct kobox_linux_user_process *process = argument;
	int error = launch(process);

	WRITE_ONCE(process->launch_error, error ?: -EIO);
	do_exit(127 << 8);
}

static int owner_context(struct kobox_linux_user_process *process)
{
	if (!process || current != process->owner)
		return -EPERM;
	if (irqs_disabled() || in_atomic())
		return -EINVAL;
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_spawn(
	const struct kobox_linux_user_process_config *config,
	struct kobox_linux_user_process **out)
{
	struct kobox_linux_user_process *process;
	struct kernel_clone_args args = { .exit_signal = SIGCHLD, .fn = child_entry };
	const struct kobox_linux_vm_host_operations *ops;
	unsigned long flags;
	bool can_wait;
	int error;
	pid_t pid;

	if (!config || config->size != sizeof(*config) || !out || *out ||
	    !config->host_space || !config->files || !config->file_count ||
	    config->file_count > PROCESS_FILES_MAX || config->capability_mask & ~PROCESS_CAPABILITIES ||
	    config->start != KOBOX_X86_USER_START ||
	    config->length != KOBOX_X86_USER_END - KOBOX_X86_USER_START ||
	    !config->argv || current->mm || irqs_disabled() || in_atomic())
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_NAMESPACES))
		return -EOPNOTSUPP;
	if (!capable(CAP_SYS_ADMIN) || !uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	ops = config->vm_operations;
	if (!ops || ops->size != sizeof(*ops) || !ops->map || !ops->reset || !ops->close ||
	    !ops->start || !ops->enable_syscalls || !ops->snapshot || !ops->restore ||
	    !ops->write_fpregs || !ops->syscall_return || !ops->resume || !ops->event)
		return -EINVAL;
	spin_lock_irqsave(&current->sighand->siglock, flags);
	can_wait = current->sighand->action[SIGCHLD - 1].sa.sa_handler == SIG_DFL &&
		!(current->sighand->action[SIGCHLD - 1].sa.sa_flags & SA_NOCLDWAIT);
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	if (!can_wait || !thread_group_empty(current))
		return -EBUSY;
	process = kzalloc(sizeof(*process), GFP_KERNEL);
	if (!process)
		return -ENOMEM;
	mutex_init(&process->lock);
	process->owner = current;
	get_task_struct(process->owner);
	process->host = config->host_space;
	process->operations = *ops;
	process->operations.clone = NULL;
	process->start = config->start;
	process->length = config->length;
	process->capabilities = config->capability_mask;
	error = copy_inputs(process, config);
	if (error)
		goto free;
	args.fn_arg = process;
	pid = kernel_clone(&args);
	if (pid < 0) {
		error = pid;
		goto free;
	}
	process->pid = pid;
	process->task = find_get_task_by_vpid(pid);
	*out = process;
	/* Keep ownership if another, contract-violating waiter stole this child.
	 * No error path may free memory that the child could still reference.
	 */
	return process->task ? 0 : -ECHILD;
free:
	free_inputs(process);
	put_task_struct(process->owner);
	kfree(process);
	return error;
}

static bool dead_and_inactive(struct task_struct *task)
{
	if (READ_ONCE(task->__state) != TASK_DEAD)
		return false;
#ifdef CONFIG_SMP
	if (smp_load_acquire(&task->on_cpu))
		return false;
#endif
	return wait_task_inactive(task, TASK_DEAD) != 0;
}

/* Called with the owner's lock, only after the child cannot touch the handle.
 * After TASK_DEAD/inactivity, mm_users == 0 proves exit_thread retired its
 * native binding as well as the task's ordinary mm reference. Do not require
 * mm_count == 1: the supervisor or idle task may legitimately retain a lazy
 * active_mm reference, including while this very collect waits. Our observer
 * keeps the structure alive until destroy's mmdrop; Linux owns lazy ref drain.
 */
static int collect(struct kobox_linux_user_process *process)
{
	struct mm_struct *mm;
	int error, status = 0;

	if (!process->task)
		return -ECHILD;
	if (!dead_and_inactive(process->task))
		return -EAGAIN;
	if (READ_ONCE(process->task->mm) || READ_ONCE(process->task->files))
		return -EBUSY;
	if (!process->reaped) {
		error = kernel_wait(process->pid, &status);
		if (error != process->pid)
			return error < 0 ? error : -ECHILD;
		process->wait_status = status;
		process->reaped = true;
	}
	if (process->orphan) {
		error = kobox_vm_space_destroy(process->orphan);
		if (error)
			return error;
		process->orphan = NULL;
	}
	mm = smp_load_acquire(&process->mm);
	if (mm && atomic_read(&mm->mm_users))
		return -EAGAIN;
	if (!process->host_closed) {
		if (!process->bound) {
			error = kobox_host_call(process->operations.close(process->host));
			if (error)
				return error < 0 ? error : -EIO;
		}
		process->host_closed = true;
	}
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_status(struct kobox_linux_user_process *process,
	struct kobox_linux_user_process_status *status)
{
	int error = owner_context(process);

	if (error)
		return error;
	if (!status || status->size != sizeof(*status))
		return -EINVAL;
	mutex_lock(&process->lock);
	error = collect(process);
	*status = (struct kobox_linux_user_process_status) {
		.size = sizeof(*status), .pid = process->pid,
		.exec_ready = smp_load_acquire(&process->exec_ready),
		.exited = process->task && READ_ONCE(process->task->exit_state) != 0,
		.reaped = process->reaped, .host_closed = process->host_closed,
		.launch_error = READ_ONCE(process->launch_error),
		.wait_status = process->wait_status,
		.exit_code = process->reaped && !(process->wait_status & 0x7f) ?
			(process->wait_status >> 8) & 0xff : -1,
		.term_signal = process->reaped ? process->wait_status & 0x7f : 0,
	};
	mutex_unlock(&process->lock);
	return error == -EAGAIN ? 0 : error;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_read_log(struct kobox_linux_user_process *process,
	u64 offset, void *bytes, size_t capacity, size_t *read)
{
	struct file *log;
	loff_t position;
	ssize_t result;
	int error = owner_context(process);

	if (error)
		return error;
	if (!read || (capacity && !bytes) || offset > PROCESS_LOG_MAX ||
	    capacity > PROCESS_LOG_MAX - offset)
		return -EINVAL;
	*read = 0;
	mutex_lock(&process->lock);
	log = smp_load_acquire(&process->log);
	position = offset;
	result = log ? kernel_read(log, bytes, capacity, &position) : -EAGAIN;
	if (result >= 0)
		*read = result;
	mutex_unlock(&process->lock);
	return result < 0 ? result : 0;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_replace_file(struct kobox_linux_user_process *process,
	const char *path, const void *bytes, size_t length)
{
	struct file *file = NULL;
	struct dentry *parent = NULL, *target;
	char *temporary = NULL;
	const char *name;
	loff_t offset = 0;
	size_t i, prefix;
	int error = owner_context(process);

	if (error)
		return error;
	if (!valid_path(path) || !bytes || !length || length > 8192)
		return -EINVAL;
	mutex_lock(&process->lock);
	if (!process->mount || !process->task || !smp_load_acquire(&process->exec_ready) ||
	    READ_ONCE(process->task->__state) == TASK_DEAD) {
		error = -EPIPE;
		goto out;
	}
	for (i = 0; i < process->file_count; i++)
		if (process->files[i].mode == 0600 && !strcmp(path, process->files[i].path))
			break;
	if (i == process->file_count) {
		error = -EPERM;
		goto out;
	}
	name = strrchr(path, '/') + 1;
	prefix = name - path - 1; /* file_open_root_mnt takes a relative path. */
	temporary = kmalloc(prefix + 64, GFP_KERNEL);
	if (!temporary) {
		error = -ENOMEM;
		goto out;
	}
	memcpy(temporary, path + 1, prefix);
	snprintf(temporary + prefix, 64, ".kobox-config-%llu", ++process->replacement_sequence);
	file = file_open_root_mnt(process->mount, temporary,
		O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		file = NULL;
		goto out;
	}
	while ((size_t)offset < length) {
		ssize_t written = kernel_write(file, (const char *)bytes + offset,
			length - (size_t)offset, &offset);

		if (written <= 0) {
			error = written < 0 ? written : -EIO;
			goto remove;
		}
	}
	parent = dget_parent(file->f_path.dentry);
	/* Both names are in this pinned private directory. The opening task
	 * holds the process lock, not a VFS lock, throughout staging; acquire
	 * the directory lock only for the atomic publication itself. */
	lock_rename(parent, parent);
	if (file->f_path.dentry->d_parent != parent) {
		error = -ESTALE;
	} else {
		struct qstr part = QSTR_INIT(name, strlen(name));
		target = lookup_one(mnt_idmap(process->mount), &part, parent);
		if (IS_ERR(target)) {
			error = PTR_ERR(target);
		} else {
			struct renamedata rename = {.mnt_idmap = mnt_idmap(process->mount),
				.old_parent = parent, .old_dentry = file->f_path.dentry,
				.new_parent = parent, .new_dentry = target};
			error = vfs_rename(&rename);
			dput(target);
		}
	}
	unlock_rename(parent, parent);
	dput(parent);
	parent = NULL;
remove:
	if (error) {
		parent = dget_parent(file->f_path.dentry);
		inode_lock(d_inode(parent));
		if (file->f_path.dentry->d_parent == parent &&
		    !d_unhashed(file->f_path.dentry))
			vfs_unlink(mnt_idmap(process->mount), d_inode(parent), file->f_path.dentry, NULL);
		inode_unlock(d_inode(parent));
		dput(parent);
	}
out:
	if (file)
		__fput_sync(file);
	kfree(temporary);
	mutex_unlock(&process->lock);
	return error;
}

static int wait_deadline(struct kobox_linux_user_process *process, u32 timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int error;

	for (;;) {
		error = collect(process);
		if (error != -EAGAIN)
			return error;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		/* Yield to the exiting task and final switch-out; never busy-spin. */
		schedule_timeout_uninterruptible(1);
	}
}

__attribute__((visibility("default")))
int kobox_linux_user_process_wait(struct kobox_linux_user_process *process,
	u32 timeout_ms)
{
	int error = owner_context(process);

	if (error)
		return error;
	if (!timeout_ms || timeout_ms > 60000)
		return -EINVAL;
	mutex_lock(&process->lock);
	error = wait_deadline(process, timeout_ms);
	mutex_unlock(&process->lock);
	return error;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_stop(struct kobox_linux_user_process *process,
	u32 term_timeout_ms, u32 kill_timeout_ms)
{
	int error = owner_context(process);

	if (error)
		return error;
	if (!term_timeout_ms || term_timeout_ms > 60000 ||
	    !kill_timeout_ms || kill_timeout_ms > 60000)
		return -EINVAL;
	mutex_lock(&process->lock);
	error = collect(process);
	if (error != -EAGAIN)
		goto unlock;
	error = send_sig(SIGTERM, process->task, 1);
	if (error && error != -ESRCH)
		goto unlock;
	error = wait_deadline(process, term_timeout_ms);
	if (error != -ETIMEDOUT)
		goto unlock;
	error = send_sig(SIGKILL, process->task, 1);
	if (error && error != -ESRCH)
		goto unlock;
	error = wait_deadline(process, kill_timeout_ms);
unlock:
	mutex_unlock(&process->lock);
	return error;
}

__attribute__((visibility("default")))
int kobox_linux_user_process_destroy(struct kobox_linux_user_process *process)
{
	int error = owner_context(process);

	if (error)
		return error;
	mutex_lock(&process->lock);
	error = collect(process);
	if (error) {
		mutex_unlock(&process->lock);
		return error == -EAGAIN ? -EBUSY : error;
	}
	mutex_unlock(&process->lock);
	/* exit_thread may drop the last executable-file reference after the
	 * task's exit_task_work. Finish deferred fputs before releasing its mount.
	 * This global drain must never run while holding the owner's mutex.
	 */
	flush_delayed_fput();
	mutex_lock(&process->lock);
	if (process->log && file_count(process->log) != 1) {
		mutex_unlock(&process->lock);
		return -EBUSY;
	}
	if (process->log)
		__fput_sync(process->log);
	if (process->mount)
		mntput(process->mount);
	if (process->mm)
		mmdrop(process->mm);
	put_task_struct(process->task);
	put_task_struct(process->owner);
	free_inputs(process);
	mutex_unlock(&process->lock);
	kfree(process);
	return 0;
}
