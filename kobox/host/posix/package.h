/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_PACKAGE_H
#define KOBOX_POSIX_PACKAGE_H

#include "../../boot/package.h"

/* Borrowed Linux descriptors; success owns sealed duplicates and mappings. */
struct kobox_posix_package_input {
	uint64_t expected_generation;
	uint8_t expected_manifest_digest[32];
	uint8_t expected_grant_digest[32];
	int manifest_descriptor;
	int grant_descriptor;
	const int *artifact_descriptors;
	size_t artifact_count;
};

int kobox_posix_package_open(const struct kobox_posix_package_input *input,
			     struct kobox_boot_package **out);
void kobox_posix_package_close(struct kobox_boot_package **package);
/* Borrow the native FD only from a blob acquired by this backend. */
int kobox_posix_blob_descriptor(const struct kobox_boot_blob *blob);

#endif
