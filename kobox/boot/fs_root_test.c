// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint64_t u64;
typedef uint64_t sector_t;
typedef unsigned dev_t;
typedef struct { u8 bytes[16]; } guid_t;
struct device { struct device *parent; const void *type; unsigned refs; };
struct pci_dev { struct device dev; };
struct gendisk { struct device device; u64 diskseq; unsigned flags; bool live; };
struct partition_meta_info { char uuid[37]; };
struct block_device {
    struct device bd_device;
    struct gendisk *bd_disk;
    struct partition_meta_info *bd_meta_info;
    dev_t bd_dev;
    sector_t start, sectors;
    bool partition;
};
struct file { struct block_device *bdev; unsigned refs; };
struct class_dev_iter { unsigned next; };
static const char part_type, disk_type, block_class;
#define GENHD_FL_HIDDEN 1
#define BLK_OPEN_READ 1
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define dev_to_bdev(d) ((struct block_device *)((char *)(d) - offsetof(struct block_device, bd_device)))
static struct device *disk_to_dev(struct gendisk *d) { return &d->device; }
static bool disk_live(struct gendisk *d) { return d->live; }
static u64 bdev_nr_bytes(struct block_device *b) { return b->sectors * 512; }
static sector_t bdev_nr_sectors(struct block_device *b) { return b->sectors; }
static sector_t get_start_sect(struct block_device *b) { return b->start; }
static bool bdev_is_partition(struct block_device *b) { return b->partition; }
static void get_device(struct device *d) { ++d->refs; }
static void put_device(struct device *d) { assert(d->refs); --d->refs; }
static struct block_device *file_bdev(struct file *f) { return f->bdev; }
static void fput(struct file *f) { assert(f->refs); --f->refs; }

static struct block_device parts[4], replacement;
static struct partition_meta_info metadata[4];
static struct gendisk disks[2];
static struct pci_dev pci[2];
static struct file opened;
static unsigned count, opens;
static void (*at_open)(void);
static int open_error;
static void class_dev_iter_init(struct class_dev_iter *i, const void *c, void *p, void *t)
{ (void)c; (void)p; (void)t; i->next=0; }
static struct device *class_dev_iter_next(struct class_dev_iter *i)
{ return i->next<count ? &parts[i->next++].bd_device : NULL; }
static void class_dev_iter_exit(struct class_dev_iter *i) { (void)i; }
static struct file *bdev_file_open_by_dev(dev_t dev, int mode, void *holder, void *ops)
{
    (void)holder; (void)ops;
    assert(mode==BLK_OPEN_READ);
    ++opens;
    if (open_error) return (void *)(intptr_t)open_error;
    assert(dev && dev<=count);
    opened.bdev=&parts[dev-1];
    opened.refs=1;
    if (at_open) at_open();
    return &opened;
}
static int guid_parse(const char *s, guid_t *g)
{
    unsigned a, b, c, d[8];
    int end=0;
    if (sscanf(s,"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x%n",
               &a,&b,&c,&d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6],&d[7],&end)!=11 || end!=36)
        return -EINVAL;
    for (unsigned i=0;i<4;i++) g->bytes[i]=(u8)(a>>(8*i));
    for (unsigned i=0;i<2;i++) g->bytes[4+i]=(u8)(b>>(8*i));
    for (unsigned i=0;i<2;i++) g->bytes[6+i]=(u8)(c>>(8*i));
    for (unsigned i=0;i<8;i++) g->bytes[8+i]=(u8)d[i];
    return 0;
}

#include "fs_root_selector.inc"

static const u8 target[16]={0x33,0x22,0x11,0,0x55,0x44,0x77,0x46,
                            0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
static const char target_text[]="00112233-4455-4677-8899-aabbccddeeff";
static void reset(void)
{
    memset(parts,0,sizeof(parts));
    memset(metadata,0,sizeof(metadata));
    memset(disks,0,sizeof(disks));
    memset(pci,0,sizeof(pci));
    opened=(struct file){0}; opens=0; count=2; at_open=NULL; open_error=0;
    for (unsigned i=0;i<2;i++) {
        disks[i].device.parent=&pci[i].dev;
        disks[i].diskseq=i+7;
        disks[i].live=true;
    }
    for (unsigned i=0;i<4;i++) {
        parts[i]=(struct block_device){.bd_device={.type=&part_type},
            .bd_disk=&disks[0],.bd_meta_info=&metadata[i],.bd_dev=i+1,
            .start=2048+i*100,.sectors=100,.partition=true};
        strcpy(metadata[i].uuid,"ffffffff-ffff-4fff-8fff-ffffffffffff");
    }
    strcpy(metadata[0].uuid,target_text);
}
static void check(int expected)
{
    struct pci_dev *allowed[]={&pci[0]};
    struct file *selected=(void *)1;
    assert(fs_select_root(allowed,1,target,&selected)==expected);
    if (!expected) {
        assert(selected==&opened && opened.refs==1 && selected->bdev==&parts[0]);
        fput(selected);
    } else assert(!selected);
    assert(!opened.refs);
    for (unsigned i=0;i<4;i++) assert(!parts[i].bd_device.refs);
}
static void change_seq(void) { ++disks[0].diskseq; }
static void change_start(void) { ++parts[0].start; }
static void change_size(void) { ++parts[0].sectors; }
static void remove_disk(void) { disks[0].live=false; }
static void change_uuid(void) { metadata[0].uuid[0]='f'; }
static void reuse_dev(void) { replacement=parts[0]; opened.bdev=&replacement; }
static void hide_disk(void) { disks[0].flags=GENHD_FL_HIDDEN; }
static void change_parent(void) { disks[0].device.parent=&pci[1].dev; }

int main(void)
{
    assert(fs_root_fstype_valid("ext4\0\0\0"));
    assert(fs_root_fstype_valid("xfs\0\0\0\0"));
    assert(fs_root_fstype_valid("btrfs\0\0"));
    assert(!fs_root_fstype_valid("tmpfs\0\0"));
    assert(!fs_root_fstype_valid("ext4junk"));
    assert(!fs_root_fstype_valid("\0\0\0\0\0\0\0"));
    reset(); check(0); assert(opens==1);
    reset(); count=4; parts[3].bd_disk=&disks[1]; check(0);
    reset(); strcpy(metadata[1].uuid,target_text); check(-ENOTUNIQ); assert(!opens);
    reset(); strcpy(metadata[1].uuid,target_text); parts[1].bd_disk=&disks[1]; check(0);
    reset(); parts[0].bd_disk=&disks[1]; check(-ENODEV); assert(!opens);
    reset(); metadata[0].uuid[0]='f'; check(-ENODEV); assert(!opens);
    reset(); parts[0].bd_meta_info=NULL; check(-ENODEV);
    reset(); strcpy(metadata[0].uuid,"12345678-02"); check(-ENODEV);
    reset(); parts[0].partition=false; parts[0].bd_device.type=&disk_type; check(-ENODEV);
    reset(); disks[0].flags=GENHD_FL_HIDDEN; check(-ENODEV);
    reset(); disks[0].live=false; check(-ENODEV);
    reset(); parts[0].sectors=0; check(-ENODEV);
    void (*faults[])(void)={change_seq,change_start,change_size,remove_disk,change_uuid,reuse_dev,
                           hide_disk,change_parent};
    for (unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);i++) {
        reset(); at_open=faults[i]; check(-ESTALE);
    }
    reset(); open_error=-EIO; check(-EIO);
    puts("FS_ROOT_PARTUUID_UNIT_PASS");
}
