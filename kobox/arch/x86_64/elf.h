/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_X86_64_ELF_H
#define KOBOX_X86_64_ELF_H

#include <stdbool.h>
#include <stddef.h>

bool kobox_x86_64_elf_matches(const void *data, size_t size, bool core);

#endif
