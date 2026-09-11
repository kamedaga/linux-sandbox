// SPDX-License-Identifier: GPL-2.0-only
/* Test-only storage exercises the actual core compiler TLS relocations. */
static __thread unsigned long tls_value __attribute__((aligned(64))) =
	0x12345678UL;

unsigned long *kobox_linux_tls_probe(void)
{
	return &tls_value;
}
