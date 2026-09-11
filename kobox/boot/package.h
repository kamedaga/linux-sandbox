/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_PACKAGE_H
#define KOBOX_BOOT_PACKAGE_H

#include <kobox2/closure_manifest.h>
#include <kobox2/resource_grant.h>
#include <stdbool.h>

#define KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS 64U

struct kobox_boot_blob {
	void *owner;
	const void *data;
	size_t size;
};

enum kobox_package_result {
	KOBOX_PACKAGE_OK,
	KOBOX_PACKAGE_INVALID,
	KOBOX_PACKAGE_NO_MEMORY,
	KOBOX_PACKAGE_IO,
	KOBOX_PACKAGE_IMMUTABILITY,
	KOBOX_PACKAGE_TOO_LARGE,
	KOBOX_PACKAGE_MALFORMED,
	KOBOX_PACKAGE_EXEC_FORMAT,
	KOBOX_PACKAGE_STALE,
};

/* Inputs are borrowed opaque capabilities, not FD numbers. Open must acquire
 * an independently owned, immutable view, bounded before mapping. Failure
 * must release partial acquisition and leave the blob empty. Close releases
 * the view and ownership; it is called only for successful acquisitions.
 */
struct kobox_package_operations {
	enum kobox_package_result (*open)(void *context, const void *input,
					 size_t maximum,
					 struct kobox_boot_blob *blob);
	void (*close)(void *context, struct kobox_boot_blob *blob);
	/* Selected architecture's ELF profile, not an OS loader invocation. */
	bool (*elf_matches)(const void *data, size_t size, bool core);
};

/* Expected identity is independent launch-owner state, never packet claims.
 * Operations/context must outlive the package and all borrowed views.
 */
struct kobox_boot_package_input {
	uint64_t expected_generation;
	uint8_t expected_manifest_digest[32];
	uint8_t expected_grant_digest[32];
	const void *manifest;
	const void *grant;
	const void *const *artifacts;
	size_t artifact_count;
	const struct kobox_package_operations *operations;
	void *context;
};

struct kobox_boot_package {
	kb2_closure_manifest_t manifest;
	kb2_resource_grant_t grant;
	struct kobox_boot_blob manifest_blob;
	struct kobox_boot_blob grant_blob;
	struct kobox_boot_blob artifacts[KOBOX_BOOT_PACKAGE_MAX_ARTIFACTS];
	size_t artifact_count;
	const struct kobox_package_operations *operations;
	void *context;
};

/* Validates every artifact before any ELF loading or resource import.
 * One boot core comes first, followed by native Linux modules in dependency
 * order. Legacy kobox lifecycle entry points are intentionally rejected.
 * Storage is caller-owned and initially zero. Failure releases every acquired
 * blob and leaves it empty. Close only after module users and registry exit.
 */
enum kobox_package_result kobox_boot_package_open(
	const struct kobox_boot_package_input *input,
	struct kobox_boot_package *package);
void kobox_boot_package_close(struct kobox_boot_package *package);

#endif
