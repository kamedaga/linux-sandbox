// SPDX-License-Identifier: GPL-2.0-only
#include "block_port.h"

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

struct kobox_linux_block_port {
	struct pci_dev *pci;
	struct kobox_linux_block_key write_key;
};

static bool belongs_to_pci_function(const struct gendisk *disk,
				    const struct pci_dev *pci)
{
	const struct device *parent;

	/* USB mass storage has a SCSI ancestry below xHCI; NVMe has a namespace
	 * below the PCI controller. Neither device name nor vendor ID is an
	 * authority boundary. */
	for (parent = disk_to_dev(disk)->parent; parent; parent = parent->parent)
		if (parent == &pci->dev)
			return true;
	return false;
}

int kobox_linux_block_port_open(struct kobox_linux_block_port **out,
				struct pci_dev *pci,
				const struct kobox_linux_block_key *write_key)
{
	struct kobox_linux_block_port *port;

	if (!out || !pci)
		return -EINVAL;
	*out = NULL;
	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->pci = pci;
	if (write_key)
		port->write_key = *write_key;
	*out = port;
	return 0;
}

void kobox_linux_block_port_close(struct kobox_linux_block_port *port)
{
	kfree(port);
}

int kobox_linux_block_port_snapshot(struct kobox_linux_block_port *port,
		size_t start, struct kobox_linux_block_info *devices,
		size_t capacity, size_t *count, size_t *next)
{
	struct class_dev_iter iter;
	struct device *dev;
	size_t found = 0, copied = 0;

	if (!port || !count || !next || !capacity || !devices)
		return -EINVAL;
	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
		struct gendisk *disk = dev_to_disk(dev);
		struct kobox_linux_block_info *info;

		if (!disk_live(disk) || !get_capacity(disk) ||
		    (disk->flags & GENHD_FL_HIDDEN) ||
		    !belongs_to_pci_function(disk, port->pci))
			continue;
		if (found++ < start || copied >= capacity)
			continue;
		info = &devices[copied++];
		memset(info, 0, sizeof(*info));
		info->diskseq = disk->diskseq;
		info->bytes = bdev_nr_bytes(disk->part0);
		info->major = MAJOR(disk_devt(disk));
		info->minor = MINOR(disk_devt(disk));
		info->logical_block_size = bdev_logical_block_size(disk->part0);
		info->physical_block_size = bdev_physical_block_size(disk->part0);
		info->removable = !!(disk->flags & GENHD_FL_REMOVABLE);
		info->read_only = bdev_read_only(disk->part0);
		strscpy(info->name, disk->disk_name, sizeof(info->name));
	}
	class_dev_iter_exit(&iter);
	if (start > found)
		return -EINVAL;
	*count = copied;
	*next = start + copied < found ? start + copied : 0;
	return 0;
}

static struct file *open_disk(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key, bool write)
{
	struct file *file;
	struct gendisk *disk;
	dev_t dev;

	if (!port || !key || !key->diskseq || key->major > 0xfff ||
	    key->minor > 0xfffff)
		return ERR_PTR(-EINVAL);
	if (write && (key->diskseq != port->write_key.diskseq ||
		      key->major != port->write_key.major ||
		      key->minor != port->write_key.minor))
		return ERR_PTR(-EACCES);
	dev = MKDEV(key->major, key->minor);
	file = bdev_file_open_by_dev(dev,
		BLK_OPEN_READ | (write ? BLK_OPEN_WRITE : 0), NULL, NULL);
	if (IS_ERR(file))
		return file;
	disk = file_bdev(file)->bd_disk;
	if (file_bdev(file) != disk->part0 || !disk_live(disk) ||
	    disk->diskseq != key->diskseq ||
	    !belongs_to_pci_function(disk, port->pci)) {
		fput(file);
		return ERR_PTR(-ESTALE);
	}
	return file;
}

static int check_range(struct block_device *bdev, u64 offset, size_t length)
{
	u64 size = bdev_nr_bytes(bdev);
	u32 sector = bdev_logical_block_size(bdev);

	if (!length || length > KOBOX_BLOCK_IO_MAX || !sector ||
	    (offset % sector) || (length % sector) ||
	    offset > size || length > size - offset)
		return -EINVAL;
	return 0;
}

static int submit_block_io(struct block_device *bdev, u64 offset,
		void *buffer, size_t length, bool write)
{
	const unsigned int order = get_order(length);
	const unsigned int vectors = DIV_ROUND_UP(length, PAGE_SIZE);
	struct page *pages;
	struct bio *bio;
	size_t done = 0;
	int result = 0;

	/* The exported buffer belongs to the hosted capability runtime, not to
	 * Linux's page cache.  Bounce through owned pages and submit a synchronous
	 * bio so a successful reply means that the block driver completed the I/O;
	 * buffered kernel_write() would only mean that dirty cache pages exist. */
	pages = alloc_pages(GFP_KERNEL, order);
	if (!pages)
		return -ENOMEM;
	if (write) {
		while (done < length) {
			size_t chunk = min_t(size_t, PAGE_SIZE, length - done);

			memcpy(page_address(pages + (done >> PAGE_SHIFT)),
			       (const u8 *)buffer + done, chunk);
			done += chunk;
		}
	}
	bio = bio_alloc(bdev, vectors,
			(write ? REQ_OP_WRITE : REQ_OP_READ) | REQ_SYNC,
			GFP_KERNEL);
	if (!bio) {
		result = -ENOMEM;
		goto out_pages;
	}
	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
	done = 0;
	while (done < length) {
		size_t chunk = min_t(size_t, PAGE_SIZE, length - done);
		struct page *page = pages + (done >> PAGE_SHIFT);

		if (bio_add_page(bio, page, chunk, 0) != chunk) {
			result = -EIO;
			goto out_bio;
		}
		done += chunk;
	}
	result = submit_bio_wait(bio);
	if (!result && !write) {
		done = 0;
		while (done < length) {
			size_t chunk = min_t(size_t, PAGE_SIZE, length - done);

			memcpy((u8 *)buffer + done,
			       page_address(pages + (done >> PAGE_SHIFT)),
			       chunk);
			done += chunk;
		}
	}
out_bio:
	bio_put(bio);
out_pages:
	__free_pages(pages, order);
	return result;
}

int kobox_linux_block_port_read(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key, u64 offset,
		void *buffer, size_t length)
{
	struct file *file;
	int result;

	if (!buffer)
		return -EINVAL;
	file = open_disk(port, key, false);
	if (IS_ERR(file))
		return PTR_ERR(file);
	result = check_range(file_bdev(file), offset, length);
	if (!result)
		result = submit_block_io(file_bdev(file), offset, buffer, length,
				false);
	fput(file);
	return result;
}

int kobox_linux_block_port_write(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key, u64 offset,
		const void *buffer, size_t length)
{
	struct file *file;
	int result;

	if (!buffer)
		return -EINVAL;
	file = open_disk(port, key, true);
	if (IS_ERR(file))
		return PTR_ERR(file);
	result = check_range(file_bdev(file), offset, length);
	if (!result)
		result = submit_block_io(file_bdev(file), offset, (void *)buffer,
				length, true);
	fput(file);
	return result;
}

int kobox_linux_block_port_flush(struct kobox_linux_block_port *port,
		const struct kobox_linux_block_key *key)
{
	struct file *file = open_disk(port, key, true);
	int result;

	if (IS_ERR(file))
		return PTR_ERR(file);
	/* Direct bios above have completed, but the physical device may still
	 * have a volatile write cache.  Preserve Linux block-device FLUSH
	 * semantics before reporting durable completion to the capability user. */
	result = sync_blockdev(file_bdev(file));
	if (!result)
		result = blkdev_issue_flush(file_bdev(file));
	fput(file);
	return result;
}
