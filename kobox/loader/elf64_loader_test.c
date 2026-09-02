// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "elf64_loader.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int read_file(const char *path, uint8_t **data_out, size_t *size_out)
{
	struct stat status;
	uint8_t *data;
	size_t offset = 0;
	int descriptor;

	*data_out = NULL;
	*size_out = 0;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0 || fstat(descriptor, &status) || status.st_size <= 0)
		return -1;
	data = malloc((size_t)status.st_size);
	if (!data) {
		close(descriptor);
		return -1;
	}
	while (offset < (size_t)status.st_size) {
		ssize_t bytes = read(descriptor, data + offset,
				     (size_t)status.st_size - offset);

		if (bytes <= 0) {
			free(data);
			close(descriptor);
			return -1;
		}
		offset += (size_t)bytes;
	}
	close(descriptor);
	*data_out = data;
	*size_out = (size_t)status.st_size;
	return 0;
}

static int replace_file(int descriptor, const uint8_t *data, size_t size)
{
	size_t offset = 0;

	if (ftruncate(descriptor, 0) || lseek(descriptor, 0, SEEK_SET) != 0)
		return -1;
	while (offset < size) {
		ssize_t bytes = write(descriptor, data + offset, size - offset);

		if (bytes <= 0)
			return -1;
		offset += (size_t)bytes;
	}
	return fsync(descriptor) ? -1 : 0;
}

static int resolve_core(void *context, const char *name, uintptr_t *address_out)
{
	void *symbol;

	dlerror();
	symbol = dlsym(context, name);
	if (!symbol)
		return -1;
	*address_out = (uintptr_t)symbol;
	return 0;
}

static int load_descriptor(int descriptor, void *core,
			   struct kobox_elf64_module *module)
{
	struct kobox_elf64_export exports[] = {
		{ "kobox_fixture_provider_add", KOBOX_ELF64_SYMBOL_FUNCTION, 0 },
		{ "kobox_fixture_provider_cleanup", KOBOX_ELF64_SYMBOL_FUNCTION, 0 },
		{ "kobox_fixture_provider_init", KOBOX_ELF64_SYMBOL_FUNCTION, 0 },
		{ "kobox_fixture_provider_quiesce", KOBOX_ELF64_SYMBOL_FUNCTION, 0 },
	};
	size_t index;

	if (kobox_elf64_module_load_fd(
		    descriptor, exports, ARRAY_SIZE(exports),
		    resolve_core, core, module))
		return -1;
	for (index = 0; index < ARRAY_SIZE(exports); index++) {
		if (!exports[index].address) {
			kobox_elf64_module_unload(module);
			return -1;
		}
	}
	return 0;
}

static int load_must_fail(const char *path, void *core)
{
	struct kobox_elf64_module module;
	int descriptor;
	int status;

	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	status = load_descriptor(descriptor, core, &module);
	close(descriptor);
	if (!status) {
		kobox_elf64_module_unload(&module);
		return -1;
	}
	return 0;
}

int main(int argument_count, char **arguments)
{
	struct kobox_elf64_module module;
	Elf64_Ehdr *header;
	Elf64_Shdr *sections;
	uint8_t *original = NULL;
	uint8_t *modified = NULL;
	size_t size;
	size_t index;
	char temporary[] = "/tmp/kobox-elf-loader-XXXXXX";
	void *core = NULL;
	int descriptor = -1;
	int module_descriptor = -1;
	int stage = 0;
	int result = 1;

	if (argument_count != 3)
		return 2;
	core = dlopen(arguments[1], RTLD_NOW | RTLD_LOCAL);
	module_descriptor = open(arguments[2], O_RDONLY | O_CLOEXEC);
	if (!core || module_descriptor < 0 ||
	    load_descriptor(module_descriptor, core, &module))
		goto out;
	close(module_descriptor);
	module_descriptor = -1;
	stage = 1;
	kobox_elf64_module_unload(&module);
	if (read_file(arguments[2], &original, &size))
		goto out;
	stage = 2;
	modified = malloc(size);
	if (!modified)
		goto out;
	descriptor = mkstemp(temporary);
	if (descriptor < 0)
		goto out;
	stage = 3;

	memcpy(modified, original, size);
	header = (Elf64_Ehdr *)modified;
	header->e_machine = EM_NONE;
	if (replace_file(descriptor, modified, size) ||
	    load_must_fail(temporary, core))
		goto out;
	stage = 4;

	memcpy(modified, original, size);
	header = (Elf64_Ehdr *)modified;
	sections = (Elf64_Shdr *)(modified + header->e_shoff);
	for (index = 0; index < header->e_shnum; index++) {
		if (sections[index].sh_size &&
		    (sections[index].sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) ==
		    (SHF_ALLOC | SHF_EXECINSTR)) {
			sections[index].sh_flags |= SHF_WRITE;
			break;
		}
	}
	if (index == header->e_shnum) {
		stage = 41;
		goto out;
	}
	if (replace_file(descriptor, modified, size)) {
		stage = 42;
		goto out;
	}
	if (load_must_fail(temporary, core)) {
		stage = 43;
		goto out;
	}
	stage = 5;

	memcpy(modified, original, size);
	header = (Elf64_Ehdr *)modified;
	header->e_shoff = UINT64_MAX;
	if (replace_file(descriptor, modified, size) ||
	    load_must_fail(temporary, core))
		goto out;
	stage = 6;
	result = 0;

out:
	if (module_descriptor >= 0)
		close(module_descriptor);
	if (descriptor >= 0)
		close(descriptor);
	unlink(temporary);
	free(modified);
	free(original);
	if (core)
		dlclose(core);
	if (result)
		fprintf(stderr, "ELF64 loader validation test failed at stage %d\n",
			stage);
	return result;
}
