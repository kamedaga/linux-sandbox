/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_GEM_LIFETIME_TEST_H
#define KOBOX_GEM_LIFETIME_TEST_H

#include "../boot/vm_gate.h"
#include "../boot/pressure_gate.h"

struct vfsmount;
struct mm_struct;
struct task_struct;
struct kobox_vm_lifetime;

enum kobox_gem_cleanup {
	KOBOX_GEM_CLEANUP_NONE,
	KOBOX_GEM_CLEANUP_NORMAL,
	KOBOX_GEM_CLEANUP_DEATH,
};

/* Borrowed GPL test services; no additional runtime module exports. */
struct kobox_gem_failure_services {
	struct vfsmount *mount;
	uint32_t cleanup;
	void (*drain)(void);
	unsigned long (*vmalloc_pages)(void);
	int (*pressure)(struct kobox_linux_pressure_report *report,
			int (*inspect)(void *argument), void *argument);
	struct kobox_vm_lifetime *(*tasks_begin)(struct mm_struct *mms[2],
		struct task_struct *tasks[2], struct kobox_linux_vm_report *report);
	int (*tasks_finish)(struct kobox_vm_lifetime *audit);
};

enum kobox_gem_final_owner {
	KOBOX_GEM_FINAL_VMA,
	KOBOX_GEM_FINAL_OBJECT,
};

struct kobox_gem_lifetime_report {
	uint32_t phase, line;
	int32_t result;
	uint32_t files, handles, pins, vmaps, mappings;
	uint32_t accesses, faults, denied, partial_unmaps;
	uint32_t object_reclaims, page_reclaims;
	uint32_t live_checks, revoked;
	uint32_t allocation_stage, fail_nth, injected, rollbacks, recoveries, sweeps;
	uint32_t pressure_accesses;
	uint32_t close_faults;
	uint32_t cleanup_rollbacks;
	uint32_t pressure_unmaps;
	uint32_t client_deaths, aborted_faults;
	uint32_t deferred_callbacks, rejected_work;
	uint32_t fault_retries, page_table_reclaims;
	struct kobox_linux_pressure_report pressure;
	struct kobox_linux_vm_report task_lifetime;
};

/* GPL test-module entry. drain executes native deferred cleanup in PID 1. */
int kobox_gem_lifetime_test(const struct kobox_linux_vm_test *host,
	struct kobox_gem_lifetime_report *report, void (*drain)(void),
	enum kobox_gem_final_owner final_owner);
int kobox_gem_failure_test(const struct kobox_linux_vm_test *host,
	struct kobox_gem_lifetime_report *report,
	const struct kobox_gem_failure_services *services,
	enum kobox_gem_final_owner final_owner);

#endif
