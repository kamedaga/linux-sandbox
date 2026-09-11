/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_MESA_KMS_TEST_H
#define KOBOX_MESA_KMS_TEST_H

#include <EGL/egl.h>
#include <gbm.h>

EGLConfig kobox_mesa_kms_config(EGLDisplay display);
int kobox_mesa_kms_test(int fd, struct gbm_device *device, EGLDisplay display,
			EGLContext context, unsigned int cpu);

#endif
