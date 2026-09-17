// SPDX-License-Identifier: GPL-2.0-only

#include "resource_port.h"
#include "../host/posix/core.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct state {
	struct kobox_linux_resource_binding reply;
	int error;
};

static int lookup(void *context, const char *name, uint32_t slot, size_t index,
		  uint64_t rights, const uint8_t digest[32],
		  struct kobox_linux_resource_binding *binding)
{
	struct state *state = context;

	if (name || slot != 1 || index || rights != 1 || digest[0] != 7)
		return -EINVAL;
	*binding = state->reply;
	return state->error;
}

static void *core_symbol(struct kobox_posix_core *core, const char *name)
{
	const struct kobox_fixed_image *image = kobox_posix_core_image(core);
	size_t offset;

	if (!image || kobox_fixed_image_symbol(image, name, &offset) !=
	    KOBOX_FIXED_IMAGE_OK)
		return NULL;
	return (unsigned char *)kobox_posix_core_base(core) + offset;
}

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "resource port check failed at %u\n", __LINE__); \
		return 1; \
	} \
} while (0)

int main(int argc, char **argv)
{
	struct state state = {0};
	struct kobox_linux_resource_binding binding;
	struct kobox_linux_resource_port port = {
		.size = sizeof(port), .generation = 17, .context = &state, .lookup = lookup,
	};
	uint8_t digest[32] = {7};
	int (*install)(const struct kobox_linux_resource_port *);
	/* NULL selects the built-in core, so this test needs no Linux struct layout. */
	int (*bind)(const void *, uint32_t, size_t, uint64_t, const uint8_t *,
		    struct kobox_linux_resource_binding *);
	struct kobox_posix_core *core = NULL;
	void *symbol;

	CHECK(argc == 2);
	CHECK(!kobox_posix_core_open(argv[1], &core));
	symbol = core_symbol(core, "kobox_linux_resource_port_install");
	CHECK(symbol);
	memcpy(&install, &symbol, sizeof(install));
	symbol = core_symbol(core, "kobox_linux_resource_bind");
	CHECK(symbol);
	memcpy(&bind, &symbol, sizeof(bind));
	CHECK(bind(NULL, 1, 0, 1, digest, &binding) == -ENODEV);
	CHECK(!binding.object);
	port.generation = 0;
	CHECK(install(&port) == -EINVAL);
	port.generation = 17;
	CHECK(!install(&port));
	CHECK(install(&port) == -EBUSY);
	/* The install owns a copy of the immutable port description. */
	port.generation = 18;
	port.lookup = NULL;
	state.reply = (struct kobox_linux_resource_binding) {
		.generation = 17, .object_id = 101, .rights = 1,
		.object = &state, .operations = &state,
	};
	CHECK(!bind(NULL, 1, 0, 1, digest, &binding));
	CHECK(binding.generation == 17 && binding.object_id == 101);
	state.reply.generation = 16;
	CHECK(bind(NULL, 1, 0, 1, digest, &binding) == -EPROTO && !binding.object);
	state.reply.generation = 17;
	state.reply.rights = 0;
	CHECK(bind(NULL, 1, 0, 1, digest, &binding) == -EPROTO && !binding.object);
	state.reply.rights = 1;
	state.error = 1;
	CHECK(bind(NULL, 1, 0, 1, digest, &binding) == -EPROTO && !binding.object);
	state.error = -EACCES;
	CHECK(bind(NULL, 1, 0, 1, digest, &binding) == -EACCES && !binding.object);
	CHECK(!kobox_posix_core_close(&core));
	puts("resource port binary boundary passed (not a device/backend Gate)");
	return 0;
}
