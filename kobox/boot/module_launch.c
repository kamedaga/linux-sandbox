// SPDX-License-Identifier: GPL-2.0-only

#include "module_launch.h"

#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
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
	struct kobox_linux_device_session *device = NULL;
	size_t index, prior;
	int result;

	if (!launch || launch->size != sizeof(*launch) || !launch->modules ||
	    !launch->count || launch->count > 64 || !launch->lifecycle ||
	    !report || report->size != sizeof(*report))
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
	if (launch->device) {
		result = kobox_linux_device_prepare(launch->device, &device);
		if (result)
			goto finish_device;
	}
	result = register_module_notifier(&session.notifier);
	if (result)
		goto finish_device;
	for (index = 0; index < launch->count; index++) {
		const struct kobox_linux_native_module *module = &launch->modules[index];

		WRITE_ONCE(session.expected, module->name);
		regs.di = (unsigned long)module->image;
		regs.si = module->length;
		regs.dx = (unsigned long)(module->parameters ?: "");
		result = __x64_sys_init_module(&regs);
		WRITE_ONCE(session.expected, NULL);
		if (result)
			break;
		report->loaded++;
	}
	if (!result && device)
		result = kobox_linux_device_ready(device, &report->device);
	if (!result)
		result = kobox_linux_lifecycle_serve(launch->lifecycle,
						   kobox_linux_device_service(device));
	if (device)
		report->cleanup_result = kobox_linux_device_quiesce(device,
							 &report->device);
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
	if (device && !report->cleanup_result &&
	    report->loaded == report->unloaded)
		report->cleanup_result = kobox_linux_device_finish(device,
								&report->device);
unlock:
	mutex_unlock(&launch_lock);
	report->result = result ?: report->cleanup_result;
	return report->result;
}
