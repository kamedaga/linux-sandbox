// SPDX-License-Identifier: GPL-2.0-only

#include "module_launch.h"
#include "diagnostic.h"
#include "../arch/x86_64/host_call.h"

#ifndef KOBOX_BOOT_GPU
#define KOBOX_BOOT_GPU 1
#endif
#ifndef KOBOX_BOOT_NET
#define KOBOX_BOOT_NET 0
#endif
#ifndef KOBOX_BOOT_BLOCK
#define KOBOX_BOOT_BLOCK 0
#endif
#ifndef KOBOX_BOOT_FS
#define KOBOX_BOOT_FS 0
#endif
#ifndef KOBOX_BOOT_VIRTIO_GPU
#define KOBOX_BOOT_VIRTIO_GPU 1
#endif
/* A PCI-only link profile must not implicitly become the USB input service. */
#ifndef KOBOX_BOOT_INPUT
#define KOBOX_BOOT_INPUT (!KOBOX_BOOT_GPU && !KOBOX_BOOT_NET)
#endif

#include <linux/errno.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <asm/ptrace.h>
#include "../../kernel/module/internal.h"

long __x64_sys_init_module(const struct pt_regs *regs);
long __x64_sys_delete_module(const struct pt_regs *regs);
void flush_module_init_free_work(void);

struct module_session {
	struct notifier_block notifier;
	struct task_struct *owner;
	const char *expected;
};

static DEFINE_MUTEX(launch_lock);
static bool launch_consumed;

static void report_progress(const struct kobox_linux_module_launch *launch,
			    unsigned phase, size_t module_index, int status)
{
	if (launch->progress)
		kobox_host_call((launch->progress(launch->progress_context,
					    phase, module_index, status), 0));
}

static int admit_module(struct notifier_block *notifier,
			unsigned long state, void *data)
{
	struct module_session *session = container_of(notifier,
					struct module_session, notifier);
	const struct module *module = data;
	const char *expected = READ_ONCE(session->expected);

	if (state != MODULE_STATE_COMING)
		return NOTIFY_DONE;
	/* Upstream calls the robust COMING chain before module parameters,
	 * constructors, or init can execute. No ELF/struct-module parsing here.
	 * A fixed closure cannot admit nested or background module loads.
	 */
	if (current != session->owner || !expected)
		return notifier_from_errno(-EPERM);
	if (strcmp(module->name, expected))
		return notifier_from_errno(-EKEYREJECTED);
	return NOTIFY_OK;
}

int kobox_linux_modules_run(const struct kobox_linux_module_launch *launch,
			   struct kobox_linux_module_launch_report *report)
{
	struct module_session session = {
		.notifier = {.notifier_call = admit_module, .priority = INT_MAX},
		.owner = current,
	};
	struct pt_regs regs = {0};
#if KOBOX_BOOT_GPU
	struct kobox_linux_device_session *device = NULL;
#if !KOBOX_BOOT_VIRTIO_GPU
	struct kobox_linux_lifecycle inspected_lifecycle;
#endif
#elif KOBOX_BOOT_NET
	struct kobox_linux_net_port *net_port = NULL;
#elif KOBOX_BOOT_INPUT
	struct kobox_linux_input_port *input_port = NULL;
#endif
#if KOBOX_BOOT_BLOCK
	struct kobox_linux_block_port *block_port = NULL;
#endif
#if KOBOX_BOOT_BLOCK || KOBOX_BOOT_FS
	struct kobox_linux_io_service io_service = {0};
#endif
#if KOBOX_BOOT_FS
	struct kobox_linux_fs_mount *filesystem = NULL;
#endif
	struct kobox_linux_device_port **pci_devices = NULL;
#if KOBOX_BOOT_FS
	struct pci_dev **filesystem_devices = NULL;
#endif
	struct kobox_linux_firmware_store *firmware_store = NULL;
	void *service = NULL;
	size_t index, prior;
	int result;

	if (!launch || launch->size != sizeof(*launch) || !launch->modules ||
	    !launch->count || launch->count > 64 || !launch->lifecycle ||
	    !report || report->size != sizeof(*report) ||
	    (launch->device && launch->pci_device_count) ||
	    (!!launch->pci_devices != !!launch->pci_device_count) ||
	    (!KOBOX_BOOT_GPU && launch->device) ||
	    (launch->capture_input && launch->pci_device_count != 1) ||
	    (launch->capture_network && launch->pci_device_count != 1) ||
	    (launch->capture_block && launch->pci_device_count != 1) ||
	    (launch->block_write && !launch->capture_block) ||
	    (launch->block_write && !launch->block_write_key.diskseq) ||
	    (!launch->block_write && (launch->block_write_key.diskseq ||
		launch->block_write_key.major || launch->block_write_key.minor)) ||
	    (launch->capture_input && launch->capture_network) ||
	    (!!launch->progress != !!launch->progress_context) ||
	    launch->capture_input > 1 ||
	    launch->capture_network > 1 ||
	    launch->capture_block > 1 || launch->block_write > 1 ||
	    (KOBOX_BOOT_GPU && (launch->capture_input || launch->capture_network)) ||
	    (!KOBOX_BOOT_INPUT && launch->capture_input) ||
	    (!KOBOX_BOOT_NET && launch->capture_network) ||
	    (!KOBOX_BOOT_BLOCK && launch->capture_block) ||
	    (launch->filesystem && (!KOBOX_BOOT_FS || !launch->pci_device_count ||
		launch->capture_block || launch->capture_input || launch->capture_network)))
		return -EINVAL;
	if ((!launch->firmware && launch->firmware_count) ||
	    launch->firmware_count > KOBOX_FIRMWARE_MAX_FILES)
		return -EINVAL;
	*report = (struct kobox_linux_module_launch_report) {.size = sizeof(*report)};
	for (index = 0; index < launch->count; index++) {
		const struct kobox_linux_native_module *module = &launch->modules[index];

		if (!module->image || !module->length || !module->name ||
		    !module->name[0] || strnlen(module->name, MODULE_NAME_LEN) == MODULE_NAME_LEN)
			return -EINVAL;
		for (prior = 0; prior < index; prior++)
			if (!strcmp(module->name, launch->modules[prior].name))
				return -EEXIST;
	}
	if (!mutex_trylock(&launch_lock))
		return -EBUSY;
	if (launch_consumed) {
		result = -EALREADY;
		goto unlock;
	}
	launch_consumed = true;
	if (launch->pci_device_count) {
		pci_devices = kcalloc(launch->pci_device_count,
				      sizeof(*pci_devices), GFP_KERNEL);
		if (!pci_devices) {
			result = -ENOMEM;
			goto unlock;
		}
	}
	result = kobox_linux_firmware_install(launch->firmware,
					      launch->firmware_count,
					      &firmware_store);
	if (result)
		goto unlock;
#if KOBOX_BOOT_GPU
	if (launch->device) {
		result = kobox_linux_device_prepare(launch->device, &device);
		if (result)
			goto finish_device;
	} else
#endif
	for (index = 0; index < launch->pci_device_count; index++) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_PCI_PREPARE, index, 0);
		result = kobox_linux_device_port_prepare(&launch->pci_devices[index],
						 &pci_devices[index]);
		if (result)
			goto finish_device;
		report_progress(launch, KOBOX_MODULE_PROGRESS_PCI_READY, index, 0);
	}
	result = register_module_notifier(&session.notifier);
	if (result)
		goto finish_device;
	for (index = 0; index < launch->count; index++) {
		const struct kobox_linux_native_module *module = &launch->modules[index];

		WRITE_ONCE(session.expected, module->name);
		report_progress(launch, KOBOX_MODULE_PROGRESS_MODULE_BEGIN, index, 0);
		regs.di = (unsigned long)module->image;
		regs.si = module->length;
		regs.dx = (unsigned long)(module->parameters ?: "");
		result = __x64_sys_init_module(&regs);
		WRITE_ONCE(session.expected, NULL);
		if (result) {
			report_progress(launch, KOBOX_MODULE_PROGRESS_MODULE_FAILED,
					index, result);
			break;
		}
		report->loaded++;
#if KOBOX_BOOT_INPUT
		/* USB enumeration is asynchronous, so register before xHCI probes. */
		if (launch->capture_input && !strcmp(module->name, "usbcore")) {
			result = kobox_linux_input_port_open(&input_port);
			if (result)
				break;
		}
#endif
#if KOBOX_BOOT_GPU
		if (device) {
			result = kobox_linux_device_module_ready(device);
			if (result)
				break;
		}
#endif
	}
	if (!result)
		report_progress(launch, KOBOX_MODULE_PROGRESS_MODULES_LOADED,
				report->loaded, 0);
#if KOBOX_BOOT_GPU
	if (!result && device)
		result = kobox_linux_device_ready(device, &report->device);
#endif
	if (!result && launch->pci_device_count) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_PROBE_WAIT, 0, 0);
		wait_for_device_probe();
		for (index = 0; !result && index < launch->pci_device_count; index++) {
			struct pci_dev *pci = kobox_linux_device_port_pci(pci_devices[index]);
			u32 expected = launch->pci_devices[index].expected_class;

			if (!pci || !pci->driver || (expected && pci->class != expected))
				result = -ENODEV;
			else {
				report->pci_bound++;
				report_progress(launch, KOBOX_MODULE_PROGRESS_PROBE_BOUND, index, 0);
			}
		}
	}
#if KOBOX_BOOT_INPUT
	if (!result && launch->capture_input && !input_port)
		result = -ENODEV;
	if (!result)
		service = input_port;
#endif
#if KOBOX_BOOT_BLOCK
	if (!result && launch->capture_block)
		result = kobox_linux_block_port_open(&block_port,
			kobox_linux_device_port_pci(pci_devices[0]),
			launch->block_write ? &launch->block_write_key : NULL);
	if (!result) {
#if KOBOX_BOOT_INPUT
		io_service.input = input_port;
#endif
		io_service.block = block_port;
		service = &io_service;
	}
#endif
#if KOBOX_BOOT_NET
	if (!result && launch->capture_network) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_NET_OPEN, 0, 0);
		result = kobox_linux_net_port_open(&net_port,
				kobox_linux_device_port_pci(pci_devices[0]),
				launch->progress, launch->progress_context);
		if (!result)
			report_progress(launch, KOBOX_MODULE_PROGRESS_NET_READY, 0, 0);
	}
	if (!result)
		service = net_port;
#endif
#if KOBOX_BOOT_FS
	if (!result && launch->filesystem) {
		filesystem_devices = kcalloc(launch->pci_device_count,
					      sizeof(*filesystem_devices), GFP_KERNEL);
		if (!filesystem_devices)
			result = -ENOMEM;
		else {
			for (index = 0; index < launch->pci_device_count; index++)
				filesystem_devices[index] = kobox_linux_device_port_pci(pci_devices[index]);
			result = kobox_linux_fs_mount_open(&filesystem, launch->filesystem,
					filesystem_devices, launch->pci_device_count);
		}
		if (!result) {
			io_service.filesystem = kobox_linux_fs_mount_service(filesystem);
			service = &io_service;
		}
	}
#endif
#if KOBOX_BOOT_GPU
	service = kobox_linux_device_service(device);
#endif
	if (!result) {
		const struct kobox_linux_lifecycle *lifecycle = launch->lifecycle;

#if KOBOX_BOOT_GPU && !KOBOX_BOOT_VIRTIO_GPU
		/* The normal device lifecycle remains unchanged. Only the one-shot
		 * physical-GPU inspection has an owner-task sensor/time monitor.
		 */
		if (device) {
			inspected_lifecycle = *launch->lifecycle;
			inspected_lifecycle.monitor =
				kobox_linux_device_inspection_monitor;
			inspected_lifecycle.monitor_context = device;
			lifecycle = &inspected_lifecycle;
		}
#endif
		report_progress(launch, KOBOX_MODULE_PROGRESS_LIFECYCLE_READY, 0, 0);
		result = kobox_linux_lifecycle_serve(lifecycle, service);
	}
#if KOBOX_BOOT_INPUT
	/* The handler must be gone before HID/USB modules can be unloaded. */
	if (input_port)
		kobox_linux_input_port_close(input_port);
#endif
#if KOBOX_BOOT_BLOCK
	if (block_port)
		kobox_linux_block_port_close(block_port);
#endif
#if KOBOX_BOOT_FS
	if (filesystem)
		report->cleanup_result = kobox_linux_fs_mount_close(filesystem);
#endif
#if KOBOX_BOOT_NET
	if (net_port)
		kobox_linux_net_port_close(net_port);
#endif
#if KOBOX_BOOT_GPU
	if (device) {
		report->cleanup_result = kobox_linux_device_quiesce(device,
							 &report->device);
		if (report->cleanup_result)
			kobox_linux_boot_diagnostic(
				"kobox-modules: cleanup quiesce status=%d files_active=%u files_close_error=%d render_closed=%u\n",
				report->cleanup_result, report->device.files.active,
				report->device.files.close_error,
				report->device.render_closed);
	}
#endif
	if (report->cleanup_result)
		goto unregister_notifier;
	/* Hosted lifecycle calls do not cross Linux's syscall-return boundary.
	 * Completed block reads can therefore still own a driver module through
	 * fput's task_work/delayed-fput lists. After service quiescence and port
	 * close, drain both before checking module references; a busy module
	 * after this boundary remains a real cleanup failure, not a retry case.
	 */
	task_work_run();
	flush_delayed_fput();
	for (index = report->loaded; index; index--) {
		regs.di = (unsigned long)launch->modules[index - 1].name;
		regs.si = O_NONBLOCK;
		report->cleanup_result = __x64_sys_delete_module(&regs);
		if (report->cleanup_result) {
			kobox_linux_boot_diagnostic(
				"kobox-modules: cleanup unload module=%s loaded=%zu unloaded=%zu status=%d\n",
				launch->modules[index - 1].name, report->loaded,
				report->unloaded, report->cleanup_result);
			break;
		}
		report->unloaded++;
	}
	flush_module_init_free_work();
	rcu_barrier();
unregister_notifier:
	unregister_module_notifier(&session.notifier);
finish_device:
	/* Failed unload still owns ports and backing. The host must retire this
	 * process and revoke its device; it cannot reuse a partial generation.
	 */
#if KOBOX_BOOT_GPU
	if (device && !report->cleanup_result &&
	    report->loaded == report->unloaded) {
		report->cleanup_result = kobox_linux_device_finish(device,
								&report->device);
		if (report->cleanup_result)
			kobox_linux_boot_diagnostic(
				"kobox-modules: cleanup device status=%d files_active=%u files_close_error=%d render_closed=%u\n",
				report->cleanup_result, report->device.files.active,
				report->device.files.close_error,
				report->device.render_closed);
	}
#endif
	if (!report->cleanup_result && report->loaded == report->unloaded) {
		for (index = launch->pci_device_count; index; index--) {
			if (!pci_devices[index - 1])
				continue;
			report->cleanup_result = kobox_linux_device_port_finish(pci_devices[index - 1]);
			if (report->cleanup_result) {
				kobox_linux_boot_diagnostic(
					"kobox-modules: cleanup PCI port=%zu status=%d\n",
					index - 1, report->cleanup_result);
				break;
			}
			pci_devices[index - 1] = NULL;
			report->pci_detached++;
		}
	}
	if (!report->cleanup_result && report->loaded == report->unloaded) {
		report->cleanup_result =
			kobox_linux_firmware_remove(&firmware_store);
		if (report->cleanup_result)
			kobox_linux_boot_diagnostic(
				"kobox-modules: cleanup firmware status=%d\n",
				report->cleanup_result);
	}
unlock:
#if KOBOX_BOOT_FS
	kfree(filesystem_devices);
#endif
	kfree(pci_devices);
	mutex_unlock(&launch_lock);
	report->result = result ?: report->cleanup_result;
	if (report->result)
		kobox_linux_boot_diagnostic(
			"kobox-modules: launch result=%d cleanup=%d loaded=%zu unloaded=%zu\n",
			report->result, report->cleanup_result,
			report->loaded, report->unloaded);
	return report->result;
}
