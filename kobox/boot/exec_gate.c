// SPDX-License-Identifier: GPL-2.0-only

#include "exec_gate.h"
#include "exception.h"
#include "../task/user.h"
#include "../mm/port.h"
#include "../arch/x86_64/user_layout.h"

#include <linux/binfmts.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/stat.h>
#include <linux/mount.h>
#include <uapi/linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include "../../fs/internal.h"

struct exec_case {
	const struct kobox_exec_test *host;
	struct kobox_exec_report *report;
	struct vfsmount *mount;
	struct file *file;
	struct file *log;
	struct task_struct *child;
	struct kobox_vm_space *space;
	unsigned int cpu;
	bool adopted;
	int result;
};

static int empty_user_pgd(void)
{
	struct mm_struct *mm = mm_alloc();
	unsigned int index;
	int result = 0;

	if (!mm)
		return -ENOMEM;
	/* pgd_none() is always false when the five-level PGD is folded. */
	for (index = 0; index < PGD_KERNEL_START; index++) {
		if (native_pgd_val(mm->pgd[index])) {
			result = -EINVAL;
			break;
		}
	}
	mmput(mm);
	return result;
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		state->report->line = __LINE__; \
		return -EINVAL; \
	} \
} while (0)

static int launch(struct exec_case *state)
{
	const struct kobox_linux_vm_test *host = state->host->vm;
	const char *const argv[] = {"/client", "initial", state->cpu ? "1" : "0", NULL};
	const char *const envp[] = {"KOBOX=elf", NULL};
	struct path root = {.mnt = state->mount, .dentry = state->mount->mnt_root};
	unsigned long argc;
	int result;

	CHECK(!set_cpus_allowed_ptr(current, cpumask_of(state->cpu)));
	CHECK(!unshare_fs_struct());
	set_fs_root(current->fs, &root);
	set_fs_pwd(current->fs, &root);
	CHECK(f_dupfd(17, state->file, 0) == 17);
	CHECK(f_dupfd(19, state->file, 0) == 19);
	if (state->log) {
		CHECK(f_dupfd(1, state->log, 0) == 1);
		CHECK(f_dupfd(2, state->log, 0) == 2);
	}
	state->report->phase = 2;
	result = kernel_execve(argv[0], argv, envp);
	if (result)
		return result;
	state->report->phase = 3;
	CHECK(current->mm && !(current->flags & PF_KTHREAD));
	CHECK(current->mm->task_size == KOBOX_X86_USER_END);
	CHECK(current_pt_regs()->ip >= KOBOX_X86_USER_START &&
	      current_pt_regs()->ip < KOBOX_X86_USER_END);
	CHECK(current_pt_regs()->sp < KOBOX_X86_USER_END);
	CHECK(!get_user(argc, (unsigned long __user *)current_pt_regs()->sp) && argc == 3);
	state->space = kobox_vm_space_bind(current->mm, host->spaces[state->cpu],
					  host->operations, host->start, host->length);
	if (IS_ERR(state->space)) {
		result = PTR_ERR(state->space);
		state->space = NULL;
		return result;
	}
	state->report->entered++;
	state->result = 0;
	state->adopted = true;
	result = kobox_user_start(state->space, host->spaces[state->cpu]);
	state->adopted = false;
	return result;
}

static int child_entry(void *argument)
{
	struct exec_case *state = argument;

	get_task_struct(current);
	state->child = current;
	state->result = launch(state);
	do_exit(1 << 8);
}

static int install_parents(struct exec_case *state, const char *name)
{
	struct dentry *parent = dget(state->mount->mnt_root);
	const char *component = name, *slash;
	int result = 0;

	/* The fixture grants regular files only, with no dot or empty components. */
	if (*component == '/')
		component++;
	for (;;) {
		struct dentry *child;
		struct qstr part;

		slash = strchr(component, '/');
		part.name = component;
		part.len = slash ? slash - component : strlen(component);
		if (!part.len || part.len > NAME_MAX ||
		    (part.len == 1 && component[0] == '.') ||
		    (part.len == 2 && !memcmp(component, "..", 2))) {
			result = -EINVAL;
			break;
		}
		if (!slash)
			break;
		inode_lock(d_inode(parent));
		child = lookup_one(mnt_idmap(state->mount), &part, parent);
		if (!IS_ERR(child) && d_really_is_negative(child))
			child = vfs_mkdir(mnt_idmap(state->mount),
					  d_inode(parent), child, 0755);
		inode_unlock(d_inode(parent));
		if (IS_ERR(child)) {
			result = PTR_ERR(child);
			break;
		}
		if (!d_is_dir(child)) {
			dput(child);
			result = -ENOTDIR;
			break;
		}
		dput(parent);
		parent = child;
		component = slash + 1;
	}
	dput(parent);
	return result;
}

static int install_image(struct exec_case *state, const char *name,
			 const void *bytes, size_t size)
{
	struct file *file;
	loff_t position = 0;
	ssize_t written;
	int result;

	if (!name || (!bytes && size))
		return -EINVAL;
	result = mnt_want_write(state->mount);
	if (result)
		return result;
	result = install_parents(state, name);
	mnt_drop_write(state->mount);
	if (result)
		return result;

	file = file_open_root_mnt(state->mount, name,
				  O_CREAT | O_EXCL | O_WRONLY, 0755);
	if (IS_ERR(file))
		return PTR_ERR(file);
	written = kernel_write(file, bytes, size, &position);
	/* Drop the writer before upstream exec performs deny_write_access. */
	__fput_sync(file);
	return written == size ? 0 : written < 0 ? written : -EIO;
}

static int install_node(struct exec_case *state,
			const struct kobox_exec_node *node, const char *directory,
			bool replace)
{
	struct file *folder;
	struct inode *parent;
	struct dentry *base;
	struct qstr name = QSTR_INIT(node->name, strlen(node->name));
	struct iattr attributes = {
		.ia_valid = ATTR_MODE | ATTR_CTIME,
		.ia_mode = S_IFCHR | node->mode,
	};
	struct dentry *dentry;
	int result;

	folder = file_open_root_mnt(state->mount, directory, O_PATH | O_DIRECTORY, 0);
	if (IS_ERR(folder))
		return PTR_ERR(folder);
	base = folder->f_path.dentry;
	parent = d_inode(base);
	result = mnt_want_write(state->mount);
	if (result)
		goto release_folder;
	inode_lock(parent);
	dentry = lookup_one(mnt_idmap(state->mount), &name, base);
	if (!IS_ERR(dentry) && replace && d_really_is_positive(dentry)) {
		result = vfs_unlink(mnt_idmap(state->mount), parent, dentry, NULL);
		dput(dentry);
		dentry = result ? ERR_PTR(result) :
			lookup_one(mnt_idmap(state->mount), &name, base);
	}
	if (IS_ERR(dentry)) {
		result = PTR_ERR(dentry);
	} else {
		result = vfs_mknod(mnt_idmap(state->mount), parent, dentry,
				   S_IFCHR | node->mode,
				   MKDEV(node->major, node->minor));
		if (!result) {
			inode_lock_nested(d_inode(dentry), I_MUTEX_CHILD);
			result = notify_change(mnt_idmap(state->mount), dentry,
					       &attributes, NULL);
			inode_unlock(d_inode(dentry));
		}
		dput(dentry);
	}
	inode_unlock(parent);
	mnt_drop_write(state->mount);
release_folder:
	__fput_sync(folder);
	return result;
}

static int mount_at(const char *directory, const char *type,
		    unsigned long flags, const char *options)
{
	struct path path;
	char *data = NULL;
	int result = kern_path(directory, LOOKUP_DIRECTORY, &path);

	if (result)
		return result;
	if (options) {
		/* path_mount owns the last byte of its writable option page. */
		data = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!data) {
			result = -ENOMEM;
			goto release_path;
		}
		result = strscpy(data, options, PAGE_SIZE);
		if (result < 0)
			goto release_path;
	}
	result = path_mount(type, &path, type, flags, data);
release_path:
	kfree(data);
	path_put(&path);
	return result;
}

static int unmount_at(const char *directory)
{
	struct path path;
	int result = kern_path(directory, LOOKUP_MOUNTPOINT | LOOKUP_FOLLOW, &path);

	return result ?: path_umount(&path, 0);
}

static int mount_client_root(struct exec_case *state)
{
	struct path path;
	int result;

	result = do_mkdirat(AT_FDCWD, getname_kernel("/exec-client-root"), 0755);
	if (!result)
		result = mount_at("/exec-client-root", "tmpfs", 0, "mode=0755");
	if (!result)
		result = kern_path("/exec-client-root", LOOKUP_DIRECTORY, &path);
	if (result)
		return result;
	state->mount = path.mnt;
	dput(path.dentry);
	result = do_mkdirat(AT_FDCWD, getname_kernel("/exec-client-root/sys"), 0755);
	if (!result)
		result = mount_at("/exec-client-root/sys", "sysfs",
				    MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (!result)
		result = do_mkdirat(AT_FDCWD, getname_kernel("/exec-client-root/dev"), 0755);
	if (!result)
		result = do_mkdirat(AT_FDCWD, getname_kernel("/exec-client-root/dev/dri"), 0755);
	return result;
}

int kobox_linux_exec_replace_node(struct file *namespace_file,
				  const struct kobox_exec_node *node)
{
	struct exec_case state = {.mount = namespace_file->f_path.mnt};

	return install_node(&state, node, ".", true);
}

static int run_case(struct exec_case *state)
{
	struct kernel_clone_args args = {
		.exit_signal = SIGCHLD, .fn = child_entry, .fn_arg = state,
	};
	struct kobox_exec_result record;
	struct page *page;
	unsigned long deadline;
	loff_t position = 0;
	pid_t pid, waited;
	int status = 0, observed = 0;

	/*
	 * A normal VFS open gives this unlinked tmpfs file the positioned-I/O
	 * modes and accounting required when installing it in a user FD table.
	 */
	state->file = file_open_root_mnt(state->mount, ".", O_TMPFILE | O_RDWR, 0600);
	CHECK(!IS_ERR(state->file));
	if (state->host->file_count) {
		state->log = file_open_root_mnt(state->mount, ".", O_TMPFILE | O_RDWR, 0600);
		CHECK(!IS_ERR(state->log));
	}
	state->adopted = false;
	state->child = NULL;
	state->space = NULL;
	state->result = -EINVAL;
	pid = kernel_clone(&args);
	CHECK(pid > 0);
	if (state->host->observe)
		observed = state->host->observe(state->host->observe_context,
						state->file, state->cpu, pid);
	waited = kernel_wait(pid, &status);
	CHECK(waited == pid && state->child);
	deadline = jiffies + 5 * HZ;
	while (!wait_task_inactive(state->child, TASK_DEAD)) {
		CHECK(time_before(jiffies, deadline));
		schedule_timeout_uninterruptible(1);
	}
	state->report->program_status = status;
	if (state->log) {
		loff_t offset = 0;
		size_t used = strnlen(state->report->diagnostics,
				      sizeof(state->report->diagnostics) - 1);

		kernel_read(state->log, state->report->diagnostics + used,
			    sizeof(state->report->diagnostics) - 1 - used, &offset);
		__fput_sync(state->log);
		state->log = NULL;
	}
	CHECK(!state->child->mm && !state->child->files);
	put_task_struct(state->child);
	state->child = NULL;
	if (status && !state->report->user_failure.line) {
		position = KOBOX_EXEC_FAILURE_OFFSET;
		kernel_read(state->file, &state->report->user_failure,
			    sizeof(state->report->user_failure), &position);
		position = 0;
	}
	CHECK(!observed && !status && !state->result && state->adopted);
	state->space = NULL; /* exit_thread owns and has retired the binding. */
	CHECK(kernel_read(state->file, &record, sizeof(record), &position) == sizeof(record));
	CHECK(record.pid == pid && record.phase == 2 && record.cpu == state->cpu);
	CHECK(file_count(state->file) == 1);
	page = shmem_read_mapping_page(state->file->f_mapping, 0);
	CHECK(!IS_ERR(page));
	__fput_sync(state->file);
	state->file = NULL;
	lru_add_drain_all();
	CHECK(page_ref_count(page) == 1);
	put_page(page);
	state->report->exited++;
	state->report->reclaimed++;
	state->report->cpu_mask |= BIT(state->cpu);
	return 0;
}

__attribute__((visibility("default")))
int kobox_linux_exec_verify(const struct kobox_exec_test *host,
			   struct kobox_exec_report *report)
{
	struct exec_case *state;
	struct file_system_type *type;
	char options[] = "mode=0755";
	size_t index;
	int result;

	if (!host || host->size != sizeof(*host) || !host->vm || !host->image ||
	    (host->node_count && !host->nodes) ||
	    (host->file_count && !host->files) ||
	    !host->length || !report || report->size != sizeof(*report) ||
	    host->vm->start != KOBOX_X86_USER_START ||
	    host->vm->length != KOBOX_X86_USER_END - KOBOX_X86_USER_START ||
	    current->mm || task_pid_nr(current) != 1 || num_online_cpus() != 2)
		return -EINVAL;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->host = host;
	state->report = report;
	result = empty_user_pgd();
	if (result) {
		report->line = __LINE__;
		kfree(state);
		return result;
	}
	if (host->file_count) {
		result = mount_client_root(state);
		if (result) {
			report->line = __LINE__;
			report->result = result;
			return result;
		}
		goto install;
	}
	type = get_fs_type("tmpfs");
	if (!type) {
		kfree(state);
		return -ENODEV;
	}
	state->mount = vfs_kern_mount(type, 0, "elf-root", options);
	put_filesystem(type);
	if (IS_ERR(state->mount)) {
		result = PTR_ERR(state->mount);
		kfree(state);
		return result;
	}
install:
	report->phase = 1;
	result = install_image(state, "client", host->image, host->length);
	if (!result)
		result = install_image(state, "bad", "not an executable", 17);
	for (index = 0; !result && index < host->file_count; index++)
		result = install_image(state, host->files[index].path,
					host->files[index].data, host->files[index].length);
	for (index = 0; !result && index < host->node_count; index++) {
		result = install_node(state, &host->nodes[index], ".", false);
		if (!result && host->file_count)
			result = install_node(state, &host->nodes[index], "dev/dri", false);
	}
	kernel_sigaction(SIGCHLD, SIG_DFL);
	for_each_online_cpu(state->cpu) {
		if (result)
			break;
		result = run_case(state);
	}
	if (!result) {
		if (host->file_count) {
			/*
			 * The native binding drops its mm reference in exit_thread,
			 * after exit_task_work. Final executable/library fputs can
			 * therefore be queued to upstream delayed_fput. All clients
			 * are joined; drain these before attempting a normal umount.
			 */
			flush_delayed_fput();
			mntput(state->mount);
			result = unmount_at("/exec-client-root/sys");
			if (result)
				report->line = __LINE__;
			if (!result) {
				result = unmount_at("/exec-client-root");
				if (result)
					report->line = __LINE__;
			}
			if (!result) {
				result = do_rmdir(AT_FDCWD, getname_kernel("/exec-client-root"));
				if (result)
					report->line = __LINE__;
			}
		} else {
			kern_unmount(state->mount);
		}
		rcu_barrier();
		report->phase = 4;
		kfree(state);
	}
	/* Failure is terminal to this fixture process; do not free state that
	 * an incompletely stopped child could still reference.
	 */
	report->warnings = kobox_linux_exception_warnings();
	report->result = result ?: (report->warnings ? -EINVAL : 0);
	return report->result;
}
