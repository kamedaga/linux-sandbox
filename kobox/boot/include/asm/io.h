/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_ASM_IO_H
#define KOBOX_BOOT_ASM_IO_H

#include_next <asm/io.h>

#ifdef KOBOX_BOOT_RUNTIME
u64 kobox_mmio_read(const volatile void __iomem *address, unsigned int width);
void kobox_mmio_write(volatile void __iomem *address, unsigned int width, u64 value);

/* Device models outside this process require transactions rather than CPU
 * loads from shared register-looking storage. Real mapped BARs keep native
 * accesses. Both cases use the same upstream drivers and I/O accessors.
 */
#define kobox_io_read(name, type) \
static inline type kobox_##name(const volatile void __iomem *address) \
{ return kobox_mmio_read(address, sizeof(type)); }
#define kobox_io_write(name, type) \
static inline void kobox_##name(type value, volatile void __iomem *address) \
{ kobox_mmio_write(address, sizeof(type), value); }

kobox_io_read(readb, u8)
kobox_io_read(readw, u16)
kobox_io_read(readl, u32)
kobox_io_read(readq, u64)
kobox_io_write(writeb, u8)
kobox_io_write(writew, u16)
kobox_io_write(writel, u32)
kobox_io_write(writeq, u64)

#undef readb
#undef readw
#undef readl
#undef readq
#undef writeb
#undef writew
#undef writel
#undef writeq
#define readb kobox_readb
#define readw kobox_readw
#define readl kobox_readl
#define readq kobox_readq
#define writeb kobox_writeb
#define writew kobox_writew
#define writel kobox_writel
#define writeq kobox_writeq
/* Upstream relaxed/raw aliases expand through these instruction names. */
#define __readb kobox_readb
#define __readw kobox_readw
#define __readl kobox_readl
#define __readq kobox_readq
#define __writeb kobox_writeb
#define __writew kobox_writew
#define __writel kobox_writel
#define __writeq kobox_writeq
#endif

#endif
