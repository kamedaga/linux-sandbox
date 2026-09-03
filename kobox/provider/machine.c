// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/types.h>

#include <asm/gsseg.h>
#include <asm/apic.h>
#include <asm/hw_irq.h>
#include <asm/i8259.h>
#include <asm/setup.h>
#include <asm/x86_init.h>

static unsigned char kobox_vector_lock;

/* The provider starts from a manifest-defined state, without a boot command. */
char boot_command_line[COMMAND_LINE_SIZE];

/* CONFIG_JUMP_LABEL=n uses the initialized atomic static-key path. */
bool static_key_initialized = true;

/* The provider receives virtual IRQ resources and has no physical IOAPIC. */
int noioapicquirk = 1;
int noioapicreroute = -1;

static void kobox_legacy_pic_void(void)
{
}

static void kobox_legacy_pic_irq(unsigned int irq)
{
	(void)irq;
}

static void kobox_legacy_pic_init(int auto_eoi)
{
	(void)auto_eoi;
}

static int kobox_legacy_pic_probe(void)
{
	return 0;
}

static int kobox_legacy_pic_pending(unsigned int irq)
{
	(void)irq;
	return 0;
}

struct legacy_pic null_legacy_pic = {
	.nr_legacy_irqs = 0,
	.mask = kobox_legacy_pic_irq,
	.unmask = kobox_legacy_pic_irq,
	.mask_all = kobox_legacy_pic_void,
	.restore_mask = kobox_legacy_pic_void,
	.init = kobox_legacy_pic_init,
	.probe = kobox_legacy_pic_probe,
	.irq_pending = kobox_legacy_pic_pending,
	.make_irq = kobox_legacy_pic_irq,
};

struct legacy_pic *legacy_pic = &null_legacy_pic;

/* The provider enters after machine boot and cannot execute Linux boot hooks. */
static __noreturn void kobox_machine_boot_fault(void)
{
	__builtin_trap();
}

static void kobox_machine_boot_void(void)
{
	kobox_machine_boot_fault();
}

static int kobox_machine_boot_int(void)
{
	kobox_machine_boot_fault();
}

static char *kobox_machine_boot_memory(void)
{
	kobox_machine_boot_fault();
}

static void kobox_machine_boot_set_root(u64 address)
{
	(void)address;
	kobox_machine_boot_fault();
}

static u64 kobox_machine_boot_get_root(void)
{
	kobox_machine_boot_fault();
}

static struct irq_domain *kobox_machine_boot_msi_domain(void)
{
	kobox_machine_boot_fault();
}

static void kobox_machine_boot_fixup_cpu(struct cpuinfo_x86 *cpu, int node)
{
	(void)cpu;
	(void)node;
	kobox_machine_boot_fault();
}

static unsigned long kobox_machine_calibrate(void)
{
	kobox_machine_boot_fault();
}

static void kobox_machine_get_wallclock(struct timespec64 *time)
{
	(void)time;
	kobox_machine_boot_fault();
}

static int kobox_machine_set_wallclock(const struct timespec64 *time)
{
	(void)time;
	return -EOPNOTSUPP;
}

static bool kobox_machine_is_untracked_pat_range(u64 start, u64 end)
{
	(void)start;
	(void)end;
	return false;
}

static unsigned char kobox_machine_get_nmi_reason(void)
{
	kobox_machine_boot_fault();
}

static void kobox_machine_pin_vcpu(int cpu)
{
	(void)cpu;
	kobox_machine_boot_fault();
}

static bool kobox_machine_is_private_mmio(u64 address)
{
	(void)address;
	return false;
}

static int kobox_machine_memory_transition(unsigned long address, int pages,
					   bool encrypted)
{
	(void)address;
	(void)pages;
	(void)encrypted;
	return 0;
}

static bool kobox_machine_no_encryption_flush(bool encrypted)
{
	(void)encrypted;
	return false;
}

static bool kobox_machine_no_cache_flush(void)
{
	return false;
}

static void kobox_machine_no_encryption_state(void)
{
}

static unsigned int kobox_machine_io_apic_read(unsigned int apic,
					       unsigned int reg)
{
	(void)apic;
	(void)reg;
	kobox_machine_boot_fault();
}

asmlinkage void asm_load_gs_index(u16 selector)
{
	(void)selector;
	kobox_machine_boot_fault();
}

void lock_vector_lock(void)
{
	while (__atomic_test_and_set(&kobox_vector_lock, __ATOMIC_ACQUIRE))
		;
}

void unlock_vector_lock(void)
{
	__atomic_clear(&kobox_vector_lock, __ATOMIC_RELEASE);
}

void lapic_online(void)
{
	/* Native provider threads have no local APIC state to enable. */
}

void lapic_offline(void)
{
	kobox_machine_boot_fault();
}

int lapic_can_unplug_cpu(void)
{
	return -EBUSY;
}

void lapic_update_legacy_vectors(void)
{
	/* The sandbox profile has no legacy PIC vector range. */
}

void pgtable_cache_init(void)
{
	/* x86-64 uses Linux's architecture-independent empty implementation. */
}

void x86_init_noop(void)
{
}

void x86_init_uint_noop(unsigned int unused)
{
	(void)unused;
}

bool bool_x86_init_noop(void)
{
	return false;
}

void x86_op_int_noop(int cpu)
{
	(void)cpu;
}

int set_rtc_noop(const struct timespec64 *time)
{
	(void)time;
	return -EOPNOTSUPP;
}

void get_rtc_noop(struct timespec64 *time)
{
	(void)time;
}

struct x86_init_ops x86_init = {
	.resources = {
		.probe_roms = kobox_machine_boot_void,
		.reserve_resources = kobox_machine_boot_void,
		.memory_setup = kobox_machine_boot_memory,
		.dmi_setup = kobox_machine_boot_void,
	},
	.mpparse = {
		.setup_ioapic_ids = kobox_machine_boot_void,
		.find_mptable = kobox_machine_boot_void,
		.early_parse_smp_cfg = kobox_machine_boot_void,
		.parse_smp_cfg = kobox_machine_boot_void,
	},
	.irqs = {
		.pre_vector_init = kobox_machine_boot_void,
		.intr_init = kobox_machine_boot_void,
		.intr_mode_select = kobox_machine_boot_void,
		.intr_mode_init = kobox_machine_boot_void,
		.create_pci_msi_domain = kobox_machine_boot_msi_domain,
	},
	.oem = {
		.arch_setup = kobox_machine_boot_void,
		.banner = kobox_machine_boot_void,
	},
	.paging = {
		.pagetable_init = kobox_machine_boot_void,
	},
	.timers = {
		.setup_percpu_clockev = kobox_machine_boot_void,
		.timer_init = kobox_machine_boot_void,
		.wallclock_init = kobox_machine_boot_void,
	},
	.iommu = {
		.iommu_init = kobox_machine_boot_int,
	},
	.pci = {
		.arch_init = kobox_machine_boot_int,
		.init = kobox_machine_boot_int,
		.init_irq = kobox_machine_boot_void,
		.fixup_irqs = kobox_machine_boot_void,
	},
	.hyper = {
		.init_platform = kobox_machine_boot_void,
		.guest_late_init = kobox_machine_boot_void,
		.x2apic_available = bool_x86_init_noop,
		.msi_ext_dest_id = bool_x86_init_noop,
		.init_mem_mapping = kobox_machine_boot_void,
		.init_after_bootmem = kobox_machine_boot_void,
	},
	.acpi = {
		.set_root_pointer = kobox_machine_boot_set_root,
		.get_root_pointer = kobox_machine_boot_get_root,
		.reduced_hw_early_init = kobox_machine_boot_void,
	},
};

struct x86_cpuinit_ops x86_cpuinit = {
	.setup_percpu_clockev = kobox_machine_boot_void,
	.early_percpu_clock_init = kobox_machine_boot_void,
	.fixup_cpu_id = kobox_machine_boot_fixup_cpu,
	.parallel_bringup = false,
};

struct x86_platform_ops x86_platform = {
	.calibrate_cpu = kobox_machine_calibrate,
	.calibrate_tsc = kobox_machine_calibrate,
	.get_wallclock = kobox_machine_get_wallclock,
	.set_wallclock = kobox_machine_set_wallclock,
	.iommu_shutdown = kobox_machine_boot_void,
	.is_untracked_pat_range = kobox_machine_is_untracked_pat_range,
	.nmi_init = kobox_machine_boot_void,
	.get_nmi_reason = kobox_machine_get_nmi_reason,
	.save_sched_clock_state = kobox_machine_boot_void,
	.restore_sched_clock_state = kobox_machine_boot_void,
	.legacy = {
		.i8042 = X86_LEGACY_I8042_PLATFORM_ABSENT,
	},
	.realmode_reserve = kobox_machine_boot_void,
	.realmode_init = kobox_machine_boot_void,
	.hyper = {
		.pin_vcpu = kobox_machine_pin_vcpu,
		.is_private_mmio = kobox_machine_is_private_mmio,
	},
	.guest = {
		.enc_status_change_prepare = kobox_machine_memory_transition,
		.enc_status_change_finish = kobox_machine_memory_transition,
		.enc_tlb_flush_required = kobox_machine_no_encryption_flush,
		.enc_cache_flush_required = kobox_machine_no_cache_flush,
		.enc_kexec_begin = kobox_machine_no_encryption_state,
		.enc_kexec_finish = kobox_machine_no_encryption_state,
	},
};

struct x86_apic_ops x86_apic_ops = {
	.io_apic_read = kobox_machine_io_apic_read,
	.restore = kobox_machine_boot_void,
};
