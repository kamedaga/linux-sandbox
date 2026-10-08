/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_EXEC_H
#define KOBOX_BOOT_FS_EXEC_H
#include "fs_port.h"

struct kobox_linux_exec_table;
struct kobox_linux_exec_file;
struct kobox_linux_exec_table *kobox_linux_exec_create(void);
/* Quiesce table users before destroy. Pinned objects own their Linux file
 * independently, and keep deny-write until the last operation releases it. */
void kobox_linux_exec_destroy(struct kobox_linux_exec_table *);
int kobox_linux_exec_open(struct kobox_linux_exec_table *,
    struct kobox_linux_fs_port *, const struct cred *, u64 client,
    u64 directory, const char *name, u64 *handle);
struct kobox_linux_exec_file *kobox_linux_exec_get(
    struct kobox_linux_exec_table *, u64 client, u64 handle);
void kobox_linux_exec_put(struct kobox_linux_exec_file *);
ssize_t kobox_linux_exec_pread(struct kobox_linux_exec_file *,
    const struct cred *, void *, size_t, loff_t);
int kobox_linux_exec_stat(struct kobox_linux_exec_file *,
    const struct cred *, int flags, u32 mask, struct kstat *);
int kobox_linux_exec_close(struct kobox_linux_exec_table *, u64 client, u64 handle);
void kobox_linux_exec_release_client(struct kobox_linux_exec_table *, u64 client);
#ifdef KOBOX_RUNTIME_GATES
/* Trusted standalone gate only: not present in production and never selected
 * by request bytes. Stages inject ENOMEM before allocation or table install. */
enum kobox_exec_gate_failure { KOBOX_EXEC_GATE_ALLOC = 1, KOBOX_EXEC_GATE_INSTALL };
void kobox_linux_exec_gate_fail_next(struct kobox_linux_exec_table *,
    enum kobox_exec_gate_failure);
#endif
#endif
