// SPDX-License-Identifier: GPL-2.0-only

#include <linux/uaccess.h>
#include "../memory/port.h"

bool copy_from_kernel_nofault_allowed(const void *source, size_t size)
{
	/* Hosted kernel aliases occupy positive native addresses, so upstream
	 * x86's TASK_SIZE split rejects real dentries and makes d_path return
	 * fault placeholders. Recognize only the machine's kernel windows,
	 * not arbitrary native input or a failed client-MM lookup. The copy
	 * itself and fault recovery remain upstream maccess/extable code.
	 */
	return kobox_linux_memory_native_range((unsigned long)source, size);
}
