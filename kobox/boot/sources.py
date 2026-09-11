# SPDX-License-Identifier: GPL-2.0-only
"""Explicit full-core port and optional Gate inputs; never infer from globbing."""

COMMON_SOURCES = (
    "kobox/runtime/host.c",
    "kobox/memory/early_boot.c",
    "kobox/task/port.c",
    "kobox/task/user.c",
    "kobox/task/time_port.c",
    "kobox/boot/port.c",
    "kobox/boot/cpu_port.c",
    "kobox/mm/port.c",
    "kobox/mm/uaccess.c",
    "kobox/boot/module_exports.c",
    "kobox/boot/resource_port.c",
    "kobox/boot/pci_port.c",
    "kobox/boot/dma_port.c",
    "kobox/boot/irq_port.c",
    "kobox/memory/mmio.c",
    "kobox/boot/lifecycle.c",
    "kobox/boot/module_launch.c",
    "kobox/boot/module_port.c",
)

ARCH_SOURCES = (
    "kobox/arch/x86_64/tls.c",
    "kobox/arch/x86_64/fpu.c",
    "kobox/arch/x86_64/registers.c",
    "kobox/arch/x86_64/task.c",
    "kobox/arch/x86_64/exception.c",
)

GATE_SOURCES = (
    "kobox/tests/gates/task_smp.c",
    "kobox/boot/tls_gate.c",
    "kobox/task/time_gate.c",
    "kobox/boot/service_gate.c",
    "kobox/boot/wait_gate.c",
    "kobox/boot/rcu_gate.c",
    "kobox/boot/workqueue_gate.c",
    "kobox/boot/cleanup_gate.c",
    "kobox/boot/memory_gate.c",
    "kobox/boot/vfs_gate.c",
    "kobox/boot/shmem_gate.c",
    "kobox/boot/pressure_gate.c",
    "kobox/boot/allocation_gate.c",
    "kobox/boot/vm_gate.c",
    "kobox/boot/vm_lifetime.c",
    "kobox/boot/module_gate.c",
    "kobox/boot/client_task_gate.c",
    "kobox/boot/uaccess_gate.c",
    "kobox/boot/syscall_gate.c",
    "kobox/boot/exec_gate.c",
    "kobox/boot/pci_gate.c",
    "kobox/boot/pci_hardware_gate.c",
    "kobox/boot/virtio_gate.c",
    "kobox/boot/dma_gate.c",
    "kobox/boot/irq_gate.c",
)

def support_sources(with_gates):
    return COMMON_SOURCES + ARCH_SOURCES + (GATE_SOURCES if with_gates else ())
