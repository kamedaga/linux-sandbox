// SPDX-License-Identifier: GPL-2.0-only

#include "qtest.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int config_read(struct kobox_qtest *test, unsigned int offset,
		       unsigned int width, unsigned int *value)
{
	char response[128];
	int result;

	result = kobox_qtest_command(test, response, sizeof(response),
				    "outl 0xcf8 0x%x", 0x80000800 | (offset & ~3U));
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response),
					    "in%c 0x%x", width == 1 ? 'b' : 'l',
					    0xcfc + (offset & 3));
	if (!result && sscanf(response, "OK 0x%x", value) != 1)
		result = EPROTO;
	return result;
}

static int modern_registers(struct kobox_qtest *test)
{
	char response[128];
	unsigned int capability, type, bar = 0, offset = 0, feature;
	unsigned int visits = 0;
	int result;

	/* Inspect the real hardware capability list. Production enumeration
	 * and negotiation will be performed by upstream virtio-pci, not here.
	 */
	result = config_read(test, 0x34, 1, &capability);
	while (!result && capability && visits++ < 48) {
		result = config_read(test, capability, 1, &type);
		if (result)
			break;
		if (type == 9) {
			result = config_read(test, capability + 3, 1, &type);
			if (!result && type == 1) {
				result = config_read(test, capability + 4, 1, &bar);
				if (!result)
					result = config_read(test, capability + 8, 4, &offset);
				break;
			}
		}
		if (!result)
			result = config_read(test, capability + 1, 1, &capability);
	}
	if (result || !capability || visits > 48 || bar > 5)
		return result ? result : EPROTO;
	result = kobox_qtest_command(test, response, sizeof(response),
				    "outl 0xcf8 0x%x", 0x80000810 + 4 * bar);
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "outl 0xcfc 0x20000000");
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "outl 0xcf8 0x80000804");
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "outw 0xcfc 6");
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "writel 0x%x 1",
					    0x20000000 + offset);
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "readl 0x%x",
					    0x20000004 + offset);
	/* Modern version and ACCESS_PLATFORM are device features 32 and 33. */
	if (!result && (sscanf(response, "OK 0x%x", &feature) != 1 || (feature & 3) != 3))
		return EPROTO;
	return result;
}

int main(int argc, char **argv)
{
	const char *arguments[] = {
		"-machine", "q35", "-m", "64M",
		"-device", "virtio-gpu-pci,addr=1.0,disable-legacy=on,iommu_platform=on",
		"-device", "intel-iommu,caching-mode=on,intremap=off",
	};
	struct kobox_qtest *test = NULL;
	char response[128];
	unsigned int identifier = 0;
	int result;

	if (argc != 2)
		return 1;
	result = kobox_qtest_start(&test, argv[1], arguments,
				  sizeof(arguments) / sizeof(arguments[0]));
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response),
					    "outl 0xcf8 0x80000800");
	if (!result)
		result = kobox_qtest_command(test, response, sizeof(response), "inl 0xcfc");
	if (!result && (sscanf(response, "OK 0x%x", &identifier) != 1 || identifier != 0x10501af4))
		result = 1;
	if (!result)
		result = modern_registers(test);
	if (!result && kobox_qtest_command(test, response, sizeof(response), "inl 0xcfc\ninl 0xcfc") != EINVAL)
		result = EPROTO;
	if (!result && kobox_qtest_command(test, response, 4, "readl 0x20000004") != EOVERFLOW)
		result = EPROTO;
	if (!result && kobox_qtest_command(test, response, sizeof(response), "inl 0xcfc") != EPIPE)
		result = EPROTO;
	if (test && kobox_qtest_close(test))
		result = 1;
	if (result)
		fprintf(stderr, "QEMU hardware transport: %s (%d), PCI=%08x\n",
			strerror(result), result, identifier);
	else
		puts("QEMU virtio-gpu PCI/MMIO transport passed (not the Linux driver Gate)");
	return !!result;
}
