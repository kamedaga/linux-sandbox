// SPDX-License-Identifier: GPL-2.0-only
#include "tls.h"
#include "host_call.h"
#include "../../runtime/host.h"

/* Resolve compiler TLS accesses through the selected loader. Hidden binding
 * prevents this core-local shim from interposing the native libc resolver.
 */
__attribute__((visibility("hidden")))
void *__tls_get_addr(const struct kobox_x86_tls_index *index)
{
	return kobox_host_call(kobox_runtime_tls_address(index->module,
						       index->offset));
}
