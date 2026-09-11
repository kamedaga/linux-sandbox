# SPDX-License-Identifier: GPL-2.0-only

enable_language(ASM)

add_library(kobox_arch_linux_x86 STATIC
arch/x86_64/linux_signal.c arch/x86_64/linux_ptrace.c)

target_compile_options(kobox_arch_linux_x86 PRIVATE -Wall -Wextra -Wpedantic -Werror)
