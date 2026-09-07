/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MODULE_VISIBILITY_H
#define KOBOX_MODULE_VISIBILITY_H

/* Native module relocations bind all imports through Linux's ksymtab, never
 * a userspace PLT/GOT or interposition. Include before all C declarations so
 * function addresses, as well as direct calls, use PC-relative addressing.
 */
#pragma GCC visibility push(hidden)

#endif
