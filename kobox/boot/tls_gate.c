// SPDX-License-Identifier: GPL-2.0-only
#include "../runtime/host.h"

unsigned long *kobox_linux_tls_probe(void)
{
	struct kobox_runtime_thread_state *state = kobox_runtime_thread_state();

	if (!state->gate_initialized) {
		state->gate_value = 0x12345678UL;
		state->gate_initialized = 1;
	}
	return &state->gate_value;
}
