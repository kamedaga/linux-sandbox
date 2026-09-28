// SPDX-License-Identifier: GPL-2.0-only

#include "drm_file.h"
#include "diagnostic.h"
#include "dma_host.h"
#include "drm_memory_policy.h"

#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/jiffies.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/task_work.h>
#include <linux/vmalloc.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode.h>
#include <drm/amdgpu_drm.h>
#include <drm/virtgpu_drm.h>
#include <kobox2/gpu_layout.h>
#include <kobox2/gpu_drm_amdgpu_layout.h>
#include <kobox2/amdgpu_info_bridge.h>
#include <kobox2/amdgpu_info_shape.h>
#include <kobox2/amdgpu_cs_shape.h>
#include <asm/ptrace.h>

long __x64_sys_ioctl(const struct pt_regs *regs);

struct kobox_linux_drm_file {
	struct vfsmount *mount;
	struct file *guard;
	struct task_struct *owner;
	struct files_struct *files;
	int fd;
#if defined(KOBOX_DRM_EXEC_PROFILE)
	/* Each file has one checked owner; separate counters avoid a lock on the
	 * measured path and distinguish clients without inspecting commands. */
	struct {
		u64 calls, copy_cycles, ioctl_cycles, fence_cycles, errors;
	} exec_profile[2];
#endif
};

#if defined(KOBOX_DRM_EXEC_PROFILE)
static u64 exec_profile_stamp(void)
{
	u32 low, high;
	asm volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
	return (u64)high << 32 | low;
}

static void exec_profile_record(struct kobox_linux_drm_file *file, u32 flags,
			       u64 start, u64 copied, u64 submitted, int result)
{
	unsigned int bucket = !!(flags & VIRTGPU_EXECBUF_FENCE_FD_OUT);
	u64 finished = exec_profile_stamp();
	u64 calls = ++file->exec_profile[bucket].calls;

	file->exec_profile[bucket].copy_cycles += copied - start;
	file->exec_profile[bucket].ioctl_cycles += submitted - copied;
	file->exec_profile[bucket].fence_cycles += finished - submitted;
	file->exec_profile[bucket].errors += result != 0;
	/* A few aggregate checkpoints suffice to separate copying/submission
	 * from the compatibility fence wait without per-command console traffic. */
	if (calls > 16384 || (calls != 1 && (calls < 256 || (calls & (calls - 1)))))
		return;
	kobox_linux_boot_diagnostic("KOBOX_DRM_SUBMIT file=%d fence_out=%u calls=%llu "
		"copy_cycles=%llu ioctl_cycles=%llu fence_export_cycles=%llu errors=%llu\n",
		file->fd, bucket, calls, file->exec_profile[bucket].copy_cycles,
		file->exec_profile[bucket].ioctl_cycles,
		file->exec_profile[bucket].fence_cycles,
		file->exec_profile[bucket].errors);
}
#endif

struct kobox_linux_drm_mapping {
	const struct kobox_linux_drm_mapping_operations *operations;
	struct module *module;
	void *private_mapping;
	struct task_struct *owner;
	struct files_struct *files;
};

static DEFINE_MUTEX(mapping_provider_lock);
static const struct kobox_linux_drm_mapping_operations *mapping_provider;
static struct module *mapping_provider_module;

int kobox_linux_drm_mapping_register(
	const struct kobox_linux_drm_mapping_operations *operations,
	struct module *owner)
{
	int error = 0;

	if (!operations || operations->size != sizeof(*operations) ||
	    !operations->get_size ||
	    !operations->map || !operations->release || !owner)
		return -EINVAL;
	mutex_lock(&mapping_provider_lock);
	if (mapping_provider)
		error = -EBUSY;
	else {
		mapping_provider = operations;
		mapping_provider_module = owner;
	}
	mutex_unlock(&mapping_provider_lock);
	return error;
}

void kobox_linux_drm_mapping_unregister(
	const struct kobox_linux_drm_mapping_operations *operations)
{
	mutex_lock(&mapping_provider_lock);
	if (mapping_provider == operations) {
		mapping_provider = NULL;
		mapping_provider_module = NULL;
	}
	mutex_unlock(&mapping_provider_lock);
}

static bool valid_context(void)
{
	return !in_interrupt() && !irqs_disabled() && !current->mm &&
	       ((current->flags & PF_KTHREAD) || current->pid == 1) &&
	       current->files;
}

static int check_owner(struct kobox_linux_drm_file *file)
{
	struct file *installed;
	bool matches;

	if (!file || !valid_context() || current != file->owner ||
	    current->files != file->files)
		return -EPERM;
	installed = fget(file->fd);
	matches = installed && installed == file->guard;
	if (installed)
		fput(installed);
	return matches ? 0 : -EBADF;
}

static int drm_event_observe(wait_queue_entry_t *entry, unsigned int mode,
			     int flags, void *key)
{
	struct kobox_linux_drm_event_observer *observer =
		container_of(entry, struct kobox_linux_drm_event_observer, wait);

	(void)mode;
	(void)flags;
	(void)key;
	if (READ_ONCE(observer->attached))
		observer->notify(observer->context);
	/* This is a non-exclusive observer, never the waitqueue's consumer. */
	return 0;
}

int kobox_linux_drm_event_attach(struct kobox_linux_drm_file *file,
				 struct kobox_linux_drm_event_observer *observer,
				 void (*notify)(void *context), void *context)
{
	struct drm_file *drm_file;
	int result = check_owner(file);

	if (result)
		return result;
	if (!observer || observer->attached || observer->file || !notify)
		return -EINVAL;
	drm_file = file->guard->private_data;
	if (!drm_file)
		return -ENODEV;
	observer->file = file;
	observer->context = context;
	observer->notify = notify;
	init_waitqueue_func_entry(&observer->wait, drm_event_observe);
	WRITE_ONCE(observer->attached, true);
	add_wait_queue(&drm_file->event_wait, &observer->wait);
	return 0;
}

int kobox_linux_drm_event_detach(
	struct kobox_linux_drm_event_observer *observer)
{
	struct drm_file *drm_file;
	int result;

	if (!observer || !observer->attached || !observer->file)
		return -EINVAL;
	result = check_owner(observer->file);
	if (result)
		return result;
	drm_file = observer->file->guard->private_data;
	if (!drm_file)
		return -ENODEV;
	WRITE_ONCE(observer->attached, false);
	/* remove_wait_queue synchronizes with an in-flight wake callback before
	 * the file or the fixed observer slot can be retired. */
	remove_wait_queue(&drm_file->event_wait, &observer->wait);
	memset(observer, 0, sizeof(*observer));
	return 0;
}

static int create_node(struct vfsmount *mount, dev_t device)
{
	struct inode *parent = d_inode(mount->mnt_root);
	struct qstr name = QSTR_INIT("render", 6);
	struct dentry *node;
	int result;

	result = mnt_want_write(mount);
	if (result)
		return result;
	inode_lock(parent);
	node = lookup_one(mnt_idmap(mount), &name, mount->mnt_root);
	if (IS_ERR(node)) {
		result = PTR_ERR(node);
	} else {
		result = vfs_mknod(mnt_idmap(mount), parent, node,
				   S_IFCHR | 0600, device);
		dput(node);
	}
	inode_unlock(parent);
	mnt_drop_write(mount);
	return result;
}

int kobox_linux_drm_distinct(struct kobox_linux_drm_file *left,
			    struct kobox_linux_drm_file *right)
{
	int result = check_owner(left);

	if (!result)
		result = check_owner(right);
	if (result)
		return result;
	if (left == right || left->fd == right->fd || left->guard == right->guard ||
	    left->guard->private_data == right->guard->private_data)
		return -EUCLEAN;
	return 0;
}

int kobox_linux_drm_open(dev_t device, u32 node_type,
			 struct kobox_linux_drm_file **out)
{
	struct kobox_linux_drm_file *file;
	struct file_system_type *type;
	struct file *opened;
	int result;

	if (!out || *out || MAJOR(device) != DRM_MAJOR ||
	    (node_type != KB2_GPU_NODE_PRIMARY &&
	     node_type != KB2_GPU_NODE_RENDER))
		return -EINVAL;
	if (!valid_context())
		return -EPERM;
	file = kzalloc(sizeof(*file), GFP_KERNEL);
	if (!file)
		return -ENOMEM;
	type = get_fs_type("tmpfs");
	if (!type) {
		result = -ENODEV;
		goto free_file;
	}
	file->mount = kern_mount(type);
	put_filesystem(type);
	if (IS_ERR(file->mount)) {
		result = PTR_ERR(file->mount);
		goto free_file;
	}
	result = create_node(file->mount, device);
	if (result)
		goto unmount;
	opened = file_open_root_mnt(file->mount, "render", O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(opened)) {
		result = PTR_ERR(opened);
		goto unmount;
	}
	/* The devt is trusted; still verify the actual DRM file before publishing. */
	if (!opened->private_data ||
	    (node_type == KB2_GPU_NODE_RENDER &&
	     !drm_is_render_client(opened->private_data)) ||
	    (node_type == KB2_GPU_NODE_PRIMARY &&
	     !drm_is_primary_client(opened->private_data))) {
		result = -EACCES;
		goto close_opened;
	}
	result = get_unused_fd_flags(O_CLOEXEC);
	if (result < 0)
		goto close_opened;
	file->fd = result;
	file->guard = get_file(opened);
	file->owner = current;
	get_task_struct(file->owner);
	file->files = current->files;
	fd_install(file->fd, opened);
	*out = file;
	return 0;

close_opened:
	__fput_sync(opened);
unmount:
	kern_unmount(file->mount);
free_file:
	kfree(file);
	return result;
}

/* Only the two typed callers below can select commands and construct args.
 * Native uaccess accepts these private buffers; it does not validate peers.
 */
static int private_ioctl(struct kobox_linux_drm_file *file, unsigned int cmd,
			 void *argument)
{
	struct pt_regs regs = {
		.di = file->fd,
		.si = cmd,
		.dx = (unsigned long)argument,
	};

	int result = __x64_sys_ioctl(&regs);

	/* Hosted PID 1 never returns through Linux's exit-to-user boundary.
	 * GEM_CLOSE can queue final dma-buf/shmem fputs on its task work list.
	 * Drain only this task, after the ioctl has released its driver locks. */
	task_work_run();
	return result;
}

int kobox_linux_drm_version(struct kobox_linux_drm_file *file,
			    const size_t capacity[3],
			    struct kobox_linux_drm_version *out)
{
	struct kobox_linux_drm_version *result;
	struct drm_version argument = {0};
	int error;

	if (!capacity || !out || capacity[0] > sizeof(out->name) ||
	    capacity[1] > sizeof(out->date) ||
	    capacity[2] > sizeof(out->description))
		return -EINVAL;
	error = check_owner(file);
	if (error)
		return error;
	result = kzalloc(sizeof(*result), GFP_KERNEL);
	if (!result)
		return -ENOMEM;
	argument.name_len = capacity[0];
	argument.date_len = capacity[1];
	argument.desc_len = capacity[2];
	argument.name = capacity[0] ? result->name : NULL;
	argument.date = capacity[1] ? result->date : NULL;
	argument.desc = capacity[2] ? result->description : NULL;
	error = private_ioctl(file, DRM_IOCTL_VERSION, &argument);
	if (!error) {
		result->major = argument.version_major;
		result->minor = argument.version_minor;
		result->patchlevel = argument.version_patchlevel;
		result->name_length = argument.name_len;
		result->date_length = argument.date_len;
		result->description_length = argument.desc_len;
		*out = *result;
	}
	kfree(result);
	return error;
}

int kobox_linux_drm_get_cap(struct kobox_linux_drm_file *file, u64 capability,
			   u64 *value)
{
	struct drm_get_cap argument = {.capability = capability};
	int result;

	if (!value)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_GET_CAP, &argument);
	if (!result)
		*value = argument.value;
	return result;
}

int kobox_linux_drm_amdgpu_info(struct kobox_linux_drm_file *file,
			       u32 query, const u32 *selectors,
			       void *output, size_t size)
{
	struct drm_amdgpu_info argument = {0};
	struct drm_amdgpu_info_device device = {0};
	struct drm_amdgpu_info_hw_ip hw_ip = {0};
	struct drm_amdgpu_info_firmware firmware = {0};
	struct drm_amdgpu_memory_info memory = {0};
	struct drm_amdgpu_info_vram_gtt vram = {0};
	struct drm_amdgpu_info_video_caps video = {0};
	struct kb2_amdgpu_info_shape shape;
	u64 private_value = 0;
	u32 registers[128] = {0};
	u32 selector, instance;
	void *native = &private_value;
	size_t native_size = size;
	int result;

	if (!output || !selectors ||
	    !kb2_amdgpu_info_shape(query, selectors, &shape) ||
	    size != shape.bytes)
		return -EINVAL;
	static_assert(AMDGPU_HW_IP_NUM == 10);
	static_assert(sizeof(firmware) == KB2_GPU_DRM_AMDGPU_RECORD_INFO_FIRMWARE_SIZE);
	static_assert(sizeof(memory) == KB2_GPU_DRM_AMDGPU_RECORD_INFO_MEMORY_SIZE);
	static_assert(sizeof(vram) == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VRAM_GTT_SIZE);
	static_assert(sizeof(video) == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VIDEO_CAPS_SIZE);
	selector = selectors[0];
	instance = selectors[1];

	if (query == AMDGPU_INFO_DEV_INFO &&
	    size == KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_SIZE) {
		if (selector || instance)
			return -EINVAL;
		native = &device;
		native_size = sizeof(device);
	} else if (query == AMDGPU_INFO_HW_IP_INFO &&
		   size == KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP_SIZE) {
		native = &hw_ip;
		native_size = sizeof(hw_ip);
	} else if (query == AMDGPU_INFO_FW_VERSION) {
		native = &firmware;
	} else if (query == AMDGPU_INFO_MEMORY) {
		native = &memory;
	} else if (query == AMDGPU_INFO_VRAM_GTT) {
		native = &vram;
	} else if (query == AMDGPU_INFO_VIDEO_CAPS) {
		native = &video;
	} else if (query == AMDGPU_INFO_MAX_IBS) {
		native = registers;
	} else if (query == AMDGPU_INFO_READ_MMR_REG) {
		if (!instance || instance > ARRAY_SIZE(registers) ||
		    size != instance * sizeof(u32) || selector > U32_MAX - (instance - 1))
			return -EINVAL;
		native = registers;
	} else if ((size != sizeof(u32) && size != sizeof(u64)) || instance) {
		return -EINVAL;
	}
	result = check_owner(file);
	if (result)
		return result;
	/* The upstream ioctl writes only to this Linux-owned buffer. A native
	 * client pointer is never installed in the driver-visible argument. */
	argument.return_pointer = (uintptr_t)native;
	argument.return_size = native_size;
	argument.query = query;
	if (query == AMDGPU_INFO_HW_IP_COUNT ||
	    query == AMDGPU_INFO_HW_IP_INFO) {
		argument.query_hw_ip.type = selector;
		argument.query_hw_ip.ip_instance = instance;
	} else if (query == AMDGPU_INFO_READ_MMR_REG) {
		argument.read_mmr_reg.dword_offset = selector;
		argument.read_mmr_reg.count = instance;
		argument.read_mmr_reg.instance = selectors[2];
		argument.read_mmr_reg.flags = selectors[3];
	} else if (query == AMDGPU_INFO_FW_VERSION) {
		argument.query_fw.fw_type = selector;
		argument.query_fw.ip_instance = instance;
		argument.query_fw.index = selectors[2];
	} else if (query == AMDGPU_INFO_VIDEO_CAPS) {
		argument.video_cap.type = selector;
	} else if (query == AMDGPU_INFO_SENSOR)
		argument.sensor_info.type = selector;
	else if (selector)
		return -EINVAL;
	result = private_ioctl(file, DRM_IOCTL_AMDGPU_INFO, &argument);
	if (result)
		return result;
	if (native == &device) {
		memset(output, 0, size);
#define PACK_DEVICE(member, wire) \
		memcpy((u8 *)output + \
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_##wire##_OFFSET, \
			&device.member, sizeof(device.member));
		KB2_AMDGPU_DEVICE_FIELDS(PACK_DEVICE)
#undef PACK_DEVICE
	} else if (native == &hw_ip) {
		memset(output, 0, size);
#define PACK_HW_IP(member, wire) \
		memcpy((u8 *)output + \
			KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP_##wire##_OFFSET, \
			&hw_ip.member, sizeof(hw_ip.member));
		KB2_AMDGPU_HW_IP_FIELDS(PACK_HW_IP)
#undef PACK_HW_IP
	} else {
		memcpy(output, native, size);
	}
	return result;
}

int kobox_linux_drm_gem_size(struct kobox_linux_drm_file *file,
			     u32 handle, u64 *size)
{
	const struct kobox_linux_drm_mapping_operations *operations;
	struct module *module;
	int result;

	if (!handle || !size)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	mutex_lock(&mapping_provider_lock);
	operations = mapping_provider;
	module = mapping_provider_module;
	if (!operations || !try_module_get(module)) {
		mutex_unlock(&mapping_provider_lock);
		return -ENODEV;
	}
	mutex_unlock(&mapping_provider_lock);
	result = operations->get_size(file->guard, handle, size);
	module_put(module);
	return result;
}

int kobox_linux_drm_amdgpu_mmap_validate(struct kobox_linux_drm_file *file,
					 u32 handle)
{
	union drm_amdgpu_gem_mmap argument = {0};
	int result;

	if (!handle)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	/* Run the driver's own CPU-access checks before exporting system pages
	 * as a native VMO; Kobox's mapping ID is not Linux's VMA offset. */
	argument.in.handle = handle;
	result = private_ioctl(file, DRM_IOCTL_AMDGPU_GEM_MMAP, &argument);
	return result ?: (!argument.out.addr_ptr ? -EUCLEAN : 0);
}

int kobox_linux_drm_amdgpu_cs(struct kobox_linux_drm_file *file,
			     u32 context_id, u32 bo_list_handle,
			     const struct kb2_amdgpu_cs_part *parts, size_t count,
			     const void *data, size_t bytes, u64 *sequence)
{
	struct cs_frame {
		struct drm_amdgpu_cs_chunk chunks[KB2_AMDGPU_CS_PARTS];
		struct drm_amdgpu_cs_chunk_ib ibs[KB2_AMDGPU_CS_PARTS];
		struct drm_amdgpu_bo_list_in lists[KB2_AMDGPU_CS_PARTS];
		u64 pointers[KB2_AMDGPU_CS_PARTS];
	} *frame;
	union drm_amdgpu_cs argument = {0};
	u8 *payload;
	size_t offset = 0, i;
	unsigned int ibs = 0, bos = 0;
	int result;

	if (!context_id || !parts || !count || count > KB2_AMDGPU_CS_PARTS ||
	    !data || !bytes || bytes > KB2_AMDGPU_CS_BYTES || !sequence)
		return -EINVAL;
	result = check_owner(file);
	if (result) return result;
	/* The native pointer graph is private and survives the whole ioctl.
	 * Even same-layout records are snapshotted, never driver-visible VMOs. */
	frame = kvzalloc(sizeof(*frame), GFP_KERNEL);
	payload = kvmalloc(bytes, GFP_KERNEL);
	if (!frame || !payload) {
		result = -ENOMEM;
		goto out;
	}
	memcpy(payload, data, bytes);
	for (i = 0; i < count; i++) {
		const struct kb2_amdgpu_cs_part *p = &parts[i];
		struct drm_amdgpu_cs_chunk *chunk = &frame->chunks[i];
		u8 *src;
		if (!kb2_amdgpu_cs_part_valid(p) || p->offset != offset ||
		    p->bytes > bytes - offset) {
			result = -EINVAL;
			goto out;
		}
		src = payload + offset;
		offset += p->bytes;
		chunk->chunk_data = (uintptr_t)src;
		chunk->length_dw = p->bytes / sizeof(u32);
		switch (p->record) {
		case KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB: {
			struct drm_amdgpu_cs_chunk_ib *ib = &frame->ibs[i];
			u32 reserved;
			memcpy(&ib->flags, src, 4);
			memcpy(&ib->ip_type, src + 4, 4);
			memcpy(&ib->ip_instance, src + 8, 4);
			memcpy(&ib->ring, src + 12, 4);
			memcpy(&ib->va_start, src + 16, 8);
			memcpy(&ib->ib_bytes, src + 24, 4);
			memcpy(&reserved, src + 28, 4);
			if (reserved || !ib->va_start || !ib->ib_bytes ||
			    ib->ip_type >= AMDGPU_HW_IP_NUM) {
				result = -EINVAL;
				goto out;
			}
			chunk->chunk_id = AMDGPU_CHUNK_ID_IB;
			chunk->chunk_data = (uintptr_t)ib;
			chunk->length_dw = sizeof(*ib) / sizeof(u32);
			ibs++;
			break;
		}
		case KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_BO_HANDLE: {
			struct drm_amdgpu_bo_list_in *list = &frame->lists[i];
			if (++bos > 1 || bo_list_handle) {
				result = -EINVAL;
				goto out;
			}
			static_assert(sizeof(struct drm_amdgpu_bo_list_entry) ==
				KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_BO_HANDLE_SIZE);
			list->bo_number = p->count;
			list->bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
			list->bo_info_ptr = (uintptr_t)src;
			chunk->chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
			chunk->chunk_data = (uintptr_t)list;
			chunk->length_dw = sizeof(*list) / sizeof(u32);
			break;
		}
#define NATIVE_CHUNK(record, id, type) \
		case KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_##record: { \
			static_assert(sizeof(struct type) == \
				KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_##record##_SIZE); \
			chunk->chunk_id = AMDGPU_CHUNK_ID_##id; break; }
		NATIVE_CHUNK(USER_FENCE, FENCE, drm_amdgpu_cs_chunk_fence);
		NATIVE_CHUNK(DEPENDENCY, DEPENDENCIES, drm_amdgpu_cs_chunk_dep);
		NATIVE_CHUNK(SYNCOBJ_IN, SYNCOBJ_IN, drm_amdgpu_cs_chunk_sem);
		NATIVE_CHUNK(SYNCOBJ_OUT, SYNCOBJ_OUT, drm_amdgpu_cs_chunk_sem);
		NATIVE_CHUNK(SCHEDULED_DEPENDENCY, SCHEDULED_DEPENDENCIES, drm_amdgpu_cs_chunk_dep);
		NATIVE_CHUNK(TIMELINE_WAIT, SYNCOBJ_TIMELINE_WAIT, drm_amdgpu_cs_chunk_syncobj);
		NATIVE_CHUNK(TIMELINE_SIGNAL, SYNCOBJ_TIMELINE_SIGNAL, drm_amdgpu_cs_chunk_syncobj);
		NATIVE_CHUNK(GFX_SHADOW, CP_GFX_SHADOW, drm_amdgpu_cs_chunk_cp_gfx_shadow);
#undef NATIVE_CHUNK
		default:
			result = -EOPNOTSUPP;
			goto out;
		}
		frame->pointers[i] = (uintptr_t)chunk;
	}
	if (!ibs || offset != bytes) {
		result = -EINVAL;
		goto out;
	}
	argument.in.ctx_id = context_id;
	argument.in.bo_list_handle = bo_list_handle;
	argument.in.num_chunks = count;
	argument.in.chunks = (uintptr_t)frame->pointers;
	result = private_ioctl(file, DRM_IOCTL_AMDGPU_CS, &argument);
	if (!result) *sequence = argument.out.handle;
out:
	kvfree(payload);
	kvfree(frame);
	return result;
}

int kobox_linux_drm_amdgpu_simple(struct kobox_linux_drm_file *file,
				 u32 command_id, const void *request,
				 size_t request_size, u64 deadline_ns,
				 void *record, size_t record_capacity,
				 u32 *record_id, size_t *record_size)
{
	const u8 *input = request;
	u8 *output = record;
	int result;

	if (!request || !record || !record_id || !record_size ||
	    record_capacity < KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_RESULT_SIZE)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	memset(output, 0, record_capacity);
	*record_id = 0;
	*record_size = 0;
	switch (command_id) {
	case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_CREATE: {
		union drm_amdgpu_gem_create argument = {0};

		if (request_size != sizeof(argument.in) || deadline_ns)
			return -EINVAL;
		memcpy(&argument.in, input, sizeof(argument.in));
		if (!argument.in.bo_size)
			return -EINVAL;
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_GEM_CREATE,
			&argument);
		if (!result) {
			if (!argument.out.handle)
				return -EUCLEAN;
			memcpy(output, &argument.out.handle, sizeof(u32));
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_GEM_HANDLE_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_GEM_HANDLE_RESULT_SIZE;
		}
		return result;
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_CONTEXT: {
		union drm_amdgpu_ctx argument = {0};
		u32 operation;

		if (request_size != sizeof(argument.in) || deadline_ns)
			return -EINVAL;
		memcpy(&argument.in, input, sizeof(argument.in));
		operation = argument.in.op;
		if ((operation < AMDGPU_CTX_OP_ALLOC_CTX ||
		     operation > AMDGPU_CTX_OP_QUERY_STATE2) || argument.in.flags ||
		    (operation == AMDGPU_CTX_OP_ALLOC_CTX && argument.in.ctx_id) ||
		    (operation != AMDGPU_CTX_OP_ALLOC_CTX && !argument.in.ctx_id))
			return -EINVAL;
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_CTX, &argument);
		if (!result) {
			u32 context = operation == AMDGPU_CTX_OP_ALLOC_CTX ?
				argument.out.alloc.ctx_id : 0;
			if (operation == AMDGPU_CTX_OP_ALLOC_CTX && !context)
				return -EUCLEAN;
			memcpy(output, &operation, sizeof(operation));
			memcpy(output + 4, &context, sizeof(context));
			if (operation == AMDGPU_CTX_OP_QUERY_STATE ||
			    operation == AMDGPU_CTX_OP_QUERY_STATE2) {
				memcpy(output + 8, &argument.out.state.flags, sizeof(u64));
				memcpy(output + 16, &argument.out.state.hangs, sizeof(u32));
				memcpy(output + 20, &argument.out.state.reset_status, sizeof(u32));
			}
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_CONTEXT_RESULT_SIZE;
		}
		return result;
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_METADATA: {
		struct drm_amdgpu_gem_metadata argument = {0};
		u32 bytes;
		if (request_size != KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_REQUEST_SIZE ||
		    record_capacity < KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT_SIZE ||
		    deadline_ns) return -EINVAL;
		memcpy(&argument.handle, input, sizeof(u32));
		memcpy(&argument.op, input + 4, sizeof(u32));
		memcpy(&argument.data.flags, input + 8, sizeof(u64));
		memcpy(&argument.data.tiling_info, input + 16, sizeof(u64));
		memcpy(&bytes, input + 24, sizeof(u32));
		if (!argument.handle || argument.data.flags ||
		    (argument.op != AMDGPU_GEM_METADATA_OP_SET_METADATA &&
		     argument.op != AMDGPU_GEM_METADATA_OP_GET_METADATA) ||
		    bytes > sizeof(argument.data.data) || input[28] || input[29] ||
		    input[30] || input[31]) return -EINVAL;
		argument.data.data_size_bytes = bytes;
		if (argument.op == AMDGPU_GEM_METADATA_OP_SET_METADATA)
			memcpy(argument.data.data, input + 32, bytes);
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_GEM_METADATA, &argument);
		if (!result) {
			bytes = argument.data.data_size_bytes;
			if (bytes > sizeof(argument.data.data)) return -EPROTO;
			memcpy(output, &argument.data.flags, sizeof(u64));
			memcpy(output + 8, &argument.data.tiling_info, sizeof(u64));
			memcpy(output + 16, &bytes, sizeof(u32));
			memcpy(output + 24, argument.data.data, bytes);
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_RESULT_SIZE;
		}
		return result;
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_WAIT_IDLE: {
		union drm_amdgpu_gem_wait_idle argument = {0};
		if (request_size != KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_REQUEST_SIZE)
			return -EINVAL;
		memcpy(&argument.in.handle, input, sizeof(u32));
		memcpy(&argument.in.flags, input + 4, sizeof(u32));
		if (!argument.in.handle || argument.in.flags) return -EINVAL;
		argument.in.timeout = deadline_ns;
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE, &argument);
		if (!result) {
			memcpy(output, &argument.out.status, sizeof(u32));
			memcpy(output + 4, &argument.out.domain, sizeof(u32));
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_GEM_WAIT_IDLE_RESULT_SIZE;
		}
		return result;
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_VA: {
		struct drm_amdgpu_gem_va argument = {0};

		if (request_size != KB2_GPU_DRM_AMDGPU_RECORD_GEM_VA_REQUEST_SIZE ||
		    deadline_ns)
			return -EINVAL;
		memcpy(&argument.handle, input, sizeof(u32));
		memcpy(&argument.operation, input + 4, sizeof(u32));
		memcpy(&argument.flags, input + 8, sizeof(u32));
		memcpy(&argument.va_address, input + 16, sizeof(u64));
		memcpy(&argument.offset_in_bo, input + 24, sizeof(u64));
		memcpy(&argument.map_size, input + 32, sizeof(u64));
		if (input[12] || input[13] || input[14] || input[15] ||
		    argument.vm_timeline_point || argument.vm_timeline_syncobj_out ||
		    argument.num_syncobj_handles || argument.input_fence_syncobj_handles ||
		    !argument.va_address || !argument.map_size)
			return -EINVAL;
		return private_ioctl(file, DRM_IOCTL_AMDGPU_GEM_VA, &argument);
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS: {
		union drm_amdgpu_wait_cs argument = {0};
		u32 busy;

		if (request_size != KB2_GPU_DRM_AMDGPU_RECORD_WAIT_CS_REQUEST_SIZE ||
		    !deadline_ns || deadline_ns > S64_MAX)
			return -EINVAL;
		memcpy(&argument.in.handle, input, sizeof(u64));
		argument.in.timeout = deadline_ns;
		memcpy(&argument.in.ip_type, input + 8, sizeof(u32) * 4);
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_WAIT_CS, &argument);
		if (!result) {
			if (argument.out.status > 1)
				return -EUCLEAN;
			busy = argument.out.status;
			memcpy(output, &busy, sizeof(busy));
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_WAIT_STATUS_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_WAIT_STATUS_RESULT_SIZE;
		}
		return result;
	}
	case KB2_GPU_DRM_AMDGPU_COMMAND_VM: {
		union drm_amdgpu_vm argument = {0};

		if (request_size != sizeof(argument.in) || deadline_ns)
			return -EINVAL;
		memcpy(&argument.in, input, sizeof(argument.in));
		if ((argument.in.op != AMDGPU_VM_OP_RESERVE_VMID &&
		     argument.in.op != AMDGPU_VM_OP_UNRESERVE_VMID) ||
		    argument.in.flags)
			return -EINVAL;
		result = private_ioctl(file, DRM_IOCTL_AMDGPU_VM, &argument);
		if (!result) {
			memcpy(output, &argument.out.flags, sizeof(u64));
			*record_id = KB2_GPU_DRM_AMDGPU_RECORD_VM_RESULT;
			*record_size = KB2_GPU_DRM_AMDGPU_RECORD_VM_RESULT_SIZE;
		}
		return result;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int amdgpu_sensor(struct kobox_linux_drm_file *file, u32 type,
			 u32 *value)
{
	u32 sensor_value = 0;
	struct drm_amdgpu_info argument = {
		.return_pointer = (u64)(unsigned long)&sensor_value,
		.return_size = sizeof(sensor_value),
		.query = AMDGPU_INFO_SENSOR,
		.sensor_info.type = type,
	};
	int result;

	if (!value)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_AMDGPU_INFO, &argument);
	if (!result)
		*value = sensor_value;
	return result;
}

int kobox_linux_drm_amdgpu_temperature(struct kobox_linux_drm_file *file,
					  u32 *millidegrees)
{
	return amdgpu_sensor(file, AMDGPU_INFO_SENSOR_GPU_TEMP, millidegrees);
}

int kobox_linux_drm_amdgpu_average_power(struct kobox_linux_drm_file *file,
					    u32 *watts)
{
	/* Upstream falls back to input power on older ASICs; SMU 14.0.2 itself
	 * implements average socket power, not the input-power sensor. */
	return amdgpu_sensor(file, AMDGPU_INFO_SENSOR_GPU_AVG_POWER, watts);
}

int kobox_linux_drm_set_client_cap(struct kobox_linux_drm_file *file,
				    u64 capability, u64 value)
{
	struct drm_set_client_cap argument = {
		.capability = capability,
		.value = value,
	};
	int result = check_owner(file);

	return result ?: private_ioctl(file, DRM_IOCTL_SET_CLIENT_CAP, &argument);
}

int kobox_linux_drm_get_magic(struct kobox_linux_drm_file *file, u32 *magic)
{
	struct drm_auth argument = {};
	int result = check_owner(file);

	if (!magic)
		return -EINVAL;
	result = result ?: private_ioctl(file, DRM_IOCTL_GET_MAGIC, &argument);
	if (!result)
		*magic = argument.magic;
	return result;
}

int kobox_linux_drm_auth_magic(struct kobox_linux_drm_file *file, u32 magic)
{
	struct drm_auth argument = { .magic = magic };
	int result = check_owner(file);

	return result ?: private_ioctl(file, DRM_IOCTL_AUTH_MAGIC, &argument);
}

int kobox_linux_drm_master(struct kobox_linux_drm_file *file, bool acquire)
{
	int result = check_owner(file);

	return result ?: private_ioctl(file,
		acquire ? DRM_IOCTL_SET_MASTER : DRM_IOCTL_DROP_MASTER, NULL);
}

int kobox_linux_drm_resources(struct kobox_linux_drm_file *file,
		u32 *fbs, size_t fb_capacity, u32 *crtcs, size_t crtc_capacity,
		u32 *connectors, size_t connector_capacity,
		u32 *encoders, size_t encoder_capacity,
		struct kobox_linux_drm_resources *out)
{
	struct drm_mode_card_res argument = {
		.fb_id_ptr = (uintptr_t)fbs,
		.crtc_id_ptr = (uintptr_t)crtcs,
		.connector_id_ptr = (uintptr_t)connectors,
		.encoder_id_ptr = (uintptr_t)encoders,
		.count_fbs = fb_capacity,
		.count_crtcs = crtc_capacity,
		.count_connectors = connector_capacity,
		.count_encoders = encoder_capacity,
	};
	int result;

	if (!out || (!fbs && fb_capacity) || (!crtcs && crtc_capacity) ||
	    (!connectors && connector_capacity) || (!encoders && encoder_capacity) ||
	    fb_capacity > U32_MAX || crtc_capacity > U32_MAX ||
	    connector_capacity > U32_MAX || encoder_capacity > U32_MAX)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_MODE_GETRESOURCES, &argument);
	if (!result)
		*out = (struct kobox_linux_drm_resources) {
			.fb_count = argument.count_fbs,
			.crtc_count = argument.count_crtcs,
			.connector_count = argument.count_connectors,
			.encoder_count = argument.count_encoders,
			.min_width = argument.min_width,
			.max_width = argument.max_width,
			.min_height = argument.min_height,
			.max_height = argument.max_height,
		};
	return result;
}

int kobox_linux_drm_connector(struct kobox_linux_drm_file *file,
		u32 connector_id,
		struct kobox_linux_drm_mode *modes, size_t mode_capacity,
		struct kobox_linux_drm_property_value *properties,
		size_t property_capacity,
		u32 *encoders, size_t encoder_capacity,
		struct kobox_linux_drm_connector *out)
{
	u32 *property_ids = NULL;
	u64 *property_values = NULL;
	struct drm_mode_get_connector argument;
	size_t index;
	int result;

	static_assert(sizeof(struct kobox_linux_drm_mode) ==
		      sizeof(struct drm_mode_modeinfo));
	static_assert(offsetof(struct kobox_linux_drm_mode, name) ==
		      offsetof(struct drm_mode_modeinfo, name));
	if (!connector_id || !out || (!modes && mode_capacity) ||
	    (!properties && property_capacity) || (!encoders && encoder_capacity) ||
	    mode_capacity > U32_MAX || property_capacity > U32_MAX ||
	    encoder_capacity > U32_MAX)
		return -EINVAL;
	if (property_capacity) {
		property_ids = kcalloc(property_capacity, sizeof(*property_ids),
			GFP_KERNEL);
		property_values = kcalloc(property_capacity,
			sizeof(*property_values), GFP_KERNEL);
		if (!property_ids || !property_values) {
			result = -ENOMEM;
			goto done;
		}
	}
	argument = (struct drm_mode_get_connector) {
		.encoders_ptr = (uintptr_t)encoders,
		.modes_ptr = (uintptr_t)modes,
		.props_ptr = (uintptr_t)property_ids,
		.prop_values_ptr = (uintptr_t)property_values,
		.count_modes = mode_capacity,
		.count_props = property_capacity,
		.count_encoders = encoder_capacity,
		.connector_id = connector_id,
	};
	result = check_owner(file);
	if (result)
		goto done;
	result = private_ioctl(file, DRM_IOCTL_MODE_GETCONNECTOR, &argument);
	if (result)
		goto done;
	for (index = 0; index < property_capacity; index++)
		properties[index] = (struct kobox_linux_drm_property_value) {
			.property_id = property_ids[index],
			.value = property_values[index],
		};
	*out = (struct kobox_linux_drm_connector) {
		.encoder_id = argument.encoder_id,
		.connector_id = argument.connector_id,
		.connector_type = argument.connector_type,
		.connector_type_id = argument.connector_type_id,
		.connection = argument.connection,
		.width_mm = argument.mm_width,
		.height_mm = argument.mm_height,
		.subpixel = argument.subpixel,
		.mode_count = argument.count_modes,
		.property_count = argument.count_props,
		.encoder_count = argument.count_encoders,
	};
done:
	kfree(property_values);
	kfree(property_ids);
	return result;
}

int kobox_linux_drm_encoder(struct kobox_linux_drm_file *file,
		u32 encoder_id, struct kobox_linux_drm_encoder *out)
{
	struct drm_mode_get_encoder argument = {.encoder_id = encoder_id};
	int result;

	if (!encoder_id || !out)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_MODE_GETENCODER, &argument);
	if (!result)
		*out = (struct kobox_linux_drm_encoder) {
			.encoder_id = argument.encoder_id,
			.encoder_type = argument.encoder_type,
			.crtc_id = argument.crtc_id,
			.possible_crtcs = argument.possible_crtcs,
			.possible_clones = argument.possible_clones,
		};
	return result;
}

int kobox_linux_drm_get_crtc(struct kobox_linux_drm_file *file,
		u32 crtc_id, struct kobox_linux_drm_crtc *out)
{
	struct drm_mode_crtc argument = {.crtc_id = crtc_id};
	int result;

	static_assert(sizeof(struct kobox_linux_drm_mode) ==
		      sizeof(struct drm_mode_modeinfo));
	if (!crtc_id || !out)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = private_ioctl(file, DRM_IOCTL_MODE_GETCRTC, &argument);
	if (!result) {
		*out = (struct kobox_linux_drm_crtc) {
			.crtc_id = argument.crtc_id,
			.fb_id = argument.fb_id,
			.x = argument.x,
			.y = argument.y,
			.gamma_size = argument.gamma_size,
			.mode_valid = argument.mode_valid,
		};
		memcpy(&out->mode, &argument.mode, sizeof(out->mode));
	}
	return result;
}

int kobox_linux_drm_set_crtc(struct kobox_linux_drm_file *file,
		u32 crtc_id, u32 fb_id, u32 x, u32 y,
		const u32 *connectors, size_t connector_count,
		const struct kobox_linux_drm_mode *mode)
{
	struct drm_mode_crtc argument = {
		.set_connectors_ptr = (uintptr_t)connectors,
		.count_connectors = connector_count,
		.crtc_id = crtc_id,
		.fb_id = fb_id,
		.x = x,
		.y = y,
		.mode_valid = !!mode,
	};
	int result;

	static_assert(sizeof(struct kobox_linux_drm_mode) ==
		      sizeof(struct drm_mode_modeinfo));
	if (!crtc_id || connector_count > U32_MAX ||
	    (!!connector_count != !!connectors))
		return -EINVAL;
	if (mode)
		memcpy(&argument.mode, mode, sizeof(argument.mode));
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_SETCRTC, &argument);
	return result;
}

int kobox_linux_drm_page_flip(struct kobox_linux_drm_file *file,
		u32 crtc_id, u32 fb_id, u32 flags, u32 sequence,
		u64 event_token)
{
	struct drm_mode_crtc_page_flip argument = {
		.crtc_id = crtc_id,
		.fb_id = fb_id,
		.flags = flags,
		.reserved = sequence,
		.user_data = event_token,
	};
	u32 target = flags & DRM_MODE_PAGE_FLIP_TARGET;
	int result;

	if (!crtc_id || !fb_id ||
	    (flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC |
		DRM_MODE_PAGE_FLIP_TARGET)) ||
	    target == DRM_MODE_PAGE_FLIP_TARGET || (!target && sequence) ||
	    (!(flags & DRM_MODE_PAGE_FLIP_EVENT) && event_token))
		return -EINVAL;
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_PAGE_FLIP, &argument);
	return result;
}

int kobox_linux_drm_cursor(struct kobox_linux_drm_file *file,
		const struct kobox_linux_drm_cursor *cursor)
{
	struct drm_mode_cursor2 argument;
	int result;

	if (!cursor)
		return -EINVAL;
	argument = (struct drm_mode_cursor2) {
		.flags = cursor->flags,
		.crtc_id = cursor->crtc_id,
		.x = cursor->x, .y = cursor->y,
		.width = cursor->width, .height = cursor->height,
		.handle = cursor->handle,
		.hot_x = cursor->hot_x, .hot_y = cursor->hot_y,
	};
	/* Keep DRM's master/lease, GEM ownership and device checks intact.
	 * Legacy CURSOR has the same semantics with a zero hotspot. */
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_CURSOR2, &argument);
	return result;
}

int kobox_linux_drm_dirty_fb(struct kobox_linux_drm_file *file,
		u32 fb_id, u32 flags, u32 color,
		const struct kobox_linux_drm_rectangle *rectangles,
		size_t rectangle_count)
{
	struct drm_mode_fb_dirty_cmd argument = {
		.fb_id = fb_id,
		.flags = flags,
		.color = color,
		.num_clips = rectangle_count,
	};
	struct drm_clip_rect *clips = NULL;
	int result;

	if (flags & ~DRM_MODE_FB_DIRTY_FLAGS ||
	    rectangle_count > DRM_MODE_FB_DIRTY_MAX_CLIPS ||
	    (!!rectangle_count != !!rectangles) ||
	    ((flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) &&
	     (rectangle_count & 1)))
		return -EINVAL;
	result = check_owner(file);
	if (result || !rectangle_count)
		return result ?: private_ioctl(file, DRM_IOCTL_MODE_DIRTYFB,
					       &argument);
	clips = kmalloc_array(rectangle_count, sizeof(*clips), GFP_KERNEL);
	if (!clips)
		return -ENOMEM;
	for (size_t index = 0; index < rectangle_count; index++) {
		const struct kobox_linux_drm_rectangle *rectangle =
			&rectangles[index];

		if (rectangle->x1 > U16_MAX || rectangle->y1 > U16_MAX ||
		    rectangle->x2 > U16_MAX || rectangle->y2 > U16_MAX) {
			result = -ERANGE;
			goto free_clips;
		}
		clips[index] = (struct drm_clip_rect) {
			.x1 = rectangle->x1,
			.y1 = rectangle->y1,
			.x2 = rectangle->x2,
			.y2 = rectangle->y2,
		};
	}
	argument.clips_ptr = (uintptr_t)clips;
	result = private_ioctl(file, DRM_IOCTL_MODE_DIRTYFB, &argument);
free_clips:
	kfree(clips);
	return result;
}

static void diagnose_backing_owners(struct drm_file *requester)
{
	struct drm_device *device = requester->minor->dev;
	struct drm_file *file;
	unsigned int files = 0;
	u64 handles = 0, render_bytes = 0, display_bytes = 0;
	u64 largest_file = 0, requester_bytes = 0;
	static unsigned int reports;

	/* Real multi-tab use exhausted the hosted RAM pool. On only the first
	 * two denials, distinguish live handle ownership from memory retained
	 * outside handle tables. Imported objects can be counted more than once:
	 * these sums describe references, not unique physical resident bytes. */
	if (reports >= 2)
		return;
	reports++;
	mutex_lock(&device->filelist_mutex);
	list_for_each_entry(file, &device->filelist, lhead) {
		struct drm_gem_object *object;
		unsigned long flags;
		u64 bytes = 0;
		int id;

		spin_lock_irqsave(&file->table_lock, flags);
		idr_for_each_entry(&file->object_idr, object, id) {
			bytes += object->size;
			handles++;
		}
		spin_unlock_irqrestore(&file->table_lock, flags);
		files++;
		if (drm_is_render_client(file))
			render_bytes += bytes;
		else
			display_bytes += bytes;
		largest_file = max(largest_file, bytes);
		if (file == requester)
			requester_bytes = bytes;
	}
	mutex_unlock(&device->filelist_mutex);
	kobox_linux_boot_diagnostic("kobox-drm: memory handle-references files=%u "
		"handles=%llu render-bytes=%llu display-bytes=%llu largest-file-bytes=%llu "
		"requester-bytes=%llu\n", files, handles, render_bytes, display_bytes,
		largest_file, requester_bytes);
}

static int admit_backing_memory(struct kobox_linux_drm_file *file, u64 bytes)
{
	struct sysinfo memory;
	struct drm_file *drm_file;
	int result = check_owner(file);

	if (result)
		return result;
	drm_file = file->guard->private_data;
	if (!drm_file || !drm_file->minor)
		return -ENODEV;
	si_meminfo(&memory);
	if (kobox_drm_memory_admit((u64)memory.freeram * memory.mem_unit,
				   bytes, drm_is_render_client(drm_file)))
		return 0;
	kobox_linux_boot_diagnostic(
		"kobox-drm: backing admission denied bytes=%llu free-pages=%lu "
		"shmem-pages=%lu render=%u\n", bytes, memory.freeram,
		memory.sharedram, drm_is_render_client(drm_file));
	diagnose_backing_owners(drm_file);
	return -ENOMEM;
}

int kobox_linux_drm_create_dumb(struct kobox_linux_drm_file *file,
		struct kobox_linux_drm_dumb_buffer *buffer)
{
	struct drm_mode_create_dumb argument;
	u64 stride;
	int result;

	if (!buffer || !buffer->height || !buffer->width ||
	    !buffer->bits_per_pixel || buffer->handle || buffer->pitch ||
	    buffer->size)
		return -EINVAL;
	argument = (struct drm_mode_create_dumb) {
		.height = buffer->height,
		.width = buffer->width,
		.bpp = buffer->bits_per_pixel,
		.flags = buffer->flags,
	};
	/*
	 * Use wide arithmetic before admission; the driver still validates its
	 * supported formats and computes the final pitch/size.
	 */
	stride = (u64)buffer->width * DIV_ROUND_UP((u64)buffer->bits_per_pixel, 8);
	if (stride > U64_MAX / buffer->height)
		return -EINVAL;
	result = admit_backing_memory(file, stride * buffer->height);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_CREATE_DUMB, &argument);
	if (!result) {
		if (!argument.handle || !argument.pitch || !argument.size)
			return -EUCLEAN;
		buffer->handle = argument.handle;
		buffer->pitch = argument.pitch;
		buffer->size = argument.size;
	}
	return result;
}

int kobox_linux_drm_add_fb(struct kobox_linux_drm_file *file,
		struct kobox_linux_drm_fb *framebuffer)
{
	struct drm_mode_fb_cmd argument;
	int result;

	if (!framebuffer || framebuffer->fb_id || !framebuffer->width ||
	    !framebuffer->height || !framebuffer->pitch ||
	    !framebuffer->bits_per_pixel || !framebuffer->depth ||
	    !framebuffer->handle)
		return -EINVAL;
	argument = (struct drm_mode_fb_cmd) {
		.width = framebuffer->width,
		.height = framebuffer->height,
		.pitch = framebuffer->pitch,
		.bpp = framebuffer->bits_per_pixel,
		.depth = framebuffer->depth,
		.handle = framebuffer->handle,
	};
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_ADDFB, &argument);
	if (!result)
		framebuffer->fb_id = argument.fb_id;
	return result;
}

int kobox_linux_drm_remove_fb(struct kobox_linux_drm_file *file, u32 fb_id)
{
	int result;

	if (!fb_id)
		return -EINVAL;
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_RMFB, &fb_id);
	return result;
}

int kobox_linux_drm_add_fb2(struct kobox_linux_drm_file *file,
		struct kobox_linux_drm_fb2 *framebuffer)
{
	struct drm_mode_fb_cmd2 argument;
	int result;

	if (!framebuffer || framebuffer->fb_id || !framebuffer->width ||
	    !framebuffer->height || !framebuffer->pixel_format ||
	    !framebuffer->handles[0] || !framebuffer->pitches[0] ||
	    (framebuffer->flags & ~DRM_MODE_FB_MODIFIERS))
		return -EINVAL;
	memset(&argument, 0, sizeof(argument));
	argument.width = framebuffer->width;
	argument.height = framebuffer->height;
	argument.pixel_format = framebuffer->pixel_format;
	argument.flags = framebuffer->flags;
	memcpy(argument.handles, framebuffer->handles, sizeof(argument.handles));
	memcpy(argument.pitches, framebuffer->pitches, sizeof(argument.pitches));
	memcpy(argument.offsets, framebuffer->offsets, sizeof(argument.offsets));
	memcpy(argument.modifier, framebuffer->modifiers, sizeof(argument.modifier));
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_MODE_ADDFB2, &argument);
	if (!result)
		framebuffer->fb_id = argument.fb_id;
	return result;
}

int kobox_linux_drm_object_properties(struct kobox_linux_drm_file *file,
		u32 object_id, u32 object_type,
		struct kobox_linux_drm_property_value *properties,
		size_t property_capacity, u32 *property_count)
{
	u32 *property_ids = NULL;
	u64 *property_values = NULL;
	struct drm_mode_obj_get_properties argument;
	size_t copied, index;
	int result;

	if (!object_id || !object_type || !property_count ||
	    (!properties && property_capacity) || property_capacity > U32_MAX)
		return -EINVAL;
	if (property_capacity) {
		property_ids = kcalloc(property_capacity, sizeof(*property_ids),
			GFP_KERNEL);
		property_values = kcalloc(property_capacity,
			sizeof(*property_values), GFP_KERNEL);
		if (!property_ids || !property_values) {
			result = -ENOMEM;
			goto done;
		}
	}
	argument = (struct drm_mode_obj_get_properties) {
		.props_ptr = (uintptr_t)property_ids,
		.prop_values_ptr = (uintptr_t)property_values,
		.count_props = property_capacity,
		.obj_id = object_id,
		.obj_type = object_type,
	};
	result = check_owner(file);
	if (result)
		goto done;
	result = private_ioctl(file, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &argument);
	if (result)
		goto done;
	copied = min_t(size_t, property_capacity, argument.count_props);
	for (index = 0; index < copied; index++)
		properties[index] = (struct kobox_linux_drm_property_value) {
			.property_id = property_ids[index],
			.value = property_values[index],
		};
	*property_count = argument.count_props;
done:
	kfree(property_values);
	kfree(property_ids);
	return result;
}

int kobox_linux_drm_poll_events(struct kobox_linux_drm_file *file,
				u32 requested, u32 *ready)
{
	__poll_t mask;
	int result;

	if (!ready || (requested & ~1U))
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	mask = vfs_poll(file->guard, NULL);
	*ready = requested && (mask & (EPOLLIN | EPOLLRDNORM));
	return 0;
}

int kobox_linux_drm_read_events(struct kobox_linux_drm_file *file,
				void *output, size_t capacity, size_t *bytes)
{
	struct drm_file *drm_file;
	struct drm_device *device;
	size_t copied = 0;
	int result;

	if (!output || !capacity || !bytes)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	drm_file = file->guard->private_data;
	device = drm_file->minor->dev;
	result = mutex_lock_interruptible(&drm_file->event_read_lock);
	if (result)
		return result;
	for (;;) {
		struct drm_pending_event *event = NULL;
		unsigned int length;

		spin_lock_irq(&device->event_lock);
		if (!list_empty(&drm_file->event_list)) {
			event = list_first_entry(&drm_file->event_list,
				struct drm_pending_event, link);
			drm_file->event_space += event->event->length;
			list_del(&event->link);
		}
		spin_unlock_irq(&device->event_lock);
		if (!event)
			break;
		length = event->event->length;
		if (length > capacity - copied) {
			spin_lock_irq(&device->event_lock);
			drm_file->event_space -= length;
			list_add(&event->link, &drm_file->event_list);
			spin_unlock_irq(&device->event_lock);
			wake_up_interruptible_poll(&drm_file->event_wait,
				EPOLLIN | EPOLLRDNORM);
			break;
		}
		memcpy((u8 *)output + copied, event->event, length);
		copied += length;
		kfree(event);
	}
	mutex_unlock(&drm_file->event_read_lock);
	*bytes = copied;
	return 0;
}

/* Driver-private handlers receive only fully decoded, private kernel buffers.
 * Keep the real DRM permission, unplug and module-lifetime checks by entering
 * through the installed file operation. */
static int virtgpu_ioctl(struct kobox_linux_drm_file *file, unsigned int command,
			 void *argument)
{
	struct drm_file *drm_file;
	struct drm_device *device;
	int result = check_owner(file);

	if (result)
		return result;
	drm_file = file->guard->private_data;
	device = drm_file->minor->dev;
	if (!device->driver || strcmp(device->driver->name, "virtio_gpu"))
		return -ENODEV;
	/* command is selected only by the fixed typed wrappers below.  Enter via
	 * the installed file operation so module lifetime, descriptor lookup,
	 * permission checks and unplug handling stay on the normal DRM path. */
	return private_ioctl(file, command, argument);
}

int kobox_linux_drm_gem_close(struct kobox_linux_drm_file *file, u32 handle)
{
	struct drm_gem_close argument = {.handle = handle};

	if (!handle)
		return -EINVAL;
	/* This fixed typed call deliberately retains the upstream core ioctl table,
	 * feature and DRM_RENDER_ALLOW permission path. */
	return check_owner(file) ?: private_ioctl(file, DRM_IOCTL_GEM_CLOSE,
							 &argument);
}

int kobox_linux_drm_prime_export(struct kobox_linux_drm_file *file,
				 u32 handle, u32 flags, int *dma_fd)
{
	struct drm_prime_handle argument = {
		.handle = handle, .flags = flags, .fd = -1,
	};
	int result;

	if (!handle || !dma_fd || (flags & ~(DRM_CLOEXEC | DRM_RDWR)))
		return -EINVAL;
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_PRIME_HANDLE_TO_FD, &argument);
	if (!result) {
		if (argument.fd < 0)
			return -EUCLEAN;
		*dma_fd = argument.fd;
	}
	return result;
}

int kobox_linux_drm_prime_import(struct kobox_linux_drm_file *file,
				 int dma_fd, u32 *handle)
{
	struct drm_prime_handle argument = {.fd = dma_fd};
	int result;

	if (dma_fd < 0 || !handle)
		return -EINVAL;
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_PRIME_FD_TO_HANDLE, &argument);
	if (!result) {
		if (!argument.handle)
			return -EUCLEAN;
		*handle = argument.handle;
	}
	return result;
}

static int decode_handles(const void *bytes, size_t count, u32 **handles_out)
{
	const u8 *encoded = bytes;
	u32 *handles;
	size_t index;

	if (!handles_out || !bytes || !count || count > 65536 ||
	    count > SIZE_MAX / sizeof(*handles))
		return -EINVAL;
	handles = kvmalloc_array(count, sizeof(*handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;
	for (index = 0; index < count; index++) {
		const u8 *value = encoded + index * sizeof(*handles);

		handles[index] = value[0] | (u32)value[1] << 8 |
			(u32)value[2] << 16 | (u32)value[3] << 24;
		if (!handles[index]) {
			kvfree(handles);
			return -EINVAL;
		}
	}
	*handles_out = handles;
	return 0;
}

static int decode_exec_syncobjs(const void *bytes, size_t count, bool output,
				struct drm_virtgpu_execbuffer_syncobj **out)
{
	const u8 *encoded = bytes;
	struct drm_virtgpu_execbuffer_syncobj *syncobjs;
	size_t index;

	if (!out || !bytes || !count || count > 65536 ||
	    count > SIZE_MAX / sizeof(*syncobjs))
		return -EINVAL;
	syncobjs = kvmalloc_array(count, sizeof(*syncobjs), GFP_KERNEL);
	if (!syncobjs)
		return -ENOMEM;
	for (index = 0; index < count; index++) {
		const u8 *value = encoded + index * 16;
		u32 handle = value[0] | (u32)value[1] << 8 |
			(u32)value[2] << 16 | (u32)value[3] << 24;
		u32 flags = value[4] | (u32)value[5] << 8 |
			(u32)value[6] << 16 | (u32)value[7] << 24;
		u64 point = value[8] | (u64)value[9] << 8 |
			(u64)value[10] << 16 | (u64)value[11] << 24 |
			(u64)value[12] << 32 | (u64)value[13] << 40 |
			(u64)value[14] << 48 | (u64)value[15] << 56;

		if (!handle || (output ? flags != 0 :
			(flags & ~VIRTGPU_EXECBUF_SYNCOBJ_FLAGS))) {
			kvfree(syncobjs);
			return -EINVAL;
		}
		syncobjs[index] = (struct drm_virtgpu_execbuffer_syncobj) {
			.handle = handle, .flags = flags, .point = point,
		};
	}
	*out = syncobjs;
	return 0;
}

int kobox_linux_drm_syncobj_create(struct kobox_linux_drm_file *file,
					 u32 flags, u32 *handle)
{
	struct drm_syncobj_create argument = {.flags = flags};
	int result;

	if (!handle || (flags & ~DRM_SYNCOBJ_CREATE_SIGNALED))
		return -EINVAL;
	result = check_owner(file);
	if (!result)
		result = private_ioctl(file, DRM_IOCTL_SYNCOBJ_CREATE, &argument);
	if (!result) {
		if (!argument.handle)
			return -EUCLEAN;
		*handle = argument.handle;
	}
	return result;
}

int kobox_linux_drm_syncobj_destroy(struct kobox_linux_drm_file *file,
					  u32 handle)
{
	struct drm_syncobj_destroy argument = {.handle = handle};

	if (!handle)
		return -EINVAL;
	return check_owner(file) ?: private_ioctl(file,
		DRM_IOCTL_SYNCOBJ_DESTROY, &argument);
}

int kobox_linux_drm_syncobj_wait(struct kobox_linux_drm_file *file,
				       const void *handle_bytes, size_t handle_count,
				       s64 timeout_nsec, u32 flags,
				       u64 fence_deadline_ns, u32 *first_signaled)
{
	struct drm_syncobj_wait argument = {
		.timeout_nsec = timeout_nsec,
		.count_handles = handle_count,
		.flags = flags,
		.deadline_nsec = fence_deadline_ns,
	};
	u32 *handles = NULL;
	int result;

	if (!first_signaled || handle_count > U32_MAX || (flags & ~15U) ||
	    (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) && fence_deadline_ns))
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = decode_handles(handle_bytes, handle_count, &handles);
	if (result)
		return result;
	argument.handles = (uintptr_t)handles;
	result = private_ioctl(file, DRM_IOCTL_SYNCOBJ_WAIT, &argument);
	if (!result)
		*first_signaled = argument.first_signaled;
	kvfree(handles);
	return result;
}

int kobox_linux_drm_syncobj_array(struct kobox_linux_drm_file *file,
					const void *handle_bytes, size_t handle_count,
					bool signal)
{
	struct drm_syncobj_array argument = {.count_handles = handle_count};
	u32 *handles = NULL;
	int result;

	if (handle_count > U32_MAX)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	result = decode_handles(handle_bytes, handle_count, &handles);
	if (result)
		return result;
	argument.handles = (uintptr_t)handles;
	result = private_ioctl(file, signal ? DRM_IOCTL_SYNCOBJ_SIGNAL :
		DRM_IOCTL_SYNCOBJ_RESET, &argument);
	kvfree(handles);
	return result;
}

int kobox_linux_drm_virtgpu_getparam(struct kobox_linux_drm_file *file,
				     u64 parameter, u64 *value)
{
	int private_value = 0;
	struct drm_virtgpu_getparam argument = {
		.param = parameter,
		.value = (uintptr_t)&private_value,
	};
	int result;

	if (!value)
		return -EINVAL;
	result = virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_GETPARAM, &argument);
	if (!result)
		*value = private_value;
	return result;
}

int kobox_linux_drm_virtgpu_get_caps(struct kobox_linux_drm_file *file,
				     u32 capset_id, u32 capset_version,
				     void *output, size_t capacity)
{
	struct drm_virtgpu_get_caps argument = {0};
	void *private;
	int result;

	if (!output || !capacity || capacity > SZ_1M || capacity > U32_MAX)
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	/* Never expose a Pacha mapping to Linux uaccess or the virtio-gpu driver. */
	private = kvzalloc(capacity, GFP_KERNEL);
	if (!private)
		return -ENOMEM;
	argument.cap_set_id = capset_id;
	argument.cap_set_ver = capset_version;
	argument.addr = (uintptr_t)private;
	argument.size = capacity;
	result = virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_GET_CAPS, &argument);
	if (!result)
		memcpy(output, private, capacity);
	kvfree(private);
	return result;
}

int kobox_linux_drm_virtgpu_context_init(struct kobox_linux_drm_file *file,
					 u32 parameter_mask, u32 capset_id,
					 u32 ring_count, u64 poll_ring_mask,
					 const void *debug_name,
					 size_t debug_name_size)
{
	enum {
		HAS_CAPSET = 1U << 0,
		HAS_RINGS = 1U << 1,
		HAS_POLL_MASK = 1U << 2,
		HAS_DEBUG_NAME = 1U << 3,
	};
	struct drm_virtgpu_context_set_param parameters[4] = {0};
	struct drm_virtgpu_context_init argument = {0};
	char private_name[65] = {0};
	size_t count = 0;
	int result;

	if (!parameter_mask || (parameter_mask & ~0xfU) || capset_id > 63 ||
	    ring_count > 64 ||
	    (!(parameter_mask & HAS_CAPSET) && capset_id) ||
	    (!(parameter_mask & HAS_RINGS) && ring_count) ||
	    (!(parameter_mask & HAS_POLL_MASK) && poll_ring_mask) ||
	    ((parameter_mask & HAS_POLL_MASK) &&
	     (!(parameter_mask & HAS_RINGS) || !ring_count ||
	      (ring_count < 64 && (poll_ring_mask >> ring_count)))) ||
	    debug_name_size > 64 ||
	    (!!(parameter_mask & HAS_DEBUG_NAME) != !!debug_name_size) ||
	    (debug_name_size && !debug_name))
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
	if (parameter_mask & HAS_CAPSET)
		parameters[count++] = (struct drm_virtgpu_context_set_param) {
			.param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, .value = capset_id,
		};
	if (parameter_mask & HAS_RINGS)
		parameters[count++] = (struct drm_virtgpu_context_set_param) {
			.param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS, .value = ring_count,
		};
	if (parameter_mask & HAS_POLL_MASK)
		parameters[count++] = (struct drm_virtgpu_context_set_param) {
			.param = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK,
			.value = poll_ring_mask,
		};
	if (parameter_mask & HAS_DEBUG_NAME) {
		memcpy(private_name, debug_name, debug_name_size);
		parameters[count++] = (struct drm_virtgpu_context_set_param) {
			.param = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME,
			.value = (uintptr_t)private_name,
		};
	}
	argument.num_params = count;
	argument.ctx_set_params = (uintptr_t)parameters;
	return virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &argument);
}

int kobox_linux_drm_virtgpu_execbuffer(struct kobox_linux_drm_file *file,
				       u32 flags, u32 ring_index,
				       const void *command, size_t command_size,
				       const void *handle_bytes, size_t handle_count,
				       const void *input_bytes, size_t input_count,
				       const void *output_bytes, size_t output_count,
				       struct dma_fence **completion)
{
	struct drm_virtgpu_execbuffer argument = {0};
	void *private_command;
	u32 *private_handles = NULL;
	struct drm_virtgpu_execbuffer_syncobj *private_input = NULL;
	struct drm_virtgpu_execbuffer_syncobj *private_output = NULL;
	int result;
#if defined(KOBOX_DRM_EXEC_PROFILE)
	u64 profile_start, profile_copied, profile_submitted;
#endif

	if (!completion || *completion ||
	    (flags & ~(VIRTGPU_EXECBUF_FENCE_FD_OUT |
		     VIRTGPU_EXECBUF_RING_IDX)) ||
	    (!(flags & VIRTGPU_EXECBUF_RING_IDX) && ring_index) ||
	    !command || !command_size || command_size > SZ_16M ||
	    handle_count > 65536 || input_count > 65536 || output_count > 65536 ||
	    (handle_count && !handle_bytes) || (input_count && !input_bytes) ||
	    (output_count && !output_bytes))
		return -EINVAL;
	result = check_owner(file);
	if (result)
		return result;
#if defined(KOBOX_DRM_EXEC_PROFILE)
	profile_start = exec_profile_stamp();
#endif
	private_command = kvmemdup(command, command_size, GFP_KERNEL);
	if (!private_command)
		return -ENOMEM;
	if (handle_count && (result = decode_handles(handle_bytes, handle_count,
			&private_handles)))
		goto free_command;
	if (input_count && (result = decode_exec_syncobjs(input_bytes, input_count,
			false, &private_input)))
		goto free_handles;
	if (output_count && (result = decode_exec_syncobjs(output_bytes, output_count,
			true, &private_output)))
		goto free_input;
	argument.flags = flags;
	argument.size = command_size;
	argument.command = (uintptr_t)private_command;
	argument.bo_handles = (uintptr_t)private_handles;
	argument.num_bo_handles = handle_count;
	argument.fence_fd = -1;
	argument.ring_idx = ring_index;
	argument.syncobj_stride = input_count || output_count ?
		sizeof(struct drm_virtgpu_execbuffer_syncobj) : 0;
	argument.num_in_syncobjs = input_count;
	argument.num_out_syncobjs = output_count;
	argument.in_syncobjs = (uintptr_t)private_input;
	argument.out_syncobjs = (uintptr_t)private_output;
#if defined(KOBOX_DRM_EXEC_PROFILE)
	profile_copied = exec_profile_stamp();
#endif
	result = virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_EXECBUFFER, &argument);
#if defined(KOBOX_DRM_EXEC_PROFILE)
	profile_submitted = exec_profile_stamp();
#endif
	if (!result && (flags & VIRTGPU_EXECBUF_FENCE_FD_OUT)) {
		struct dma_fence *fence;
		int closed;

		if (argument.fence_fd < 0) {
			result = -EPROTO;
		} else {
			fence = sync_file_get_fence(argument.fence_fd);
			closed = close_fd(argument.fence_fd);
			if (!fence)
				result = -EPROTO;
			else
				*completion = fence;
			if (!result && closed)
				result = closed;
		}
	}
	if (result && *completion) {
		dma_fence_put(*completion);
		*completion = NULL;
	}

#if defined(KOBOX_DRM_EXEC_PROFILE)
	exec_profile_record(file, flags, profile_start, profile_copied,
			    profile_submitted, result);
#endif
	if (result)
		pr_err("kobox-drm: virtgpu execbuffer failed status=%d flags=0x%x "
		       "command=%zu handles=%zu in-syncobjs=%zu out-syncobjs=%zu\n",
		       result, flags, command_size, handle_count, input_count,
		       output_count);
	kvfree(private_output);
free_input:
	kvfree(private_input);
free_handles:
	kvfree(private_handles);
free_command:
	kvfree(private_command);
	return result;
}

int kobox_linux_drm_virtgpu_resource_create(
	struct kobox_linux_drm_file *file,
	struct kobox_linux_virtgpu_resource_create *resource)
{
	struct drm_virtgpu_resource_create argument;
	int result;

	if (!resource)
		return -EINVAL;
	result = admit_backing_memory(file, resource->size);
	if (result)
		return result;
	argument = (struct drm_virtgpu_resource_create) {
		.target = resource->target, .format = resource->format,
		.bind = resource->bind, .width = resource->width,
		.height = resource->height, .depth = resource->depth,
		.array_size = resource->array_size, .last_level = resource->last_level,
		.nr_samples = resource->sample_count, .flags = resource->flags,
		.bo_handle = resource->backing_handle, .size = resource->size,
		.stride = resource->stride,
	};
	result = virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &argument);
	if (result == -ENOMEM) {
		struct drm_file *drm_file = file->guard->private_data;
		struct sysinfo memory;

		si_meminfo(&memory);
		kobox_linux_boot_diagnostic("kobox-drm: resource memory failure size=%u dimensions=%ux%ux%u "
		       "free-pages=%lu total-pages=%lu shmem-pages=%lu\n",
		       resource->size, resource->width, resource->height,
		       resource->depth, memory.freeram, memory.totalram,
		       memory.sharedram);
		kobox_linux_dma_diagnose(drm_file->minor->dev->dev);
	}
	if (!result) {
		resource->bo_handle = argument.bo_handle;
		resource->resource_handle = argument.res_handle;
		resource->size = argument.size;
		resource->stride = argument.stride;
	}
	return result;
}

int kobox_linux_drm_virtgpu_resource_info(
	struct kobox_linux_drm_file *file,
	struct kobox_linux_virtgpu_resource_info *resource)
{
	struct drm_virtgpu_resource_info argument;
	int result;

	if (!resource || !resource->bo_handle)
		return -EINVAL;
	argument = (struct drm_virtgpu_resource_info) {
		.bo_handle = resource->bo_handle,
	};
	result = virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &argument);
	if (!result) {
		resource->resource_handle = argument.res_handle;
		resource->size = argument.size;
		resource->blob_memory = argument.blob_mem;
	}
	return result;
}

int kobox_linux_drm_virtgpu_transfer(struct kobox_linux_drm_file *file,
				     bool from_host,
				     const struct kobox_linux_virtgpu_transfer *transfer)
{
	struct drm_virtgpu_3d_transfer_from_host argument;

	if (!transfer || !transfer->bo_handle)
		return -EINVAL;
	argument = (struct drm_virtgpu_3d_transfer_from_host) {
		.bo_handle = transfer->bo_handle,
		.box = {.x = transfer->x, .y = transfer->y, .z = transfer->z,
			.w = transfer->width, .h = transfer->height, .d = transfer->depth},
		.level = transfer->level, .offset = transfer->offset,
		.stride = transfer->stride, .layer_stride = transfer->layer_stride,
	};
	return virtgpu_ioctl(file, from_host ? DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST :
		DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &argument);
}

int kobox_linux_drm_virtgpu_wait(struct kobox_linux_drm_file *file,
				 u32 handle, u32 flags)
{
	struct drm_virtgpu_3d_wait argument = {.handle = handle, .flags = flags};

	if (!handle || (flags & ~VIRTGPU_WAIT_NOWAIT))
		return -EINVAL;
	return virtgpu_ioctl(file, DRM_IOCTL_VIRTGPU_WAIT, &argument);
}

int kobox_linux_drm_map_pages(struct kobox_linux_drm_file *file,
			      u32 handle, u32 mapping_rights,
			      u64 *page_indices, size_t page_capacity,
			      struct kobox_linux_drm_map_pages *result,
			      struct kobox_linux_drm_mapping **owner)
{
	struct kobox_linux_drm_mapping *mapping;
	const struct kobox_linux_drm_mapping_operations *operations;
	struct module *module;
	int error;

	if (!handle || !mapping_rights || (mapping_rights & ~3U) ||
	    !page_indices || !page_capacity || !result || !owner || *owner)
		return -EINVAL;
	error = check_owner(file);
	if (error)
		return error;
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;
	mutex_lock(&mapping_provider_lock);
	operations = mapping_provider;
	module = mapping_provider_module;
	if (!operations || !try_module_get(module)) {
		error = -ENODEV;
		goto unlock;
	}
	error = operations->map(file->guard, handle, mapping_rights, page_indices,
		page_capacity, result, &mapping->private_mapping);
	if (error) {
		module_put(module);
		goto unlock;
	}
	mapping->operations = operations;
	mapping->module = module;
	mapping->owner = current;
	get_task_struct(mapping->owner);
	mapping->files = current->files;
	*owner = mapping;

unlock:
	mutex_unlock(&mapping_provider_lock);
	if (error)
		kfree(mapping);
	return error;
}

int kobox_linux_drm_mapping_release(
	struct kobox_linux_drm_mapping **owner)
{
	struct kobox_linux_drm_mapping *mapping;

	if (!owner || !*owner)
		return -EINVAL;
	mapping = *owner;
	if (!valid_context() || current != mapping->owner ||
	    current->files != mapping->files)
		return -EPERM;
	mapping->operations->release(mapping->private_mapping);
	/* The final GEM reference can enqueue the backing file's last fput. */
	task_work_run();
	module_put(mapping->module);
	put_task_struct(mapping->owner);
	kfree(mapping);
	*owner = NULL;
	return 0;
}

int kobox_linux_drm_close(struct kobox_linux_drm_file **owner)
{
	struct kobox_linux_drm_file *file;
	int result;

	if (!owner || !*owner)
		return -EINVAL;
	file = *owner;
	result = check_owner(file);
	if (result)
		return result;
	/* No other task or raw FD consumer can enter an ioctl. The guard keeps
	 * close_fd from deferring the last put to task_work after module unload.
	 * A flush error still consumes the installed FD; never retry close_fd.
	 */
	result = close_fd(file->fd);
	__fput_sync(file->guard);
	task_work_run();
	kern_unmount(file->mount);
	put_task_struct(file->owner);
	kfree(file);
	*owner = NULL;
	return result;
}
