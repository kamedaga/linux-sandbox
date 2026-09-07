/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_LINUX_VM_GATE_H
#define KOBOX_LINUX_VM_GATE_H

#include "../mm/host.h"
#include "pressure_gate.h"

enum kobox_linux_vm_race {
	KOBOX_VM_RACE_NONE,
	KOBOX_VM_RACE_IRQ,
	KOBOX_VM_RACE_TRUNCATE,
	KOBOX_VM_RACE_LATE_FAULT,
	KOBOX_VM_RACE_PRESSURE_FAULT,
	KOBOX_VM_RACE_EXIT_PUBLISH,
	KOBOX_VM_RACE_EXIT,
};

enum kobox_linux_vm_lifetime {
	KOBOX_VM_LIFETIME_NONE,
	KOBOX_VM_LIFETIME_NORMAL,
	KOBOX_VM_LIFETIME_DEATH,
	KOBOX_VM_LIFETIME_ROLLBACK,
};

struct kobox_linux_vm_test {
	size_t size;
	const struct kobox_linux_vm_host_operations *operations;
	void *spaces[2];
	uint64_t pids[2];
	uint64_t start, length;
	uint32_t readonly_case;
	uint32_t reuse_case;
	uint32_t race_case;
	uint32_t lifetime_case;
	int (*notify)(uint32_t cpu);
	int (*terminate)(uint64_t pid);
	int (*probe)(void *space, uint64_t address, unsigned int write,
		uint64_t value, uint64_t sequence);
};

struct kobox_linux_vm_report {
	size_t size;
	uint64_t warnings;
	uint32_t faults, accesses, mm_checks, signals;
	uint32_t held_allocations, reclaimed_pages, revoked_aliases;
	uint32_t publication_races, publication_irqs, stale_resumes, dead_spaces;
	uint32_t delayed_faults;
	uint64_t target_pfn, target_flags;
	uint32_t target_type;
	uint32_t target_migrate, target_free, target_min, search_pages;
	int32_t target_refs;
	uint32_t target_slab_size, target_cache_match;
	uint32_t buddy_slot_reclaims;
	struct kobox_linux_pressure_report pressure;
	uint32_t async_exits, rollbacks;
	uint32_t mm_drains, task_drains, mm_reclaims, task_reclaims;
	uint32_t large_alias_pages;
	uint32_t file_reclaims, inode_reclaims, folio_reclaims;
	uint32_t phase, line;
	int32_t result;
};

#endif
