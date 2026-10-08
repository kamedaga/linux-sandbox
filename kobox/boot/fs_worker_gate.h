/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_WORKER_GATE_H
#define KOBOX_BOOT_FS_WORKER_GATE_H

struct vfsmount;
struct cred;
int kobox_linux_fs_workers_verify(struct vfsmount *root, const struct cred *cred);

#endif /* KOBOX_BOOT_FS_WORKER_GATE_H */
