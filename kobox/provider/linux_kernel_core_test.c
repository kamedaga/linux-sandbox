// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "core_lifecycle.h"
#include "../runtime/memory_resource_interface.h"

#include <kobox2/closure_layout.h>
#include <kobox2/memory_arena_layout.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_ARENA_SIZE (64u * 1024u * 1024u)
#define TEST_GENERATION UINT64_C(67)
#define TEST_OBJECT_ID UINT64_C(109)

#if defined(__clang__)
#define KOBOX_MANUAL_ELF_CALL __attribute__((no_sanitize("function")))
#else
#define KOBOX_MANUAL_ELF_CALL
#endif

struct test_memory {
	void *address;
	size_t length;
};

static struct test_memory test_memory;

static int mapped_range(void *object, void **address_out, size_t *length_out)
{
	struct test_memory *memory = object;

	if (memory != &test_memory || !address_out || !length_out)
		return -1;
	*address_out = memory->address;
	*length_out = memory->length;
	return 0;
}

static const struct kobox_memory_arena_resource_operations memory_operations = {
	.base = {
		.size = sizeof(memory_operations),
		.identity = KB2_MEMORY_ARENA_ABI_IDENTITY_BYTES,
	},
	.mapped_range = mapped_range,
};

static int resource_count(const struct kobox_module_context *context,
			  uint32_t slot_id, uint32_t *state_out,
			  size_t *count_out)
{
	if (!context || slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID ||
	    !state_out || !count_out)
		return KOBOX_MODULE_RESOURCE_NOT_VISIBLE;
	*state_out = KOBOX_MODULE_RESOURCE_PRESENT_STATE;
	*count_out = 1;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_acquire(
	const struct kobox_module_context *context, uint32_t slot_id,
	size_t object_index, uint64_t required_rights,
	struct kobox_module_resource_handle *handle_out)
{
	if (!context || slot_id != KOBOX_LINUX_CORE_MEMORY_SLOT_ID ||
	    object_index || required_rights != KB2_MEMORY_ARENA_REQUIRED_RIGHTS ||
	    !handle_out)
		return KOBOX_MODULE_RESOURCE_INVALID_ARGUMENT;
	*handle_out = (struct kobox_module_resource_handle){
		.generation = context->generation,
		.object_id = TEST_OBJECT_ID,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_bind(
	const struct kobox_module_context *context,
	struct kobox_module_resource_handle handle,
	const uint8_t expected_digest[KOBOX_MODULE_RESOURCE_INTERFACE_DIGEST_SIZE],
	struct kobox_module_resource_binding *binding_out)
{
	static const uint8_t digest[KB2_MEMORY_ARENA_SCHEMA_DIGEST_SIZE] =
		KB2_MEMORY_ARENA_SCHEMA_SHA256_BYTES;

	if (!context || handle.generation != context->generation ||
	    handle.object_id != TEST_OBJECT_ID || !expected_digest ||
	    memcmp(expected_digest, digest, sizeof(digest)) || !binding_out)
		return KOBOX_MODULE_RESOURCE_INTERFACE;
	binding_out->operations = &memory_operations.base;
	binding_out->object = &test_memory;
	return KOBOX_MODULE_RESOURCE_OK;
}

static int resource_info(const struct kobox_module_context *context,
			 struct kobox_module_resource_handle handle,
			 struct kobox_module_resource_info *info_out)
{
	if (!context || handle.generation != context->generation ||
	    handle.object_id != TEST_OBJECT_ID || !info_out)
		return KOBOX_MODULE_RESOURCE_STALE;
	*info_out = (struct kobox_module_resource_info){
		.resource_type = KB2_CLOSURE_RESOURCE_MEMORY,
		.granted_rights = KB2_MEMORY_ARENA_REQUIRED_RIGHTS,
	};
	return KOBOX_MODULE_RESOURCE_OK;
}

static const struct kobox_module_runtime_operations runtime_operations = {
	.size = sizeof(runtime_operations),
	.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
	.resource_count = resource_count,
	.resource_acquire = resource_acquire,
	.resource_bind = resource_bind,
	.resource_info = resource_info,
};

static int symbol_function(void *handle, const char *name, void *function_out,
			   size_t function_size)
{
	void *address;

	dlerror();
	address = dlsym(handle, name);
	if (dlerror() || !address || function_size != sizeof(address))
		return -1;
	memcpy(function_out, &address, sizeof(address));
	return 0;
}

KOBOX_MANUAL_ELF_CALL static int run(const char *core_provider_path,
				     const char *pci_provider_path)
{
	int (*core_init)(const struct kobox_module_context *context);
	int (*kernel_init)(const struct kobox_module_context *context);
	int (*kernel_active)(const struct kobox_module_context *context);
	const struct kb2_core_directory *directory;
	struct kobox_module_context context;
	void *provider;
	void *pci_provider;
	int (*pci_core_init)(const struct kobox_module_context *context);
	int (*pci_core_active)(const struct kobox_module_context *context);
	int status;

	provider = dlopen(core_provider_path, RTLD_NOW | RTLD_GLOBAL);
	if (!provider) {
		fprintf(stderr, "cannot load core provider: %s\n", dlerror());
		return -1;
	}
	directory = dlsym(provider, "kobox_linux_core_directory");
	if (!directory ||
	    symbol_function(provider, "kobox_linux_core_init", &core_init,
			    sizeof(core_init)) ||
	    symbol_function(provider, "kobox_linux_core_kernel_init", &kernel_init,
			    sizeof(kernel_init)) ||
	    symbol_function(provider, "kobox_linux_core_kernel_active",
			    &kernel_active, sizeof(kernel_active)))
		return -1;
	context = (struct kobox_module_context){
		.size = sizeof(context),
		.identity = KOBOX_MODULE_INTERFACE_IDENTITY_INITIALIZER,
		.generation = TEST_GENERATION,
		.node_id = 1,
		.resource_view = &test_memory,
		.runtime_operations = &runtime_operations,
		.core_operations = directory,
		.logical_cpu_count = 2,
	};
	status = core_init(&context);
	if (status) {
		fprintf(stderr, "core init status=%d\n", status);
		return -1;
	}
	status = kernel_init(&context);
	if (status) {
		fprintf(stderr, "Linux kernel init status=%d\n", status);
		return -1;
	}
	if (kernel_active(&context) != 1)
		return -1;
	pci_provider = dlopen(pci_provider_path, RTLD_NOW | RTLD_GLOBAL);
	if (!pci_provider) {
		fprintf(stderr, "cannot load PCI provider: %s\n", dlerror());
		return -1;
	}
	if (symbol_function(pci_provider, "kobox_linux_device_pci_core_init",
			    &pci_core_init, sizeof(pci_core_init)) ||
	    symbol_function(pci_provider, "kobox_linux_device_pci_core_active",
			    &pci_core_active, sizeof(pci_core_active)))
		return -1;
	status = pci_core_init(&context);
	if (status) {
		fprintf(stderr, "Linux PCI core init status=%d\n", status);
		return -1;
	}
	if (pci_core_active(&context) != 1)
		return -1;
	return 0;
}

int main(int argument_count, char **arguments)
{
	int status;

	if (argument_count != 3)
		return 2;
	test_memory.length = TEST_ARENA_SIZE;
	test_memory.address = mmap(NULL, test_memory.length,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (test_memory.address == MAP_FAILED)
		return 1;
	status = run(arguments[1], arguments[2]);
	if (status)
		fprintf(stderr, "Linux kernel core initialization failed\n");
	return status ? 1 : 0;
}
