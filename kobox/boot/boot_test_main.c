// SPDX-License-Identifier: GPL-2.0-only
#include "boot_test.h"

int main(int argc, char **argv)
{
	return kobox_boot_test_run(argc, argv, 0);
}
