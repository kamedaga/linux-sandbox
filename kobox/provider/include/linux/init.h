/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_LINUX_INIT_H
#define KOBOX_PROVIDER_LINUX_INIT_H

#include_next <linux/init.h>

/*
 * A hosted shared object cannot discard its init text as the kernel image
 * does.  Let -ffunction-sections split it so closure linking does not retain
 * unrelated boot paths from the same translation unit.
 */
#ifdef KOBOX_PROVIDER_FUNCTION_SECTIONS
#undef __init
#define __init __cold __latent_entropy __no_kstack_erase
#undef __ref
#define __ref noinline
#undef __exit
#define __exit __cold notrace
#endif

#endif /* KOBOX_PROVIDER_LINUX_INIT_H */
