/* SPDX-License-Identifier: GPL-2.0-only */

SECTIONS
{
	.initcall2.init :
	{
		KEEP(*(.initcall2.init))
	}
}
INSERT AFTER .rodata;
