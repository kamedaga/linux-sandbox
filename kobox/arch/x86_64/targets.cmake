# SPDX-License-Identifier: GPL-2.0-only

add_library(kobox_arch_elf STATIC arch/x86_64/elf.c)

target_compile_options(kobox_arch_elf PRIVATE -Wall -Wextra -Wpedantic -Werror)
