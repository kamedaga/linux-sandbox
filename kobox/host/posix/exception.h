/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_POSIX_EXCEPTION_H
#define KOBOX_POSIX_EXCEPTION_H

#include "../../boot/exception.h"

/* Install before CPU execution can fault. Unhandled faults are fatal. */
int kobox_posix_exceptions_install(kobox_linux_exception_fn dispatch);
int kobox_posix_exceptions_remove(void);

#endif
