// SPDX-License-Identifier: GPL-2.0-only
/* No-device integration test for the unmodified Linux firmware loader. */
#include <crypto/sha2.h>
#include <linux/firmware.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

static char *requests0, *requests1, *requests2;
module_param(requests0, charp, 0400);
module_param(requests1, charp, 0400);
module_param(requests2, charp, 0400);
MODULE_PARM_DESC(requests0, "First group of name:sha256 requests");
MODULE_PARM_DESC(requests1, "Second group of name:sha256 requests");
MODULE_PARM_DESC(requests2, "Third group of name:sha256 requests");

static int verify_list(const char *requests, unsigned int *count)
{
	char *copy, *next, *entry;
	int result = 0;

	if (!requests || !*requests)
		return 0;
	if (strlen(requests) > 1000)
		return -EINVAL;
	copy = kstrdup(requests, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	next = copy;
	while ((entry = strsep(&next, ",")) != NULL) {
		const struct firmware *firmware = NULL;
		char *separator = strchr(entry, ':');
		u8 expected[SHA256_DIGEST_SIZE], actual[SHA256_DIGEST_SIZE];

		if (++*count > 64 || !separator || separator == entry ||
		    strlen(separator + 1) != SHA256_DIGEST_SIZE * 2) {
			result = -EINVAL;
			break;
		}
		*separator++ = '\0';
		result = hex2bin(expected, separator, sizeof(expected));
		if (result)
			break;
		result = request_firmware(&firmware, entry, NULL);
		if (result) {
			pr_err("kobox-firmware: request=%s status=%d expected_sha256=%*phN\n",
			       entry, result, SHA256_DIGEST_SIZE, expected);
			break;
		}
		sha256(firmware->data, firmware->size, actual);
		result = memcmp(actual, expected, sizeof(actual)) ? -EBADMSG : 0;
		pr_info("kobox-firmware: request=%s status=%d size=%zu sha256=%*phN\n",
			entry, result, firmware->size, SHA256_DIGEST_SIZE, actual);
		release_firmware(firmware);
		if (result)
			break;
	}
	kfree(copy);
	return result;
}

static int __init kobox_firmware_probe_init(void)
{
	const char *groups[] = {requests0, requests1, requests2};
	unsigned int count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
		int result = verify_list(groups[i], &count);

		if (result)
			return result;
	}
	return count ? 0 : -EINVAL;
}

static void __exit kobox_firmware_probe_exit(void)
{
	pr_info("kobox-firmware: probe module unloaded\n");
}

module_init(kobox_firmware_probe_init);
module_exit(kobox_firmware_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kobox no-PCI request_firmware integration probe");
