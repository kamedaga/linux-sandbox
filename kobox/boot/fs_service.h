/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_FS_SERVICE_H
#define KOBOX_BOOT_FS_SERVICE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct kobox_linux_fs_service;
struct kobox_linux_fs_request;

#ifdef __KERNEL__
struct vfsmount;
struct cred;

struct kobox_linux_fs_service *
kobox_linux_fs_service_create(struct vfsmount *root, u64 generation);
/* Caller quiesces dispatch and owner updates before destroy. */
void kobox_linux_fs_service_destroy(struct kobox_linux_fs_service *service);

/* Trusted owner only: never expose these functions through client requests.
 * IDs and credential generations cannot be reused. Immutable creds are
 * retained, including supplementary groups, fsids and Linux capabilities.
 */
int kobox_linux_fs_service_register(struct kobox_linux_fs_service *service,
				   u64 client, u64 credential_generation,
				   const struct cred *cred, u64 rights);
int kobox_linux_fs_service_credentials(struct kobox_linux_fs_service *service,
				      u64 client, u64 credential_generation,
				      const struct cred *cred, u64 rights);
int kobox_linux_fs_service_unregister(struct kobox_linux_fs_service *service,
				     u64 client);
int kobox_linux_fs_service_transfer(struct kobox_linux_fs_service *service,
				   u64 source, u64 target, u64 handle,
				   u64 *duplicate);
#ifdef KOBOX_FS_INTERNAL_BENCH
void kobox_linux_fs_profile_reset(struct kobox_linux_fs_service *service);
void kobox_linux_fs_profile_report(struct kobox_linux_fs_service *service);
#endif
#ifdef KOBOX_RUNTIME_GATES
void kobox_linux_fs_request_gate_fail_allocation(void);
int kobox_linux_fs_request_gate_recycled(struct kobox_linux_fs_request *request,
	size_t bytes);
#endif
#endif

/* Process-local port, not a native wire structure. The authenticated client
 * comes from the channel owner, never from request bytes. Zero identity claims
 * select that binding's current credentials; nonzero claims must match. The
 * service retains capability rights with those credentials, and authorizes and
 * executes the same immutable snapshot. Frontend/owner opcodes cannot execute
 * through this entry, even for a client with every filesystem right.
 * The service snapshots
 * input before decoding; output is private and must not overlap input. Its
 * capacity is the writable descriptor lease, not a filesystem limit. Ordinary
 * operations may run concurrently; per-file offsets belong to Linux.
 * A zero return means an encoded reply (possibly a negative Linux errno).
 * A transport error leaves used=0 and has no filesystem side effects.
 * Both wire entries run current's deferred Linux task work after unwinding
 * request scopes, before the host may publish completion. The serving task
 * need not return to Linux userspace to release its final file references.
 */
int kobox_linux_fs_service_dispatch(struct kobox_linux_fs_service *service,
	uint64_t authenticated_client, const void *request, size_t request_size,
	void *response, size_t response_capacity, size_t *used);

/* Admission and execution may run on different Linux tasks. Admission owns
 * one private input snapshot and retains the client/credential/right pair and
 * actual file/dirfd operands at that point. A queued CLOSE cannot redirect or
 * invalidate another admitted operation; no pathname is reopened to recover it.
 * The caller owns a private response lease until destroy. Execute exactly once
 * and on an actual Linux task; completion includes that task's deferred fput.
 * Destroy also permits cancelling a prepared, never-executed request. An
 * absent/mismatched authenticated binding returns -EACCES without admission;
 * operation-level rights denial is retained as an encoded execution reply.
 */
int kobox_linux_fs_request_prepare(struct kobox_linux_fs_service *service,
	uint64_t authenticated_client, const void *input, size_t input_size,
	void *response, size_t response_capacity,
	struct kobox_linux_fs_request **request);
/* Pool admission already charges these bytes. This variant owns input and
 * output together with the job, with no size cap beyond that admission budget.
 * The returned private response remains valid until request_destroy, including
 * after execution has dropped client/credential/file references. Optional
 * storage must be recycled with exactly input_size + response_capacity bytes;
 * this call consumes it on both success and failure. This is a private Linux
 * helper, not a transport/host ABI or a cache of authenticated requests.
 */
int kobox_linux_fs_request_prepare_owned(struct kobox_linux_fs_service *service,
	uint64_t client, const void *input, size_t input_size, size_t response_capacity,
	struct kobox_linux_fs_request *storage,
	struct kobox_linux_fs_request **request, void **response);
/* Completed owned request only, after the caller releases its response lease.
 * Drop refs/task_work and clear metadata plus payload; return its byte capacity.
 */
size_t kobox_linux_fs_request_recycle(struct kobox_linux_fs_request *request);
/* Owner-only admission. Blocking exec/device operations retain the same
 * credential/right pair here, not when the management worker runs. Registry
 * updates are ordered by the queue owner against subsequent client admission.
 */
int kobox_linux_fs_request_prepare_control(struct kobox_linux_fs_service *service,
	const void *input, size_t input_size, void *response,
	size_t response_capacity, struct kobox_linux_fs_request **request);
int kobox_linux_fs_request_execute(struct kobox_linux_fs_request *request,
	size_t *used);
#ifdef __KERNEL__
struct fs_struct;
/* Exclusive worker scratch context; never share it across executing requests.
 * Destroy it only after worker join, before service/mount teardown.
 */
/* Inline execution on the ring owner: -EAGAIN leaves the job unexecuted. */
bool kobox_linux_fs_request_executed(const struct kobox_linux_fs_request *job);
int kobox_linux_fs_request_try_nowait(struct kobox_linux_fs_request *job,
	struct fs_struct *scope, size_t *used);
int kobox_linux_fs_request_execute_scope(struct kobox_linux_fs_request *request,
	struct fs_struct *scope, size_t *used);
#endif
void kobox_linux_fs_request_destroy(struct kobox_linux_fs_request *request);

/* Authenticated management channel only. Keep this entry separate from
 * dispatch so an ordinary client cannot grant itself credentials or files.
 */
int kobox_linux_fs_service_control(struct kobox_linux_fs_service *service,
	const void *request, size_t request_size, void *response,
	size_t response_capacity, size_t *used);

#endif /* KOBOX_BOOT_FS_SERVICE_H */
