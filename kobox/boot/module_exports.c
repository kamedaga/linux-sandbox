// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <asm/current.h>
#include <asm/irqflags.h>
#include <asm/percpu.h>
#include <asm/preempt.h>
#include <asm/io.h>
#include "../mm/port.h"
#include "resource_port.h"

/* Hosted module instructions use the same arch boundary as built-in code.
 * Upstream modpost generates the actual ksymtab and symbol-version records.
 */
EXPORT_SYMBOL_GPL(kobox_provider_current_task);
EXPORT_SYMBOL_GPL(kobox_provider_current_percpu_offset);
EXPORT_SYMBOL_GPL(kobox_provider_irq_save_flags);
EXPORT_SYMBOL_GPL(kobox_provider_preempt_save);
EXPORT_SYMBOL_GPL(kobox_provider_preempt_restore);
EXPORT_SYMBOL_GPL(kobox_vm_space_create);
EXPORT_SYMBOL_GPL(kobox_vm_space_bind);
EXPORT_SYMBOL_GPL(kobox_vm_space_destroy);
EXPORT_SYMBOL_GPL(kobox_vm_resolve_fault);
EXPORT_SYMBOL_GPL(kobox_linux_resource_bind);
EXPORT_SYMBOL_GPL(kobox_mmio_read);
EXPORT_SYMBOL_GPL(kobox_mmio_write);
