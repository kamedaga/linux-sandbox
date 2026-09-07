# SPDX-License-Identifier: GPL-2.0-only

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME kobox2.closure_inventory
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/manifest/generate_closure_inventory_test.py"
)
add_test(
	NAME kobox2.boot_core_inventory
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/manifest/generate_boot_core_inventory_test.py"
)
add_test(
    NAME kobox2.shared_provider_build
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/provider/build_shared_providers_test.py"
)
add_test(
	NAME kobox2.closure_link_plan
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/manifest/generate_link_plan_test.py"
)

add_library(
	kobox_link_plan_loader STATIC
	loader/link_plan_loader.c
	loader/elf64_loader.c
)
target_compile_features(kobox_link_plan_loader PRIVATE c_std_11)
target_compile_options(
	kobox_link_plan_loader PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_include_directories(
	kobox_link_plan_loader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/loader"
)
target_link_libraries(
	kobox_link_plan_loader PUBLIC kobox2::protocol ${CMAKE_DL_LIBS}
)

set(KOBOX_LINUX_BOOT_BUILD_DIR "" CACHE PATH
	"Canonical Linux build directory for the boot-core gate")
set(KOBOX_LINUX_NM "llvm-nm-18" CACHE STRING
	"nm command used by the Linux core gates")
set(KOBOX_LINUX_READELF "llvm-readelf-18" CACHE STRING
	"readelf command used by the Linux core gates")
if(KOBOX_LINUX_BOOT_BUILD_DIR)
	set(_kobox_linux_profile
		"${CMAKE_CURRENT_SOURCE_DIR}/manifest/profiles/virtio_gpu_virgl.json")
	set(_kobox_boot_inventory
		"${CMAKE_CURRENT_BINARY_DIR}/generated/linux_boot_core.json")
	set(_kobox_driver_inventory
		"${CMAKE_CURRENT_BINARY_DIR}/generated/virtio_gpu_virgl.json")
	add_test(
		NAME kobox2.linux_boot_core_gate
		COMMAND
			"${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/manifest/generate_boot_core_inventory.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--build-dir "${KOBOX_LINUX_BOOT_BUILD_DIR}"
			--profile "${_kobox_linux_profile}"
			--nm "${KOBOX_LINUX_NM}"
			--readelf "${KOBOX_LINUX_READELF}"
			--output "${_kobox_boot_inventory}"
	)
	add_test(
		NAME kobox2.linux_driver_closure_gate
		COMMAND
			"${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/manifest/generate_closure_inventory.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--build-dir "${KOBOX_LINUX_BOOT_BUILD_DIR}"
			--profile "${_kobox_linux_profile}"
			--nm "${KOBOX_LINUX_NM}"
			--output "${_kobox_driver_inventory}"
	)
	set_tests_properties(
		kobox2.linux_boot_core_gate kobox2.linux_driver_closure_gate
		PROPERTIES LABELS "linux-runtime;integration" TIMEOUT 30
	)
endif()

add_executable(
	kobox_linux_process_lifecycle_test
	runtime/linux_process_lifecycle.c
	runtime/linux_process_lifecycle_test.c
)
target_compile_features(kobox_linux_process_lifecycle_test PRIVATE c_std_11)
target_compile_options(
	kobox_linux_process_lifecycle_test
	PRIVATE -Wall -Wextra -Wpedantic -Werror
)
add_test(
	NAME kobox2.linux_process_lifecycle
	COMMAND kobox_linux_process_lifecycle_test
)

add_library(
	kobox_posix_host STATIC
	host/posix/clock.c
	host/posix/cpu.c
	host/posix/memory.c
	host/posix/permit.c
	host/posix/task.c
	host/posix/thread.c
	host/posix/timer.c
)
enable_language(ASM)
add_library(kobox_posix_vm STATIC host/posix/vm.c host/posix/vm_service.c)
target_compile_features(kobox_posix_vm PRIVATE c_std_11)
target_compile_options(kobox_posix_vm PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm PUBLIC kobox_posix_host)
add_executable(kobox_posix_vm_client host/posix/vm_client.c host/posix/vm_syscall.S)
target_compile_features(kobox_posix_vm_client PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_client PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(kobox_posix_vm_test host/posix/vm_test.c)
target_compile_features(kobox_posix_vm_test PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_vm_transport
	COMMAND kobox_posix_vm_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_vm_transport PROPERTIES TIMEOUT 30)
add_executable(kobox_posix_vm_service_test host/posix/vm_service_test.c)
target_compile_features(kobox_posix_vm_service_test PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_service_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm_service_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_vm_service
	COMMAND kobox_posix_vm_service_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_vm_service PROPERTIES TIMEOUT 30)
target_compile_features(kobox_posix_host PUBLIC c_std_11)
set_target_properties(kobox_posix_host PROPERTIES C_EXTENSIONS OFF)
target_compile_definitions(
	kobox_posix_host PUBLIC _POSIX_C_SOURCE=200809L
)
target_compile_options(
	kobox_posix_host PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_include_directories(
	kobox_posix_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host/posix"
)
target_link_libraries(kobox_posix_host PUBLIC Threads::Threads)

add_executable(kobox_posix_host_test host/posix/host_test.c)
target_compile_features(kobox_posix_host_test PRIVATE c_std_11)
set_target_properties(kobox_posix_host_test PROPERTIES C_EXTENSIONS OFF)
target_compile_options(
	kobox_posix_host_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(kobox_posix_host_test PRIVATE kobox_posix_host)
add_test(NAME kobox2.posix_host_gate COMMAND kobox_posix_host_test)
add_test(
	NAME kobox2.posix_host_surface_unit
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/host/posix/verify_surface_test.py"
)
add_test(
	NAME kobox2.posix_host_surface_gate
	COMMAND
		"${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/host/posix/verify_surface.py"
		--archive $<TARGET_FILE:kobox_posix_host>
		--nm "${KOBOX_LINUX_NM}"
)
set_tests_properties(
	kobox2.posix_host_gate kobox2.posix_host_surface_gate
	PROPERTIES LABELS "linux-runtime;go-no-go" TIMEOUT 15
)

if(KOBOX_LINUX_BOOT_BUILD_DIR)
	set(_kobox_memory_output "${CMAKE_CURRENT_BINARY_DIR}/linux-memory")
	set(_kobox_memory_provider_build
		"${CMAKE_CURRENT_BINARY_DIR}/linux-memory-provider")
	set(_kobox_memory_core
		"${_kobox_memory_output}/linux-early-memory-gate.so")
	set(_kobox_memory_boundary
		"${_kobox_memory_output}/linux-early-memory-boundary.so")
	set(_kobox_memory_inventory
		"${_kobox_memory_output}/inventory.json")
	add_custom_command(
		OUTPUT
			"${_kobox_memory_core}"
			"${_kobox_memory_boundary}"
			"${_kobox_memory_inventory}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${_kobox_memory_output}"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/build_early_memory.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--canonical-build-dir "${KOBOX_LINUX_BOOT_BUILD_DIR}"
			--provider-build-dir "${_kobox_memory_provider_build}"
			--output-dir "${_kobox_memory_output}"
			--protocol-include
				"${PROJECT_SOURCE_DIR}/protocol/generated/include"
			--inventory "${_kobox_memory_inventory}"
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/build_early_memory.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/early_boot.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/patches/ancestor-rw.patch"
			"${CMAKE_CURRENT_SOURCE_DIR}/../arch/x86/mm/pat/set_memory.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/host.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/port.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/build_shared_providers.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/provider.lds"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/cacheflush.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/pgtable_64_types.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/linux/init.h"
			"${KOBOX_LINUX_BOOT_BUILD_DIR}/.config"
			"${KOBOX_LINUX_BOOT_BUILD_DIR}/vmlinux"
			"${KOBOX_LINUX_BOOT_BUILD_DIR}/vmlinux.a"
		VERBATIM
	)
	add_custom_target(kobox_linux_early_memory_artifact ALL DEPENDS
		"${_kobox_memory_core}"
		"${_kobox_memory_boundary}"
		"${_kobox_memory_inventory}"
	)
	add_executable(kobox_linux_early_memory_test memory/early_boot_test.c)
	target_compile_features(kobox_linux_early_memory_test PRIVATE c_std_11)
	set_target_properties(kobox_linux_early_memory_test PROPERTIES
		C_EXTENSIONS OFF)
	target_compile_options(kobox_linux_early_memory_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_include_directories(kobox_linux_early_memory_test
		PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/memory")
	target_link_libraries(kobox_linux_early_memory_test
		PRIVATE kobox_posix_host ${CMAKE_DL_LIBS})
	add_dependencies(kobox_linux_early_memory_test
		kobox_linux_early_memory_artifact)
	add_test(
		NAME kobox2.linux_early_memory_gate
		COMMAND kobox_linux_early_memory_test
			"${_kobox_memory_boundary}"
			"${_kobox_memory_core}"
	)
	set_tests_properties(kobox2.linux_early_memory_gate PROPERTIES
		LABELS "linux-runtime;go-no-go" TIMEOUT 30)
endif()

set(KOBOX_LINUX_TASK_BUILD_DIR "" CACHE PATH
	"Canonical Linux build with the task/config preemption and SMP profile")
set(KOBOX_LINUX_BOOT_RUNTIME_CORE "" CACHE FILEPATH
	"Full core built by boot/build_boot_runtime.py --link, for the integrated Gate")
set(KOBOX_LINUX_GEM_MODULE_DIR "" CACHE PATH
	"Native modules built by boot/build_gem_modules.py for the same full core")
option(KOBOX_LINUX_MEMORY_PRESSURE_GATES
	"Run RAM/reclaim and allocator failure gates (core requires boot/pressure.config)" OFF)
if(KOBOX_LINUX_GEM_MODULE_DIR AND NOT KOBOX_LINUX_BOOT_RUNTIME_CORE)
	message(FATAL_ERROR "Native module tests require KOBOX_LINUX_BOOT_RUNTIME_CORE")
endif()
if(KOBOX_LINUX_BOOT_RUNTIME_CORE AND NOT KOBOX_LINUX_TASK_BUILD_DIR)
	message(FATAL_ERROR "The boot runtime Gate requires KOBOX_LINUX_TASK_BUILD_DIR")
endif()
if(KOBOX_LINUX_TASK_BUILD_DIR)
	set(_kobox_task_output "${CMAKE_CURRENT_BINARY_DIR}/linux-task")
	set(_kobox_task_provider_build
		"${CMAKE_CURRENT_BINARY_DIR}/linux-task-provider")
	set(_kobox_task_core "${_kobox_task_output}/linux-task-smp-gate.so")
	set(_kobox_task_boundary
		"${_kobox_task_output}/linux-task-smp-boundary.so")
	set(_kobox_task_inventory "${_kobox_task_output}/inventory.json")
	add_custom_command(
		OUTPUT
			"${_kobox_task_core}"
			"${_kobox_task_boundary}"
			"${_kobox_task_inventory}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${_kobox_task_output}"
		COMMAND
			"${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/build_task_smp.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--canonical-build-dir "${KOBOX_LINUX_TASK_BUILD_DIR}"
			--provider-build-dir "${_kobox_task_provider_build}"
			--output-dir "${_kobox_task_output}"
			--protocol-include
				"${PROJECT_SOURCE_DIR}/protocol/generated/include"
			--inventory "${_kobox_task_inventory}"
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/task/build_task_smp.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/port.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_port.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_gate.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_port.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/host.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/boot.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/current.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/irq_stack.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/mmu_context.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/preempt.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/smp.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/switch_to.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/build_early_memory.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/early_boot.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/patches/ancestor-rw.patch"
			"${CMAKE_CURRENT_SOURCE_DIR}/../arch/x86/mm/pat/set_memory.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/host.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/memory/port.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/build_shared_providers.py"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/provider.lds"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/cacheflush.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/irqflags.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/percpu.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/asm/pgtable_64_types.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/include/linux/init.h"
			"${KOBOX_LINUX_TASK_BUILD_DIR}/.config"
			"${KOBOX_LINUX_TASK_BUILD_DIR}/vmlinux"
			"${KOBOX_LINUX_TASK_BUILD_DIR}/vmlinux.a"
		VERBATIM
	)
	add_custom_target(kobox_linux_task_smp_artifact ALL DEPENDS
		"${_kobox_task_core}"
		"${_kobox_task_boundary}"
		"${_kobox_task_inventory}"
	)
	add_library(kobox_linux_posix_machine STATIC task/posix_machine.c)
	target_compile_features(kobox_linux_posix_machine PRIVATE c_std_11)
	target_compile_options(kobox_linux_posix_machine
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_posix_machine PUBLIC kobox_posix_host)
	add_executable(kobox_linux_boot_test
		boot/boot_test.c boot/image.c boot/exception_posix.c mm/posix.c)
	target_compile_features(kobox_linux_boot_test PRIVATE c_std_11)
	target_compile_options(kobox_linux_boot_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_boot_test
		PRIVATE kobox_linux_posix_machine kobox_posix_vm ${CMAKE_DL_LIBS})
	if(KOBOX_LINUX_BOOT_RUNTIME_CORE)
		get_filename_component(_kobox_boot_runtime_dir
			"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" DIRECTORY)
		add_test(NAME kobox2.linux_boot_runtime_load
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/boot/load_test.py"
				--core "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
				--inputs "${_kobox_boot_runtime_dir}/linux-boot-inputs.json")
		set_tests_properties(kobox2.linux_boot_runtime_load PROPERTIES
			FIXTURES_SETUP kobox_full_boot_core TIMEOUT 30)
		add_test(NAME kobox2.linux_boot_service_gate
			COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_boot_service_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 30)
		if(KOBOX_LINUX_GEM_MODULE_DIR)
			set(_kobox_gem_modules)
			foreach(_module IN ITEMS drivers/i2c/i2c-core.ko
					drivers/gpu/drm/drm_panel_orientation_quirks.ko
					drivers/gpu/drm/drm.ko drivers/gpu/drm/drm_shmem_helper.ko)
				if(NOT EXISTS "${KOBOX_LINUX_GEM_MODULE_DIR}/${_module}")
					message(FATAL_ERROR "Missing native module: ${_module}")
				endif()
				list(APPEND _kobox_gem_modules "${KOBOX_LINUX_GEM_MODULE_DIR}/${_module}")
			endforeach()
			add_test(NAME kobox2.linux_gem_module_prerequisite
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--modules ${_kobox_gem_modules})
			set_tests_properties(kobox2.linux_gem_module_prerequisite PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;linux-gem-prerequisite" TIMEOUT 30)
			if(NOT EXISTS "${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
				message(FATAL_ERROR "Missing native GEM lifetime test module")
			endif()
			if(KOBOX_LINUX_MEMORY_PRESSURE_GATES)
				foreach(_cleanup_case IN ITEMS vma object death_vma death_object)
					set(_cleanup_argument --gem-cleanup)
					if(_cleanup_case MATCHES "^death_")
						string(APPEND _cleanup_argument "-death")
					endif()
					if(_cleanup_case MATCHES "object$")
						string(APPEND _cleanup_argument "-object-last")
					endif()
					add_test(NAME "kobox2.linux_gem_cleanup_${_cleanup_case}"
						COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
							${_cleanup_argument} $<TARGET_FILE:kobox_posix_vm_client>
							${_kobox_gem_modules}
							"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
					set_tests_properties("kobox2.linux_gem_cleanup_${_cleanup_case}" PROPERTIES
						FIXTURES_REQUIRED kobox_full_boot_core
						LABELS "linux-runtime;integration;linux-buffer-cleanup" TIMEOUT 60)
				endforeach()
				foreach(_failure_case IN ITEMS vma object)
					if(_failure_case STREQUAL "vma")
						set(_failure_argument --gem-failure)
					else()
						set(_failure_argument --gem-failure-object-last)
					endif()
					add_test(NAME "kobox2.linux_gem_failure_${_failure_case}"
						COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
							${_failure_argument} $<TARGET_FILE:kobox_posix_vm_client>
							${_kobox_gem_modules}
							"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
					set_tests_properties("kobox2.linux_gem_failure_${_failure_case}" PROPERTIES
						FIXTURES_REQUIRED kobox_full_boot_core
						LABELS "linux-runtime;integration;linux-gem-pressure" TIMEOUT 60)
				endforeach()
			endif()
			foreach(_case IN ITEMS vma_last object_last)
				if(_case STREQUAL "vma_last")
					set(_argument --gem)
				else()
					set(_argument --gem-object-last)
				endif()
				add_test(NAME kobox2.linux_gem_lifetime_${_case}
					COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						${_argument} $<TARGET_FILE:kobox_posix_vm_client> ${_kobox_gem_modules}
						"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
				set_tests_properties(kobox2.linux_gem_lifetime_${_case} PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core
					LABELS "linux-runtime;integration;linux-gem-lifetime" TIMEOUT 30)
			endforeach()
		endif()
		add_test(NAME kobox2.linux_timed_wait_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --timed-wait)
		set_tests_properties(kobox2.linux_timed_wait_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 60)
		add_test(NAME kobox2.linux_rcu_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --rcu)
		set_tests_properties(kobox2.linux_rcu_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 60)
		add_test(NAME kobox2.linux_workqueue_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --workqueue)
		set_tests_properties(kobox2.linux_workqueue_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 90)
		add_test(NAME kobox2.linux_cleanup_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --cleanup)
		set_tests_properties(kobox2.linux_cleanup_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 90)
		add_test(NAME kobox2.linux_full_foundation_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --all)
		set_tests_properties(kobox2.linux_full_foundation_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 120)
		add_test(NAME kobox2.linux_vfs_lifetime_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --vfs)
		set_tests_properties(kobox2.linux_vfs_lifetime_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 90)
		add_test(NAME kobox2.linux_shmem_pagecache_gate
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --shmem)
		set_tests_properties(kobox2.linux_shmem_pagecache_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 90)
		if(KOBOX_LINUX_MEMORY_PRESSURE_GATES)
			foreach(_pressure_case IN ITEMS pressure alloc-failure)
				add_test(NAME "kobox2.linux_memory_${_pressure_case}"
					COMMAND kobox_linux_boot_test
						"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" "--${_pressure_case}")
				set_tests_properties("kobox2.linux_memory_${_pressure_case}" PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core
					LABELS "linux-runtime;integration;linux-memory-pressure" TIMEOUT 60)
			endforeach()
			add_test(NAME kobox2.linux_mm_vma_probe_pressure_fault
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--vm-probe-pressure-fault $<TARGET_FILE:kobox_posix_vm_client>)
			set_tests_properties(kobox2.linux_mm_vma_probe_pressure_fault PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;linux-buffer-cleanup" TIMEOUT 60)
		endif()
		foreach(_vm_case IN ITEMS probe probe_ro probe_reuse probe_irq probe_truncate
					  probe_late_fault probe_exit_publish probe_exit probe_lifetime
					  probe_death probe_rollback)
			string(REPLACE "_" "-" _vm_option "${_vm_case}")
			add_test(NAME "kobox2.linux_mm_vma_${_vm_case}"
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					"--vm-${_vm_option}" $<TARGET_FILE:kobox_posix_vm_client>)
			set_tests_properties("kobox2.linux_mm_vma_${_vm_case}" PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;go-no-go;linux-mm-vma" TIMEOUT 30)
		endforeach()
	endif()
	add_executable(kobox_linux_exception_test
		boot/exception_test.c boot/exception_posix.c)
	target_compile_features(kobox_linux_exception_test PRIVATE c_std_11)
	target_compile_options(kobox_linux_exception_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_exception_test PRIVATE kobox_posix_host)
	add_test(NAME kobox2.linux_exception_transport COMMAND kobox_linux_exception_test)
	set_tests_properties(kobox2.linux_exception_transport PROPERTIES TIMEOUT 30)
	add_executable(kobox_linux_boot_image_test boot/image_test.c boot/image.c)
	target_compile_features(kobox_linux_boot_image_test PRIVATE c_std_11)
	target_compile_options(kobox_linux_boot_image_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_boot_image_test
		PRIVATE kobox_posix_host ${CMAKE_DL_LIBS})
	add_executable(kobox_linux_task_smp_test task/task_smp_test.c)
	target_compile_features(kobox_linux_task_smp_test PRIVATE c_std_11)
	set_target_properties(kobox_linux_task_smp_test PROPERTIES C_EXTENSIONS OFF)
	target_compile_options(kobox_linux_task_smp_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_include_directories(kobox_linux_task_smp_test
		PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/task")
	target_link_libraries(kobox_linux_task_smp_test
		PRIVATE kobox_linux_posix_machine ${CMAKE_DL_LIBS})
	add_dependencies(kobox_linux_task_smp_test kobox_linux_task_smp_artifact)
	add_test(
		NAME kobox2.linux_task_smp_gate
		COMMAND kobox_linux_task_smp_test
			"${_kobox_task_boundary}"
			"${_kobox_task_core}"
	)
	set_tests_properties(kobox2.linux_task_smp_gate PROPERTIES
		LABELS "linux-runtime;go-no-go" TIMEOUT 30)
	add_test(NAME kobox2.linux_task_smp_build_unit
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/build_task_smp_test.py")
	add_test(NAME kobox2.linux_boot_runtime_build_unit
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/boot/build_boot_runtime_test.py")
	add_test(NAME kobox2.linux_gem_module_build_unit
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/boot/build_gem_modules_test.py")
	add_test(NAME kobox2.linux_boot_elf_layout
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/boot/layout_test.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--provider-build-dir "${KOBOX_LINUX_TASK_BUILD_DIR}"
			--output-dir "${CMAKE_CURRENT_BINARY_DIR}/linux-boot-layout-test")
	set_tests_properties(kobox2.linux_boot_elf_layout PROPERTIES TIMEOUT 30)
	add_test(NAME kobox2.linux_boot_image_alias
		COMMAND kobox_linux_boot_image_test
			"${CMAKE_CURRENT_BINARY_DIR}/linux-boot-layout-test/layout-fixture.so")
	set_tests_properties(kobox2.linux_boot_elf_layout PROPERTIES
		FIXTURES_SETUP kobox_boot_layout)
	set_tests_properties(kobox2.linux_boot_image_alias PROPERTIES
		FIXTURES_REQUIRED kobox_boot_layout TIMEOUT 30)
endif()

add_executable(kobox_provider_arena_test provider/arena.c provider/arena_test.c)
target_compile_features(kobox_provider_arena_test PRIVATE c_std_11)
target_compile_options(
	kobox_provider_arena_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(kobox_provider_arena_test PRIVATE Threads::Threads)
add_test(NAME kobox2.provider_arena COMMAND kobox_provider_arena_test)

add_executable(
	kobox_provider_lifecycle_test
	provider/lifecycle.c
	provider/lifecycle_test.c
)
target_compile_features(kobox_provider_lifecycle_test PRIVATE c_std_11)
target_compile_options(
	kobox_provider_lifecycle_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
add_test(NAME kobox2.provider_lifecycle COMMAND kobox_provider_lifecycle_test)

add_executable(
	kobox_device_pci_lifecycle_test
	provider/device_pci_lifecycle.c
	provider/device_pci_lifecycle_test.c
)
target_compile_features(kobox_device_pci_lifecycle_test PRIVATE c_std_11)
target_compile_options(
	kobox_device_pci_lifecycle_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(kobox_device_pci_lifecycle_test PRIVATE kobox2::protocol)
add_test(
	NAME kobox2.device_pci_lifecycle COMMAND kobox_device_pci_lifecycle_test
)

add_executable(
	kobox_linux_memory_resource_test
	runtime/memory_resource.c
	runtime/memory_resource_test.c
	provider/arena.c
)
target_compile_features(kobox_linux_memory_resource_test PRIVATE c_std_11)
target_compile_options(
	kobox_linux_memory_resource_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(kobox_linux_memory_resource_test PRIVATE kobox2::protocol)
add_test(
	NAME kobox2.linux_memory_resource COMMAND kobox_linux_memory_resource_test
)

add_executable(
	kobox_linux_core_lifecycle_test
	provider/core_lifecycle.c
	provider/lifecycle.c
	provider/arena.c
	runtime/memory_resource.c
	provider/core_lifecycle_test.c
)
target_compile_features(kobox_linux_core_lifecycle_test PRIVATE c_std_11)
target_compile_options(
	kobox_linux_core_lifecycle_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(
	kobox_linux_core_lifecycle_test PRIVATE kobox2::protocol Threads::Threads
)
add_test(
	NAME kobox2.linux_core_lifecycle COMMAND kobox_linux_core_lifecycle_test
)

set(KOBOX_CORE_ARENA_FIXTURE_OUTPUT_DIRECTORY
	"${CMAKE_BINARY_DIR}/linux-core-arena-fixture"
)
set(KOBOX_CORE_ARENA_FIXTURE_MODULE
	"${KOBOX_CORE_ARENA_FIXTURE_OUTPUT_DIRECTORY}/core_arena_consumer.ko"
)
set(KOBOX_CORE_ARENA_LEAK_FIXTURE_MODULE
	"${KOBOX_CORE_ARENA_FIXTURE_OUTPUT_DIRECTORY}/core_arena_leak_consumer.ko"
)

add_library(
	kobox_core_arena_fixture SHARED
	provider/core_lifecycle.c
	provider/lifecycle.c
	provider/arena.c
)
target_compile_features(kobox_core_arena_fixture PRIVATE c_std_11)
target_compile_options(
	kobox_core_arena_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(
	kobox_core_arena_fixture PRIVATE kobox2::protocol Threads::Threads
)
target_link_options(
	kobox_core_arena_fixture
	PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/provider/core_lifecycle.exports"
		-Wl,-z,defs
)
set_property(
	TARGET kobox_core_arena_fixture
	APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/provider/core_lifecycle.exports"
)
set_target_properties(
	kobox_core_arena_fixture
	PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY "${KOBOX_CORE_ARENA_FIXTURE_OUTPUT_DIRECTORY}"
		PREFIX ""
		OUTPUT_NAME core_arena
)

function(kobox_add_core_arena_fixture_module target output)
	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${KOBOX_CORE_ARENA_FIXTURE_OUTPUT_DIRECTORY}"
		COMMAND
			"${CMAKE_C_COMPILER}"
			-std=gnu11
			-O2
			-Wall
			-Wextra
			-Werror
			-ffreestanding
			-fno-builtin
			-mcmodel=large
			-fno-pic
			-fno-pie
			-fno-stack-protector
			-fno-asynchronous-unwind-tables
			-fno-unwind-tables
			${ARGN}
			-I "${CMAKE_CURRENT_SOURCE_DIR}/../include"
			-I "${CMAKE_CURRENT_SOURCE_DIR}"
			-I "${PROJECT_SOURCE_DIR}/protocol/generated/include"
			-c "${CMAKE_CURRENT_SOURCE_DIR}/fixture/core_arena_consumer.c"
			-o "${output}"
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/fixture/core_arena_consumer.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/provider/core_lifecycle.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/runtime/module_context.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/../include/linux/compiler_attributes.h"
			"${PROJECT_SOURCE_DIR}/protocol/generated/include/kobox2/core_runtime.h"
			"${PROJECT_SOURCE_DIR}/protocol/generated/include/kobox2/core_runtime_layout.h"
		VERBATIM
	)
	add_custom_target(${target} ALL DEPENDS "${output}")
endfunction()

kobox_add_core_arena_fixture_module(
	kobox_core_arena_fixture_module
	"${KOBOX_CORE_ARENA_FIXTURE_MODULE}"
)
kobox_add_core_arena_fixture_module(
	kobox_core_arena_leak_fixture_module
	"${KOBOX_CORE_ARENA_LEAK_FIXTURE_MODULE}"
	"-DKOBOX_ARENA_FIXTURE_LEAK=1"
)

add_executable(
	kobox_core_closure_test
	provider/core_closure_test.c
	loader/closure_loader.c
	loader/elf64_loader.c
	runtime/resource_runtime.c
	runtime/memory_resource.c
)
target_compile_features(kobox_core_closure_test PRIVATE c_std_11)
target_compile_options(
	kobox_core_closure_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(
	kobox_core_closure_test PRIVATE kobox2::protocol ${CMAKE_DL_LIBS}
)
add_dependencies(
	kobox_core_closure_test
	kobox_core_arena_fixture
	kobox_core_arena_fixture_module
	kobox_core_arena_leak_fixture_module
)
add_test(
	NAME kobox2.core_closure
	COMMAND
		kobox_core_closure_test
		$<TARGET_FILE:kobox_core_arena_fixture>
		"${KOBOX_CORE_ARENA_FIXTURE_MODULE}"
		"${KOBOX_CORE_ARENA_LEAK_FIXTURE_MODULE}"
)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The fixture ELF loader currently requires x86_64")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "The fixture module currently requires Clang or GCC")
endif()

set(KOBOX_FIXTURE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/linux-sandbox-fixture")
set(KOBOX_FIXTURE_PROVIDER_MODULE
	"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_provider.ko"
)
set(KOBOX_FIXTURE_CONSUMER_MODULE
	"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_consumer.ko"
)
set(KOBOX_FIXTURE_FAIL_CONSUMER_MODULE
	"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_consumer_fail.ko"
)
set(KOBOX_FIXTURE_RAW_MODULE
	"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_raw.ko"
)
set(KOBOX_FIXTURE_RAW_FAIL_MODULE
	"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_raw_fail.ko"
)
set(KOBOX_FIXTURE_PROVIDER_MODULE
	"${KOBOX_FIXTURE_PROVIDER_MODULE}" PARENT_SCOPE
)
set(KOBOX_FIXTURE_CONSUMER_MODULE
	"${KOBOX_FIXTURE_CONSUMER_MODULE}" PARENT_SCOPE
)

add_library(
    kobox_fixture_core SHARED
    core/fixture_core.c
    host/test/host_test.c
)
target_include_directories(kobox_fixture_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kobox_fixture_core PRIVATE Threads::Threads)
target_link_options(
	kobox_fixture_core
	PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/core/fixture_core.exports"
)
set_property(
	TARGET kobox_fixture_core
	APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/core/fixture_core.exports"
)
target_compile_features(kobox_fixture_core PRIVATE c_std_11)
target_compile_options(kobox_fixture_core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(
    kobox_fixture_core
    PROPERTIES
        C_EXTENSIONS ON
        C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${KOBOX_FIXTURE_OUTPUT_DIRECTORY}"
        PREFIX ""
        OUTPUT_NAME fixture_core
)

function(kobox_add_fixture_module target output source)
	add_custom_command(
	OUTPUT "${output}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${KOBOX_FIXTURE_OUTPUT_DIRECTORY}"
	COMMAND
        "${CMAKE_C_COMPILER}"
        -std=gnu11
        -O2
        -Wall
        -Wextra
        -Werror
        -ffreestanding
        -fno-builtin
        -mcmodel=large
        -fno-pic
        -fno-pie
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -fno-unwind-tables
		-I "${CMAKE_CURRENT_SOURCE_DIR}/../include"
		-I "${CMAKE_CURRENT_SOURCE_DIR}/fixture"
		-I "${PROJECT_SOURCE_DIR}/protocol/generated/include"
		-c "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
		-o "${output}"
	DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/${source}"
		"${CMAKE_CURRENT_SOURCE_DIR}/fixture/fixture.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/runtime/module_context.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/../include/linux/compiler_attributes.h"
	VERBATIM
	)
	add_custom_target(${target} ALL DEPENDS "${output}")
endfunction()

kobox_add_fixture_module(
	kobox_fixture_provider
	"${KOBOX_FIXTURE_PROVIDER_MODULE}"
	fixture/provider.c
)
kobox_add_fixture_module(
	kobox_fixture_consumer
	"${KOBOX_FIXTURE_CONSUMER_MODULE}"
	fixture/consumer.c
)

add_custom_command(
	OUTPUT "${KOBOX_FIXTURE_FAIL_CONSUMER_MODULE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KOBOX_FIXTURE_OUTPUT_DIRECTORY}"
    COMMAND
        "${CMAKE_C_COMPILER}"
        -std=gnu11
        -O2
        -Wall
        -Wextra
        -Werror
        -ffreestanding
        -fno-builtin
        -mcmodel=large
        -fno-pic
        -fno-pie
        -fno-stack-protector
        -fno-asynchronous-unwind-tables
        -fno-unwind-tables
        -DKOBOX_FIXTURE_FAIL_INIT=1
		-I "${CMAKE_CURRENT_SOURCE_DIR}/../include"
		-I "${CMAKE_CURRENT_SOURCE_DIR}/fixture"
		-I "${PROJECT_SOURCE_DIR}/protocol/generated/include"
		-c "${CMAKE_CURRENT_SOURCE_DIR}/fixture/consumer.c"
		-o "${KOBOX_FIXTURE_FAIL_CONSUMER_MODULE}"
	DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/fixture/consumer.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/fixture/fixture.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/runtime/module_context.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/../include/linux/compiler_attributes.h"
    VERBATIM
)
add_custom_target(kobox_fixture_fail_consumer ALL
	DEPENDS "${KOBOX_FIXTURE_FAIL_CONSUMER_MODULE}")

function(kobox_add_raw_lifecycle_fixture target output status)
	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${KOBOX_FIXTURE_OUTPUT_DIRECTORY}"
		COMMAND
			"${CMAKE_C_COMPILER}"
			-std=gnu11
			-O2
			-Wall
			-Wextra
			-Werror
			-ffreestanding
			-fno-builtin
			-mcmodel=large
			-fno-pic
			-fno-pie
			-fno-stack-protector
			-fno-asynchronous-unwind-tables
			-fno-unwind-tables
			-DKOBOX_RAW_LIFECYCLE_STATUS=${status}
			-c "${CMAKE_CURRENT_SOURCE_DIR}/fixture/raw_lifecycle.c"
			-o "${output}"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/raw_lifecycle.c"
		VERBATIM
	)
	add_custom_target(${target} ALL DEPENDS "${output}")
endfunction()

kobox_add_raw_lifecycle_fixture(
	kobox_fixture_raw
	"${KOBOX_FIXTURE_RAW_MODULE}"
	0
)
kobox_add_raw_lifecycle_fixture(
	kobox_fixture_raw_fail
	"${KOBOX_FIXTURE_RAW_FAIL_MODULE}"
	-23
)

add_executable(
    kobox_fixture_sandbox
	fixture/runtime.c
	fixture/sandbox.c
	loader/closure_loader.c
	loader/elf64_loader.c
	runtime/resource_runtime.c
)
target_include_directories(kobox_fixture_sandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    kobox_fixture_sandbox
    PRIVATE kobox2_test_transport ${CMAKE_DL_LIBS}
)
target_compile_features(kobox_fixture_sandbox PRIVATE c_std_11)
target_compile_options(kobox_fixture_sandbox PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(kobox_fixture_sandbox kobox_fixture_core
	kobox_fixture_provider kobox_fixture_consumer)

add_executable(
    kobox_fixture_loader_test
    loader/elf64_loader.c
    loader/elf64_loader_test.c
)
target_compile_features(kobox_fixture_loader_test PRIVATE c_std_11)
target_compile_options(kobox_fixture_loader_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(kobox_fixture_loader_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(kobox_fixture_loader_test kobox_fixture_core
	kobox_fixture_provider)
add_test(
    NAME kobox2.fixture_loader
    COMMAND
		kobox_fixture_loader_test
		$<TARGET_FILE:kobox_fixture_core>
		"${KOBOX_FIXTURE_PROVIDER_MODULE}"
)

add_executable(
	kobox_link_plan_loader_test
	loader/link_plan_loader_test.c
)
target_compile_features(kobox_link_plan_loader_test PRIVATE c_std_11)
target_compile_options(
	kobox_link_plan_loader_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(
	kobox_link_plan_loader_test PRIVATE kobox_link_plan_loader
)
add_dependencies(
	kobox_link_plan_loader_test
	kobox_fixture_core
	kobox_fixture_provider
	kobox_fixture_consumer
	kobox_fixture_raw
	kobox_fixture_raw_fail
)
add_test(
	NAME kobox2.link_plan_loader
	COMMAND
		kobox_link_plan_loader_test
		$<TARGET_FILE:kobox_fixture_core>
		"${KOBOX_FIXTURE_PROVIDER_MODULE}"
		"${KOBOX_FIXTURE_CONSUMER_MODULE}"
		"${KOBOX_FIXTURE_RAW_MODULE}"
		"${KOBOX_FIXTURE_RAW_FAIL_MODULE}"
)

add_executable(
    kobox_closure_loader_test
    loader/closure_loader.c
	loader/closure_loader_test.c
	loader/elf64_loader.c
	runtime/resource_runtime.c
)
target_compile_features(kobox_closure_loader_test PRIVATE c_std_11)
target_compile_options(kobox_closure_loader_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(
    kobox_closure_loader_test PRIVATE kobox2::protocol ${CMAKE_DL_LIBS}
)
add_dependencies(
	kobox_closure_loader_test
	kobox_fixture_core
	kobox_fixture_provider
	kobox_fixture_consumer
	kobox_fixture_fail_consumer
)
add_test(
    NAME kobox2.closure_loader
    COMMAND
		kobox_closure_loader_test
		$<TARGET_FILE:kobox_fixture_core>
		"${KOBOX_FIXTURE_PROVIDER_MODULE}"
		"${KOBOX_FIXTURE_CONSUMER_MODULE}"
		"${KOBOX_FIXTURE_FAIL_CONSUMER_MODULE}"
)

if(KB2_ENABLE_SANITIZERS)
	target_compile_options(
		kobox_link_plan_loader
		PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
	)
	if(TARGET kobox_real_virgl_closure_link_test)
		target_compile_options(
			kobox_real_virgl_closure_link_test
			PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
		)
		target_link_options(
			kobox_real_virgl_closure_link_test
			PRIVATE -fsanitize=address,undefined
		)
	endif()
    foreach(
        target
        kobox_provider_arena_test
        kobox_provider_lifecycle_test
        kobox_linux_memory_resource_test
        kobox_linux_core_lifecycle_test
		kobox_core_closure_test
		kobox_link_plan_loader_test
    )
        target_compile_options(
            ${target} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
    target_compile_options(
        kobox_fixture_core
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_compile_options(
        kobox_fixture_sandbox
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(kobox_fixture_sandbox PRIVATE -fsanitize=address,undefined)
    target_compile_options(
        kobox_fixture_loader_test
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(
        kobox_fixture_loader_test PRIVATE -fsanitize=address,undefined
    )
    target_compile_options(
        kobox_closure_loader_test
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(
        kobox_closure_loader_test PRIVATE -fsanitize=address,undefined
    )
else()
    target_link_options(kobox_fixture_core PRIVATE -Wl,-z,defs)
endif()
