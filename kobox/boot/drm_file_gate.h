/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_FILE_GATE_H
#define KOBOX_BOOT_DRM_FILE_GATE_H

struct kobox_linux_drm_file;
int kobox_linux_drm_file_gate(struct kobox_linux_drm_file *file,
			     unsigned int *checks);

#endif
