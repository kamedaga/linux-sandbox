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
    "kobox/mm/nofault.c",
    "kobox/boot/module_exports.c",
    "kobox/boot/resource_port.c",
    "kobox/boot/pci_port.c",
    "kobox/boot/dma_port.c",
    "kobox/boot/irq_port.c",
    "kobox/memory/mmio.c",
    "kobox/boot/lifecycle.c",
    "kobox/boot/module_launch.c",
    "kobox/boot/firmware_files.c",
    "kobox/boot/device_port.c",
    "kobox/boot/module_port.c",
)

GPU_SOURCES = (
    "kobox/boot/device_launch.c",
    "kobox/boot/drm_file.c",
    "kobox/boot/drm_fence.c",
    "kobox/boot/drm_service.c",
)

USB_INPUT_SOURCES = (
    "kobox/boot/input_port.c",
)

NET_SOURCES = (
    "kobox/boot/net_port.c",
)

BLOCK_SOURCES = (
    "kobox/boot/block_port.c",
)

FS_SOURCES = (
    "kobox/boot/fs_port.c",
    "kobox/boot/fs_service.c",
    "kobox/boot/fs_worker.c",
    "kobox/boot/fs_executor.c",
    "kobox/boot/fs_bench.c",
    "kobox/boot/fs_exec.c",
    "kobox/boot/fs_mount.c",
    "../protocol/src/filesystem.c",
)

ARCH_SOURCES = (
    "kobox/arch/x86_64/fpu.c",
    "kobox/arch/x86_64/registers.c",
    "kobox/arch/x86_64/task.c",
    "kobox/arch/x86_64/exception.c",
)

GATE_SOURCES = (
    "kobox/boot/exception_gate.c",
    "kobox/boot/drm_file_gate.c",
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

def support_sources(with_gates, device_profile="gpu"):
    if device_profile not in ("gpu", "usb-hid", "usb-storage", "nvme",
                              "virtio-net", "network", "amdgpu", "storage"):
        raise ValueError(f"unsupported device profile: {device_profile}")
    gpu = device_profile in ("gpu", "amdgpu")
    gates = GATE_SOURCES if gpu else tuple(
        source for source in GATE_SOURCES if source != "kobox/boot/drm_file_gate.c")
    device_sources = (GPU_SOURCES if gpu else
                      USB_INPUT_SOURCES + BLOCK_SOURCES if device_profile == "usb-storage" else
                      USB_INPUT_SOURCES if device_profile == "usb-hid" else
                      BLOCK_SOURCES if device_profile == "nvme" else
                      NET_SOURCES if device_profile in ("virtio-net", "network") else ())
    fs_sources = FS_SOURCES if device_profile == "storage" else ()
    if with_gates and not gpu:
        # Non-GPU profiles grant no virtio/GPU device; that device gate also
        # requires the unrelated DRM/sync_file subsystem in their core.
        gates = tuple(source for source in gates
                      if source != "kobox/boot/virtio_gate.c")
    if with_gates and device_profile == "storage":
        fs_sources += ("kobox/boot/fs_port_gate.c", "kobox/boot/fs_workload.c",
                       "kobox/boot/fs_worker_gate.c", "kobox/boot/lifecycle_gate.c")
    return (COMMON_SOURCES + ARCH_SOURCES + device_sources + fs_sources +
            (gates if with_gates else ()))
