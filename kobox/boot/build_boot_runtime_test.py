#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import importlib.util
import pathlib
import tempfile
import types
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "boot_build", pathlib.Path(__file__).with_name("build_boot_runtime.py")
)
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)
LOAD_SPEC = importlib.util.spec_from_file_location(
    "boot_load", pathlib.Path(__file__).with_name("inspect_core.py")
)
boot_load = importlib.util.module_from_spec(LOAD_SPEC)
LOAD_SPEC.loader.exec_module(boot_load)


class BootBuildTest(unittest.TestCase):
    def test_gate_sources_are_explicit_and_disjoint(self):
        production = boot.sources.support_sources(False)
        testing = boot.sources.support_sources(True)
        self.assertEqual(len(testing), len(set(testing)))
        self.assertFalse(set(production) & set(boot.sources.GATE_SOURCES))
        self.assertEqual(set(testing) - set(production), set(boot.sources.GATE_SOURCES))
        self.assertTrue(all("_gate.c" not in name for name in production))
        self.assertIn("kobox/task/port.c", production)
        self.assertIn("kobox/tests/gates/task_smp.c", boot.sources.GATE_SOURCES)
        self.assertIn("kobox/boot/vm_lifetime.c", boot.sources.GATE_SOURCES)

    def test_new_provider_overlay_invalidates_existing_kbuild_objects(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            arguments = types.SimpleNamespace(source_tree=source)
            before = boot.memory.include_overlay_identity(arguments)
            header = source / "kobox/provider/include/asm/pgtable.h"
            header.parent.mkdir(parents=True)
            header.write_text("#define KERNEL_PGD_BOUNDARY PGD_KERNEL_START\n")
            added = boot.memory.include_overlay_identity(arguments)
            self.assertNotEqual(before, added)
            self.assertEqual(added, boot.memory.include_overlay_identity(arguments))
            header.write_text("#define KERNEL_PGD_BOUNDARY 0\n")
            self.assertNotEqual(added, boot.memory.include_overlay_identity(arguments))
            header.unlink()
            self.assertEqual(before, boot.memory.include_overlay_identity(arguments))

    objects = [
        "init/main.o", "kernel/cpu.o", "kernel/smp.o", "kernel/softirq.o",
        "kernel/workqueue.o", "kernel/rcu/tree.o", "kernel/rcu/srcutree.o",
        "mm/shmem.o", "mm/filemap.o", "mm/vmscan.o", "fs/namespace.o",
        "fs/namei.o", "fs/open.o", "fs/read_write.o", "fs/file_table.o",
        "fs/file.o", "fs/inode.o", "fs/dcache.o", "fs/super.o",
        "mm/memory.o", "mm/mprotect.o", "mm/mmap.o", "kernel/fork.o",
        "kernel/cred.o", "kernel/groups.o",
        "fs/exec.o", "fs/binfmt_elf.o",
        "kernel/signal.o", "arch/x86/kernel/signal.o",
        "arch/x86/kernel/fpu/signal.o", "arch/x86/kernel/fpu/core.o",
        "net/socket.o", "net/core/scm.o", "net/unix/af_unix.o",
        "net/unix/garbage.o",
        "kernel/futex/core.o", "kernel/futex/syscalls.o", "kernel/futex/pi.o",
        "kernel/futex/requeue.o", "kernel/futex/waitwake.o",
        "kernel/kthread.o", "arch/x86/mm/fault.o", "arch/x86/mm/pgtable.o",
        "arch/x86/mm/tlb.o",
        "kernel/dma/mapping.o", "lib/scatterlist.o",
        "drivers/iommu/iommu.o", "drivers/iommu/dma-iommu.o", "drivers/iommu/iova.o",
        "kernel/irq/irqdomain.o", "kernel/irq/msi.o", "kernel/irq/manage.o",
        "kernel/irq/chip.o", "drivers/pci/msi/api.o", "drivers/pci/msi/msi.o",
        "drivers/pci/msi/irqdomain.o",
        "drivers/irqchip/irq-msi-lib.o",
    ]

    def test_memory_dma_and_irq_configuration_are_mandatory(self):
        task_config = (boot.SCRIPT_DIR.parent / "task/config").read_text()
        config = task_config + "\nCONFIG_PREEMPT_COUNT=y\nCONFIG_CONTEXT_TRACKING_IDLE=y\n"
        required_config = (boot.REQUIRED_MEMORY_CONFIG + boot.REQUIRED_DMA_CONFIG +
                           boot.REQUIRED_IRQ_CONFIG + boot.REQUIRED_CLIENT_CONFIG)
        config += "\n".join(required_config) + "\n"
        boot.validate_config(config)
        for required in required_config:
            for replacement in ("", required.replace("=y", "=m"),
                                "# " + required.replace("=y", " is not set")):
                with self.subTest(required=required, replacement=replacement), \
                        self.assertRaises(boot.BootBuildError):
                    boot.validate_config(config.replace(required, replacement))

    def test_load_segments_reject_page_permission_overlap(self):
        header = "LOAD 0 0 0 0x800 0x800 R 0x1000\n"
        code = "LOAD 0x1000 0x1000 0x1000 0x500 0x500 R E 0x1000\n"
        self.assertEqual(len(boot_load.load_segments(header + code, 4096)), 2)
        for changed in (
            header + code.replace("0x1000 0x1000 0x1000", "0x800 0x800 0x800"),
            header + code.replace("R E", "RW E"),
            code,
        ):
            with self.subTest(changed=changed), self.assertRaises(boot_load.boot.BootBuildError):
                boot_load.load_segments(changed, 4096)

    def test_boot_services_are_mandatory(self):
        boot.validate_source_order(self.objects)
        for owner in self.objects:
            with self.subTest(owner=owner), self.assertRaises(boot.BootBuildError):
                boot.validate_source_order([item for item in self.objects if item != owner])

    def test_escaping_or_duplicate_objects_are_rejected(self):
        for owner in ("../outside.o", "/outside.o", "init/main.o", "bad.a"):
            with self.subTest(owner=owner), self.assertRaises(boot.BootBuildError):
                boot.validate_source_order([*self.objects, owner])

    def test_missing_or_truncated_initcalls_are_rejected(self):
        original = {".initcallearly.init": 8, ".con_initcall.init": 4}
        boot.validate_initcalls(original, original, "service.o")
        for changed in ({}, {".initcallearly.init": 4, ".con_initcall.init": 4},
                        {**original, ".initcall6.init": 4}):
            with self.subTest(changed=changed), self.assertRaises(boot.BootBuildError):
                boot.validate_initcalls(original, changed, "service.o")

    def test_only_reviewed_machine_definitions_can_be_replaced(self):
        definitions = {
            "setup_arch": [{"source_object": "arch/x86/kernel/setup.o", "symbol_type": "T"}],
            "arch_cpu_idle_exit": [{
                "source_object": "kernel/sched/build_policy.o", "symbol_type": "W",
            }],
        }
        records = boot.validate_machine_overrides(set(definitions), definitions)
        self.assertEqual(len(records), 2)
        for symbol, owner, kind in (
            ("kernel_clone", "kernel/fork.o", "T"),
            ("get_signal", "kernel/signal.o", "T"),
            ("arch_do_signal_or_restart", "arch/x86/kernel/signal.o", "T"),
            ("__x64_sys_rt_sigreturn", "arch/x86/kernel/signal.o", "T"),
            ("fpu__restore_sig", "arch/x86/kernel/fpu/signal.o", "T"),
            ("copy_fpstate_to_sigframe", "arch/x86/kernel/fpu/signal.o", "T"),
            ("do_exit", "kernel/exit.o", "T"),
            ("copy_creds", "kernel/cred.o", "T"),
            ("commit_creds", "kernel/cred.o", "T"),
            ("begin_new_exec", "fs/exec.o", "T"),
            ("setup_arg_pages", "fs/exec.o", "T"),
            ("bprm_execve", "fs/exec.o", "T"),
            ("dup_fd", "fs/file.o", "T"),
            ("fd_install", "fs/file.o", "T"),
            ("f_dupfd", "fs/file.o", "T"),
            ("receive_fd", "fs/file.o", "T"),
            ("__sys_sendmsg", "net/socket.o", "T"),
            ("__sys_recvmsg", "net/socket.o", "T"),
            ("scm_detach_fds", "net/core/scm.o", "T"),
            ("unix_gc", "net/unix/garbage.o", "T"),
            ("do_futex", "kernel/futex/syscalls.o", "T"),
            ("futex_wait", "kernel/futex/waitwake.o", "T"),
            ("futex_wake", "kernel/futex/waitwake.o", "T"),
            ("schedule", "kernel/sched/core.o", "T"),
            ("schedule_timeout", "kernel/time/sleep_timeout.o", "T"),
            ("schedule_hrtimeout", "kernel/time/sleep_timeout.o", "T"),
            ("msleep_interruptible", "kernel/time/sleep_timeout.o", "T"),
            ("wait_for_completion_timeout", "kernel/sched/build_utility.o", "T"),
            ("prepare_to_wait_event", "kernel/sched/build_utility.o", "T"),
            ("send_sig", "kernel/signal.o", "T"),
            ("synchronize_rcu", "kernel/rcu/tree.o", "T"),
            ("synchronize_rcu_expedited", "kernel/rcu/tree.o", "T"),
            ("call_rcu", "kernel/rcu/tree.o", "T"),
            ("rcu_barrier", "kernel/rcu/tree.o", "T"),
            ("synchronize_srcu", "kernel/rcu/srcutree.o", "T"),
            ("synchronize_srcu_expedited", "kernel/rcu/srcutree.o", "T"),
            ("call_srcu", "kernel/rcu/srcutree.o", "T"),
            ("srcu_barrier", "kernel/rcu/srcutree.o", "T"),
            ("rcu_read_unlock_special", "kernel/rcu/tree.o", "T"),
            ("rcu_note_context_switch", "kernel/rcu/tree.o", "T"),
            ("alloc_workqueue_noprof", "kernel/workqueue.o", "T"),
            ("queue_work_on", "kernel/workqueue.o", "T"),
            ("queue_delayed_work_on", "kernel/workqueue.o", "T"),
            ("mod_delayed_work_on", "kernel/workqueue.o", "T"),
            ("cancel_work_sync", "kernel/workqueue.o", "T"),
            ("cancel_delayed_work_sync", "kernel/workqueue.o", "T"),
            ("flush_work", "kernel/workqueue.o", "T"),
            ("flush_delayed_work", "kernel/workqueue.o", "T"),
            ("__flush_workqueue", "kernel/workqueue.o", "T"),
            ("drain_workqueue", "kernel/workqueue.o", "T"),
            ("destroy_workqueue", "kernel/workqueue.o", "T"),
            ("workqueue_softirq_action", "kernel/workqueue.o", "T"),
            ("wq_worker_sleeping", "kernel/workqueue.o", "T"),
            ("wq_worker_running", "kernel/workqueue.o", "T"),
            ("current_is_workqueue_rescuer", "kernel/workqueue.o", "T"),
            ("disable_work_sync", "kernel/workqueue.o", "T"),
            ("disable_delayed_work_sync", "kernel/workqueue.o", "T"),
            ("timer_shutdown_sync", "kernel/time/timer.o", "T"),
            ("hrtimer_cancel", "kernel/time/hrtimer.o", "T"),
            ("synchronize_irq", "kernel/irq/manage.o", "T"),
            ("synchronize_hardirq", "kernel/irq/manage.o", "T"),
            ("request_threaded_irq", "kernel/irq/manage.o", "T"),
            ("free_irq", "kernel/irq/manage.o", "T"),
            ("handle_level_irq", "kernel/irq/chip.o", "T"),
            ("handle_edge_irq", "kernel/irq/chip.o", "T"),
            ("__irq_domain_alloc_irqs", "kernel/irq/irqdomain.o", "T"),
            ("irq_domain_free_irqs", "kernel/irq/irqdomain.o", "T"),
            ("pci_alloc_irq_vectors_affinity", "drivers/pci/msi/api.o", "T"),
            ("pci_free_irq_vectors", "drivers/pci/msi/api.o", "T"),
            ("pci_msi_create_irq_domain", "drivers/pci/msi/irqdomain.o", "T"),
            ("pci_msi_mask_irq", "drivers/pci/msi/msi.o", "T"),
            ("pci_msi_unmask_irq", "drivers/pci/msi/msi.o", "T"),
            ("msi_domain_alloc_irqs_all_locked", "kernel/irq/msi.o", "T"),
            ("vfree", "mm/vmalloc.o", "T"),
            ("mm_alloc", "kernel/fork.o", "T"),
            ("mmput", "kernel/fork.o", "T"),
            ("kthread_use_mm", "kernel/kthread.o", "T"),
            ("kthread_unuse_mm", "kernel/kthread.o", "T"),
            ("vm_mmap", "mm/util.o", "T"),
            ("vm_munmap", "mm/vma.o", "T"),
            ("__x64_sys_mprotect", "mm/mprotect.o", "T"),
            ("fixup_user_fault", "mm/gup.o", "T"),
            ("strncpy_from_user", "lib/strncpy_from_user.o", "T"),
            ("strnlen_user", "lib/strnlen_user.o", "T"),
            ("futex_wait", "kernel/futex/waitwake.o", "T"),
            ("futex_wake", "kernel/futex/waitwake.o", "T"),
            ("x64_sys_call", "arch/x86/entry/syscall_64.o", "T"),
            ("do_arch_prctl_64", "arch/x86/kernel/process_64.o", "T"),
            ("flush_thread", "arch/x86/kernel/process.o", "T"),
            ("start_thread", "arch/x86/kernel/process_64.o", "T"),
            ("x86_fsbase_read_task", "arch/x86/kernel/process_64.o", "T"),
            ("x86_gsbase_read_task", "arch/x86/kernel/process_64.o", "T"),
            ("syscall_trace_enter", "kernel/entry/syscall-common.o", "T"),
            ("syscall_exit_work", "kernel/entry/syscall-common.o", "T"),
            ("handle_mm_fault", "mm/memory.o", "T"),
            ("do_user_addr_fault", "arch/x86/mm/fault.o", "T"),
            ("unmap_mapping_range", "mm/memory.o", "T"),
            ("shmem_file_setup", "mm/shmem.o", "T"),
            ("shmem_file_setup_with_mnt", "mm/shmem.o", "T"),
            ("shmem_read_folio_gfp", "mm/shmem.o", "T"),
            ("shmem_get_folio", "mm/shmem.o", "T"),
            ("shmem_truncate_range", "mm/shmem.o", "T"),
            ("folio_add_lru", "mm/swap.o", "T"),
            ("__fput_sync", "fs/file_table.o", "T"),
            ("vfs_caches_init", "fs/dcache.o", "T"),
            ("kern_mount", "fs/namespace.o", "T"),
            ("kern_unmount", "fs/namespace.o", "T"),
            ("file_open_root", "fs/open.o", "T"),
            ("vfs_unlink", "fs/namei.o", "T"),
            ("kernel_read", "fs/read_write.o", "T"),
            ("kernel_write", "fs/read_write.o", "T"),
            ("vfs_truncate", "fs/open.o", "T"),
            ("vfs_fallocate", "fs/open.o", "T"),
            ("notify_change", "fs/attr.o", "T"),
            ("filemap_remove_folio", "mm/filemap.o", "T"),
            ("vm_memory_committed", "mm/util.o", "T"),
            ("close_fd", "fs/file.o", "T"),
            ("fput", "fs/file_table.o", "T"),
            ("flush_delayed_fput", "fs/file_table.o", "T"),
            ("iput", "fs/inode.o", "T"),
            ("task_work_run", "kernel/task_work.o", "T"),
            ("pci_scan_root_bus_bridge", "drivers/pci/probe.o", "T"),
            ("pci_bus_add_devices", "drivers/pci/bus.o", "T"),
            ("pci_find_capability", "drivers/pci/pci.o", "T"),
            ("pci_iomap", "drivers/pci/iomap.o", "T"),
            ("pci_iounmap", "drivers/pci/iomap.o", "T"),
            ("ioremap", "arch/x86/mm/ioremap.o", "T"),
            ("ioremap_wc", "arch/x86/mm/ioremap.o", "T"),
            ("memtype_reserve", "arch/x86/mm/pat/memtype.o", "T"),
            ("dma_map_phys", "kernel/dma/mapping.o", "T"),
            ("dma_map_sg_attrs", "kernel/dma/mapping.o", "T"),
            ("iommu_dma_map_phys", "drivers/iommu/dma-iommu.o", "T"),
            ("iommu_dma_map_sg", "drivers/iommu/dma-iommu.o", "T"),
            ("iommu_map", "drivers/iommu/iommu.o", "T"),
            ("alloc_iova_fast", "drivers/iommu/iova.o", "T"),
            ("__boot_cpu_id", "kernel/cpu.o", "B"),
            ("setup_arch", "kernel/new_owner.o", "T"),
            ("arch_cpu_idle_exit", "kernel/sched/build_policy.o", "T"),
            ("new_port", "arch/x86/kernel/setup.o", "T"),
        ):
            with self.subTest(symbol=symbol), self.assertRaises(boot.BootBuildError):
                boot.validate_machine_overrides({symbol}, {symbol: [{
                    "source_object": owner, "symbol_type": kind,
                }]})

    def test_service_startup_stays_in_upstream_boot(self):
        boot.validate_startup_ownership({"start_kernel", "notify_cpu_starting"})
        for symbol in boot.UPSTREAM_STARTUP_ONLY:
            with self.subTest(symbol=symbol), self.assertRaises(boot.BootBuildError):
                boot.validate_startup_ownership({symbol})

    def test_machine_recipe_preserves_kbuild_checks(self):
        recipe = ("clang-18 -Wp,-MMD,arch/.entry.o.d -D__ASSEMBLY__ "
                  "-c -o arch/entry.o /source/entry.S ; "
                  "./tools/objtool/objtool --static-call arch/entry.o")
        commands = boot.machine_compile_commands(
            recipe, "arch/entry.o", pathlib.Path("/source/entry.S"),
            pathlib.Path("/staged/entry.S"), pathlib.Path("/output/entry.o")
        )
        self.assertEqual(commands[0][-1], "/staged/entry.S")
        self.assertIn("-Wp,-MMD,/output/entry.d", commands[0])
        self.assertEqual(commands[1], ["./tools/objtool/objtool", "--static-call",
                                       "/output/entry.o"])
        for changed in (recipe.replace(" ; ", " && "),
                        recipe.replace("/source/entry.S", "/another/entry.S"),
                        recipe.replace("./tools/objtool/objtool", "true")):
            with self.subTest(changed=changed), self.assertRaises(boot.BootBuildError):
                boot.machine_compile_commands(
                    changed, "arch/entry.o", pathlib.Path("/source/entry.S"),
                    pathlib.Path("/staged/entry.S"), pathlib.Path("/output/entry.o")
                )

    def test_ancestor_permission_patch_preserves_pinned_source(self):
        source_tree = boot.SCRIPT_DIR.parents[1]
        name = "arch/x86/mm/pat/set_memory.c"
        original = (source_tree / name).read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            arguments = types.SimpleNamespace(source_tree=source_tree,
                                              output_dir=pathlib.Path(directory))
            staged = boot.memory.stage_native_source(arguments, name)
            self.assertEqual((source_tree / name).read_bytes(), original)
            expected = original.decode()
            for level in ("pgd", "p4d", "pud", "pmd"):
                before = f"*rw &= {level}_flags(*{level}) & _PAGE_RW;"
                after = f"*rw &= !!({level}_flags(*{level}) & _PAGE_RW);"
                self.assertEqual(expected.count(before), 1)
                expected = expected.replace(before, after)
            self.assertEqual(staged.read_text(), expected)
            record = arguments.native_source_patches[name]
            self.assertEqual(record["source_sha256"], boot.memory.sha256(source_tree / name))
            self.assertEqual(record["hosted_source_sha256"], boot.memory.sha256(staged))

    def test_isolated_image_bounds_include_large_model_bss(self):
        source = boot.SCRIPT_DIR.parent / "provider/provider.lds"
        original = source.read_text()
        hosted = boot.memory.isolated_linker_script(original)
        self.assertIn("_text = __ehdr_start;", hosted)
        self.assertIn("*(.lbss .lbss.*)", hosted)
        self.assertIn("INSERT AFTER .bss;", hosted)
        self.assertIn("__bss_stop = ADDR(.kobox_image_bss) + SIZEOF(.kobox_image_bss);", hosted)
        self.assertNotIn("*(.ltext.*)", hosted)
        self.assertEqual(source.read_text(), original)
        for marker in ("_text = ADDR(.ltext);", "__bss_stop = ADDR(.bss) + SIZEOF(.bss);"):
            with self.subTest(marker=marker), self.assertRaises(boot.memory.MemoryBuildError):
                boot.memory.isolated_linker_script(original.replace(marker, ""))

    def test_isolated_image_rejects_load_outside_ram_bounds(self):
        arguments = types.SimpleNamespace(nm="nm", readelf="readelf")
        symbols = "_text r 0\n__bss_stop b 4000\n"
        headers = "LOAD 0 0 0 0x1000 0x1000 R 0x1000\n"
        headers += "LOAD 0x2000 0x2000 0x2000 0x1000 0x2000 RW 0x1000\n"
        with mock.patch.object(boot.memory, "run", side_effect=[symbols, headers]):
            boot.memory.validate_isolated_image(arguments, pathlib.Path("core.so"))
        for bad_symbols, bad_headers in (
            (symbols.replace("4000", "3000"), headers),
            (symbols.replace("4000", "4001"), headers),
            ("_text r 0\n", headers),
            (symbols, ""),
            (symbols.replace("r 0", "r 1000"), headers),
        ):
            with self.subTest(symbols=bad_symbols, headers=bad_headers), \
                    mock.patch.object(boot.memory, "run", side_effect=[bad_symbols, bad_headers]), \
                    self.assertRaises(boot.memory.MemoryBuildError):
                boot.memory.validate_isolated_image(arguments, pathlib.Path("core.so"))


if __name__ == "__main__":
    unittest.main()
