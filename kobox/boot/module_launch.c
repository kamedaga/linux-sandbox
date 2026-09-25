// SPDX-License-Identifier: GPL-2.0-only

#include "module_launch.h"
#include "../arch/x86_64/host_call.h"

#ifndef KOBOX_BOOT_GPU
#define KOBOX_BOOT_GPU 1
#endif
#ifndef KOBOX_BOOT_NET
#define KOBOX_BOOT_NET 0
#endif

#include <linux/errno.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
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
#else
#if KOBOX_BOOT_NET
	struct kobox_linux_net_port *net_port = NULL;
#else
	struct kobox_linux_input_port *input_port = NULL;
#endif
#endif
	struct kobox_linux_device_port *pci_device = NULL;
	void *service = NULL;
	size_t index, prior;
	int result;

	if (!launch || launch->size != sizeof(*launch) || !launch->modules ||
	    !launch->count || launch->count > 64 || !launch->lifecycle ||
	    !report || report->size != sizeof(*report) ||
	    (launch->device && launch->pci_device) ||
	    (!KOBOX_BOOT_GPU && launch->device) ||
	    (launch->capture_input && !launch->pci_device) ||
	    (launch->capture_network && !launch->pci_device) ||
	    (launch->capture_input && launch->capture_network) ||
	    (!!launch->progress != !!launch->progress_context) ||
	    launch->capture_input > 1 ||
	    launch->capture_network > 1 ||
	    (KOBOX_BOOT_GPU && (launch->capture_input || launch->capture_network)) ||
	    (KOBOX_BOOT_NET && launch->capture_input) ||
	    (!KOBOX_BOOT_NET && launch->capture_network))
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
#if KOBOX_BOOT_GPU
	if (launch->device) {
		result = kobox_linux_device_prepare(launch->device, &device);
		if (result)
			goto finish_device;
	} else
#endif
	if (launch->pci_device) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_PCI_PREPARE, 0, 0);
		result = kobox_linux_device_port_prepare(launch->pci_device,
						 &pci_device);
		if (result)
			goto finish_device;
		report_progress(launch, KOBOX_MODULE_PROGRESS_PCI_READY, 0, 0);
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
#if !KOBOX_BOOT_GPU && !KOBOX_BOOT_NET
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
	if (!result && pci_device) {
		struct pci_dev *pci = kobox_linux_device_port_pci(pci_device);

		report_progress(launch, KOBOX_MODULE_PROGRESS_PROBE_WAIT, 0, 0);
		wait_for_device_probe();
		if (!pci || !pci->driver ||
		    (launch->pci_device->expected_class &&
		     pci->class != launch->pci_device->expected_class))
			result = -ENODEV;
		else {
			report->pci_bound = 1;
			report_progress(launch, KOBOX_MODULE_PROGRESS_PROBE_BOUND, 0, 0);
		}
	}
#if !KOBOX_BOOT_GPU && !KOBOX_BOOT_NET
	if (!result && launch->capture_input && !input_port)
		result = -ENODEV;
	if (!result)
		service = input_port;
#endif
#if KOBOX_BOOT_NET
	if (!result && launch->capture_network) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_NET_OPEN, 0, 0);
		result = kobox_linux_net_port_open(&net_port,
				kobox_linux_device_port_pci(pci_device),
				launch->progress, launch->progress_context);
		if (!result)
			report_progress(launch, KOBOX_MODULE_PROGRESS_NET_READY, 0, 0);
	}
	if (!result)
		service = net_port;
#endif
#if KOBOX_BOOT_GPU
	service = kobox_linux_device_service(device);
#endif
	if (!result) {
		report_progress(launch, KOBOX_MODULE_PROGRESS_LIFECYCLE_READY, 0, 0);
		result = kobox_linux_lifecycle_serve(launch->lifecycle, service);
	}
#if !KOBOX_BOOT_GPU && !KOBOX_BOOT_NET
	/* The handler must be gone before HID/USB modules can be unloaded. */
	if (input_port)
		kobox_linux_input_port_close(input_port);
#endif
#if KOBOX_BOOT_NET
	if (net_port)
		kobox_linux_net_port_close(net_port);
#endif
#if KOBOX_BOOT_GPU
	if (device)
		report->cleanup_result = kobox_linux_device_quiesce(device,
							 &report->device);
#endif
	if (report->cleanup_result)
		goto unregister_notifier;
	for (index = report->loaded; index; index--) {
		regs.di = (unsigned long)launch->modules[index - 1].name;
		regs.si = O_NONBLOCK;
		report->cleanup_result = __x64_sys_delete_module(&regs);
		if (report->cleanup_result)
			break;
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
	    report->loaded == report->unloaded)
		report->cleanup_result = kobox_linux_device_finish(device,
								&report->device);
#endif
	if (pci_device && !report->cleanup_result &&
	    report->loaded == report->unloaded) {
		report->cleanup_result = kobox_linux_device_port_finish(pci_device);
		if (!report->cleanup_result)
			report->pci_detached = 1;
	}
unlock:
	mutex_unlock(&launch_lock);
	report->result = result ?: report->cleanup_result;
	return report->result;
}
