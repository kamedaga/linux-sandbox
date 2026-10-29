// SPDX-License-Identifier: GPL-2.0-only
#include "audio_port.h"
#include "../arch/x86_64/host_call.h"
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soundcard.h>
#include <linux/task_work.h>

struct audio_wait {
    struct list_head link;
    wait_queue_entry_t entry;
    wait_queue_head_t *head;
    struct audio_file *file;
};
struct audio_file {
    struct list_head link, waits;
    struct file *file;
    struct kobox_linux_audio_notify notify;
    uint64_t id;
    bool closing;
    int poll_error;
};
struct kobox_linux_audio_port {
    struct task_struct *owner;
    struct vfsmount *mount;
    struct list_head files;
    uint64_t sequence;
};
static struct audio_file *find_file(struct kobox_linux_audio_port *port, uint64_t id)
{
    struct audio_file *file;
    if (!port || port->owner != current) return NULL;
    list_for_each_entry(file, &port->files, link)
        if (file->id == id) return file;
    return NULL;
}
static int wake_audio(wait_queue_entry_t *entry, unsigned mode, int flags, void *key)
{
    struct audio_wait *wait = container_of(entry, struct audio_wait, entry);
    struct audio_file *file = wait->file;
    (void)mode; (void)flags; (void)key;
    if (!READ_ONCE(file->closing))
        (void)kobox_host_call(file->notify.notify(file->notify.context));
    return 0;
}
struct audio_poll { poll_table table; struct audio_file *file; };
static void register_wait(struct file *unused, wait_queue_head_t *head, poll_table *table)
{
    struct audio_file *file = container_of(table, struct audio_poll, table)->file;
    struct audio_wait *wait;
    (void)unused;
    list_for_each_entry(wait, &file->waits, link)
        if (wait->head == head) return;
    wait = kzalloc(sizeof(*wait), GFP_ATOMIC);
    if (!wait) { file->poll_error = -ENOMEM; return; }
    wait->head = head; wait->file = file;
    init_waitqueue_func_entry(&wait->entry, wake_audio);
    list_add_tail(&wait->link, &file->waits);
    add_wait_queue(head, &wait->entry);
}
int kobox_linux_audio_port_open(struct kobox_linux_audio_port **out)
{
    struct kobox_linux_audio_port *port;
    struct file_system_type *type;
    struct dentry *node;
    struct inode *parent;
    struct qstr name = QSTR_INIT("pcm", 3);
    int error;
    if (!out || *out || current->mm || in_interrupt() || irqs_disabled()) return -EINVAL;
    port = kzalloc(sizeof(*port), GFP_KERNEL);
    if (!port) return -ENOMEM;
    type = get_fs_type("tmpfs");
    if (!type) { kfree(port); return -ENODEV; }
    port->mount = kern_mount(type); put_filesystem(type);
    if (IS_ERR(port->mount)) { error = PTR_ERR(port->mount); goto free; }
    error = mnt_want_write(port->mount);
    if (error) goto unmount;
    parent = d_inode(port->mount->mnt_root);
    inode_lock(parent);
    node = lookup_one(mnt_idmap(port->mount), &name, port->mount->mnt_root);
    if (IS_ERR(node)) error = PTR_ERR(node);
    else {
        /* Linux's standard OSS PCM node, card zero. No peer selects a devt. */
        error = vfs_mknod(mnt_idmap(port->mount), parent, node,
            S_IFCHR | 0600, MKDEV(14, 3));
        dput(node);
    }
    inode_unlock(parent); mnt_drop_write(port->mount);
    if (error) goto unmount;
    INIT_LIST_HEAD(&port->files); port->owner = current;
    *out = port; return 0;
unmount:
    kern_unmount(port->mount);
free:
    kfree(port); return error;
}
int kobox_linux_audio_poll(struct kobox_linux_audio_port *port, uint64_t id)
{
    struct audio_file *file = find_file(port, id);
    struct audio_poll poll;
    unsigned mask;
    if (!file) return -EBADF;
    poll.file = file;
    init_poll_funcptr(&poll.table, register_wait);
    mask = vfs_poll(file->file, &poll.table);
    return file->poll_error ?: (int)mask;
}
int kobox_linux_audio_open(struct kobox_linux_audio_port *port, uint32_t flags,
    const struct kobox_linux_audio_notify *notify, uint64_t *handle)
{
    struct audio_file *file;
    int error;
    if (!port || port->owner != current || !notify || !notify->notify || !handle ||
        (flags & ~(O_WRONLY | O_NONBLOCK | O_CLOEXEC)) || !(flags & O_WRONLY)) return -EINVAL;
    if (port->sequence == U64_MAX) return -EOVERFLOW;
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (!file) return -ENOMEM;
    /* Never let an occupied PCM block the service's only request task. */
    file->file = file_open_root_mnt(port->mount, "pcm", O_WRONLY | O_NONBLOCK, 0);
    if (IS_ERR(file->file)) { error = PTR_ERR(file->file); kfree(file); return error; }
    if (!(flags & O_NONBLOCK)) file->file->f_flags &= ~O_NONBLOCK;
    file->notify = *notify; file->id = ++port->sequence;
    INIT_LIST_HEAD(&file->waits); list_add_tail(&file->link, &port->files);
    *handle = file->id; return 0;
}
int kobox_linux_audio_close(struct kobox_linux_audio_port *port, uint64_t id)
{
    struct audio_file *file = find_file(port, id);
    struct audio_wait *wait, *next;
    if (!file) return -EBADF;
    WRITE_ONCE(file->closing, true);
    list_for_each_entry_safe(wait, next, &file->waits, link) {
        /* Removal synchronizes with the callback before its native FD dies. */
        remove_wait_queue(wait->head, &wait->entry);
        list_del(&wait->link); kfree(wait);
    }
    list_del(&file->link); __fput_sync(file->file); kfree(file);
    task_work_run(); return 0;
}
void kobox_linux_audio_port_close(struct kobox_linux_audio_port *port)
{
    struct audio_file *file, *next;
    if (!port) return;
    BUG_ON(port->owner != current);
    list_for_each_entry_safe(file, next, &port->files, link)
        kobox_linux_audio_close(port, file->id);
    kern_unmount(port->mount); kfree(port);
}
long kobox_linux_audio_write(struct kobox_linux_audio_port *port, uint64_t id,
    const void *bytes, size_t size)
{
    struct audio_file *file = find_file(port, id);
    loff_t position = 0;
    if (!file) return -EBADF;
    if (!bytes || size > 4096) return -EINVAL;
    /* Native uaccess accepts this private copied buffer for the owning task.
     * The upstream OSS/ALSA implementation retains all PCM write semantics. */
    return file->file->f_op->write(file->file, (const char __user *)bytes,
        size, &position);
}
int kobox_linux_audio_ioctl(struct kobox_linux_audio_port *port, uint64_t id,
    uint32_t command, void *bytes, size_t size)
{
    struct audio_file *file = find_file(port, id);
    size_t expected;
    if (!file) return -EBADF;
    switch (command) {
    case SNDCTL_DSP_RESET: case SNDCTL_DSP_SYNC: case SNDCTL_DSP_POST:
        expected = 0; break;
    case SNDCTL_DSP_SPEED: case SNDCTL_DSP_STEREO: case SNDCTL_DSP_GETBLKSIZE:
    case SNDCTL_DSP_SETFMT: case SNDCTL_DSP_CHANNELS: case SNDCTL_DSP_SETFRAGMENT:
    case SNDCTL_DSP_GETFMTS: case SNDCTL_DSP_GETCAPS: case SNDCTL_DSP_GETTRIGGER:
    case SNDCTL_DSP_SETTRIGGER: case SNDCTL_DSP_GETODELAY:
        expected = sizeof(int); break;
    case SNDCTL_DSP_GETOSPACE: expected = sizeof(struct audio_buf_info); break;
    case SNDCTL_DSP_GETOPTR: expected = sizeof(struct count_info); break;
    default: return -ENOTTY;
    }
    if (size != expected || (size && !bytes)) return -EINVAL;
    return file->file->f_op->unlocked_ioctl(file->file, command, (unsigned long)bytes);
}

int kobox_linux_audio_flags(struct kobox_linux_audio_port *port, uint64_t id, uint64_t flags)
{
    struct audio_file *file = find_file(port, id);
    if (!file) return -EBADF;
    if (flags != U64_MAX) {
        spin_lock(&file->file->f_lock);
        file->file->f_flags = (file->file->f_flags & ~O_NONBLOCK) | (flags & O_NONBLOCK);
        spin_unlock(&file->file->f_lock);
    }
    return file->file->f_flags & (O_ACCMODE | O_NONBLOCK);
}
