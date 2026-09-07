/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_ALLOCATION_GATE_H
#define KOBOX_LINUX_ALLOCATION_GATE_H

struct vfsmount;

/* Test harness only: configure upstream fault injection around a callback.
 * The callback must clear every task's fail_nth before returning.
 */
int kobox_linux_with_allocation_failures(int (*run)(void *), void *argument);
struct vfsmount *kobox_linux_allocation_mount(void);

#endif
