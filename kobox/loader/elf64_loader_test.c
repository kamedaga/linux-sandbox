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

static int load_must_fail(const char *path)
{
	struct kobox_elf64_module module;

	if (!kobox_elf64_module_load(path, &module)) {
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
	int stage = 0;
	int result = 1;

	if (argument_count != 3)
		return 2;
	core = dlopen(arguments[1], RTLD_NOW | RTLD_GLOBAL);
	if (!core || kobox_elf64_module_load(arguments[2], &module))
		goto out;
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
	if (replace_file(descriptor, modified, size) || load_must_fail(temporary))
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
	if (load_must_fail(temporary)) {
		stage = 43;
		goto out;
	}
	stage = 5;

	memcpy(modified, original, size);
	header = (Elf64_Ehdr *)modified;
	header->e_shoff = UINT64_MAX;
	if (replace_file(descriptor, modified, size) || load_must_fail(temporary))
		goto out;
	stage = 6;
	result = 0;

out:
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
