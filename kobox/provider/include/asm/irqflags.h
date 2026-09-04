/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_PROVIDER_ASM_IRQFLAGS_H
#define KOBOX_PROVIDER_ASM_IRQFLAGS_H

#ifdef __ASSEMBLY__
#include_next <asm/irqflags.h>
#else

#include <asm/nospec-branch.h>
#include <asm/processor-flags.h>
#include <linux/types.h>

extern unsigned long kobox_provider_irq_save_flags(void);
extern void kobox_provider_irq_disable(void);
extern void kobox_provider_irq_enable(void);
extern unsigned long kobox_provider_irq_save(void);
extern void kobox_provider_irq_restore(unsigned long flags);
extern void kobox_provider_cpu_idle(void);

static __always_inline unsigned long native_save_fl(void)
{
	return kobox_provider_irq_save_flags();
}

static __always_inline void native_irq_disable(void)
{
	kobox_provider_irq_disable();
}

static __always_inline void native_irq_enable(void)
{
	kobox_provider_irq_enable();
}

static __always_inline void native_safe_halt(void)
{
	native_irq_enable();
	kobox_provider_cpu_idle();
}

static __always_inline void native_halt(void)
{
	kobox_provider_cpu_idle();
}

static __always_inline int native_irqs_disabled_flags(unsigned long flags)
{
	return (u32)flags != 0;
}

static __always_inline unsigned long native_local_irq_save(void)
{
	return kobox_provider_irq_save();
}

static __always_inline void native_local_irq_restore(unsigned long flags)
{
	kobox_provider_irq_restore(flags);
}

static __always_inline unsigned long arch_local_save_flags(void)
{
	return native_save_fl();
}

static __always_inline void arch_local_irq_disable(void)
{
	native_irq_disable();
}

static __always_inline void arch_local_irq_enable(void)
{
	native_irq_enable();
}

static __always_inline unsigned long arch_local_irq_save(void)
{
	return native_local_irq_save();
}

static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	native_local_irq_restore(flags);
}

static __always_inline void arch_safe_halt(void)
{
	native_safe_halt();
}

static __always_inline void halt(void)
{
	native_halt();
}

static __always_inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return native_irqs_disabled_flags(flags);
}

static __always_inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif /* __ASSEMBLY__ */

#endif /* KOBOX_PROVIDER_ASM_IRQFLAGS_H */
