# SPDX-License-Identifier: GPL-2.0-only

add_test(NAME kobox2.build_boundaries
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/build_boundaries_test.py"
		--source "${PROJECT_SOURCE_DIR}" --artifacts "${CMAKE_CURRENT_BINARY_DIR}"
		--cmake "${CMAKE_COMMAND}" --cc "${CMAKE_C_COMPILER}")
set_tests_properties(kobox2.build_boundaries PROPERTIES TIMEOUT 90)

add_executable(kobox_posix_device_proxy_test host/posix/device_proxy_test.c)
target_compile_options(kobox_posix_device_proxy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_device_proxy_test PRIVATE kobox_posix_device_proxy)
target_link_options(kobox_posix_device_proxy_test PRIVATE -Wl,--wrap=calloc,--wrap=free)
add_test(NAME kobox2.native_device_proxy COMMAND kobox_posix_device_proxy_test)
set_tests_properties(kobox2.native_device_proxy PROPERTIES TIMEOUT 10)

add_executable(kobox_posix_bootstrap_test host/posix/bootstrap_test.c)
target_compile_options(kobox_posix_bootstrap_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_bootstrap_test PRIVATE kobox_posix_bootstrap)
set(KOBOX_PRODUCTION_BOOT_RUNTIME_CORE "" CACHE FILEPATH "Core built without --with-gates")
if(KOBOX_PRODUCTION_BOOT_RUNTIME_CORE)
	add_test(NAME kobox2.production_bootstrap
		COMMAND kobox_posix_bootstrap_test "${KOBOX_PRODUCTION_BOOT_RUNTIME_CORE}")
	set_tests_properties(kobox2.production_bootstrap PROPERTIES TIMEOUT 30)
endif()

# This tests the kernel-side machine ABI, with implicit vector use disabled.
add_executable(kobox_host_call_test host/call_test.c)
target_compile_definitions(kobox_host_call_test PRIVATE KOBOX_BOOT_RUNTIME=1)
target_compile_options(kobox_host_call_test PRIVATE
	-Wall -Wextra -Werror -O2 -mgeneral-regs-only -fno-sanitize=all)
target_link_options(kobox_host_call_test PRIVATE -fno-sanitize=all)
add_test(NAME kobox2.host_call_fp_bank COMMAND kobox_host_call_test)

add_library(kobox_posix_qtest STATIC tests/linux/qemu/qtest.c)
target_compile_options(kobox_posix_qtest PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qtest PUBLIC Threads::Threads)
add_executable(kobox_posix_qtest_test tests/linux/qemu/qtest_test.c)
target_compile_options(kobox_posix_qtest_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qtest_test PRIVATE kobox_posix_qtest)
add_library(kobox_posix_qemu_pci STATIC tests/linux/qemu/qemu_pci.c)
target_compile_options(kobox_posix_qemu_pci PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qemu_pci PUBLIC kobox_posix_qtest)
add_executable(kobox_posix_qemu_pci_test tests/linux/qemu/qemu_pci_test.c)
target_compile_options(kobox_posix_qemu_pci_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qemu_pci_test PRIVATE kobox_posix_qemu_pci)
add_executable(kobox_posix_qemu_dma_test tests/linux/qemu/qemu_dma_test.c)
target_compile_options(kobox_posix_qemu_dma_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qemu_dma_test PRIVATE kobox_posix_qemu_pci)
add_executable(kobox_posix_qemu_revoke_test tests/linux/qemu/qemu_revoke_test.c)
target_compile_options(kobox_posix_qemu_revoke_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qemu_revoke_test PRIVATE kobox_posix_qemu_pci)
target_link_options(kobox_posix_qemu_revoke_test PRIVATE -Wl,--wrap=kobox_qtest_command)
find_program(KOBOX_QEMU_SYSTEM_X86_64 qemu-system-x86_64)
if(KOBOX_QEMU_SYSTEM_X86_64)
	add_test(NAME kobox2.qemu_host_revoke
		COMMAND kobox_posix_qemu_revoke_test "${KOBOX_QEMU_SYSTEM_X86_64}")
	set_tests_properties(kobox2.qemu_host_revoke PROPERTIES TIMEOUT 20)
	add_test(NAME kobox2.qemu_iommu_dma
		COMMAND kobox_posix_qemu_dma_test "${KOBOX_QEMU_SYSTEM_X86_64}")
	set_tests_properties(kobox2.qemu_iommu_dma PROPERTIES TIMEOUT 20)
	add_test(NAME kobox2.qemu_pci_ram_backing
		COMMAND kobox_posix_qemu_pci_test "${KOBOX_QEMU_SYSTEM_X86_64}")
	set_tests_properties(kobox2.qemu_pci_ram_backing PROPERTIES TIMEOUT 15)
	add_test(NAME kobox2.qemu_device_transport
		COMMAND kobox_posix_qtest_test "${KOBOX_QEMU_SYSTEM_X86_64}")
	set_tests_properties(kobox2.qemu_device_transport PROPERTIES TIMEOUT 15)
endif()

add_executable(kobox_boot_package_test boot/package_test.c)
target_compile_options(kobox_boot_package_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_boot_package_test PRIVATE kobox_posix_package)
target_link_options(kobox_boot_package_test PRIVATE
	-Wl,--wrap=malloc,--wrap=calloc,--wrap=mmap,--wrap=munmap)
add_test(NAME kobox2.boot_package_validation COMMAND kobox_boot_package_test)
add_executable(kobox_pci_fixture_test boot/pci_fixture.c boot/pci_fixture_test.c
	boot/pci_enum_fixture.c boot/pci_config_fixture.c)
target_compile_options(kobox_pci_fixture_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_pci_fixture_test PRIVATE kobox2::protocol)
add_test(NAME kobox2.pci_resource_fixture COMMAND kobox_pci_fixture_test)

add_executable(kobox_pci_resource_test boot/pci_resource_test.c)
target_compile_options(kobox_pci_resource_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_pci_resource_test PRIVATE kobox_boot_pci_resource)
add_test(NAME kobox2.pci_resource_host_port COMMAND kobox_pci_resource_test)

add_executable(kobox_iommu_resource_test boot/iommu_resource_test.c)
target_compile_options(kobox_iommu_resource_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_iommu_resource_test PRIVATE kobox_boot_iommu_resource)
add_test(NAME kobox2.iommu_resource_host_port COMMAND kobox_iommu_resource_test)

add_executable(kobox_resource_runtime_test runtime/resource_runtime_test.c)
target_compile_options(kobox_resource_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(kobox_resource_runtime_test PRIVATE -Wl,--wrap=calloc)
target_link_libraries(kobox_resource_runtime_test PRIVATE kobox_boot_resources kobox_posix_resources)
add_test(NAME kobox2.resource_import_rollback COMMAND kobox_resource_runtime_test)

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
	fixture/linux_process_lifecycle.c
	fixture/linux_process_lifecycle_test.c
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

# Common upcall bookkeeping has no native TLS/runtime ABI, including compiler
# injected stack-guard hooks. Keep native backend stack protection unchanged.

add_executable(kobox_machine_domain_test machine/domain_test.c)
target_link_libraries(kobox_machine_domain_test PRIVATE kobox_machine)
target_compile_options(kobox_machine_domain_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME kobox2.machine_domain COMMAND kobox_machine_domain_test)

add_executable(kobox_posix_core_test host/posix/core_test.c)
target_compile_options(kobox_posix_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_core_test PRIVATE kobox_posix_core Threads::Threads)

add_executable(kobox_device_codec_test runtime/device_codec_test.c)
target_compile_options(kobox_device_codec_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_device_codec_test PRIVATE kobox_device_codec)
add_test(NAME kobox2.device_codec COMMAND kobox_device_codec_test)
# Common metadata has no native TLS/stack-guard ABI. Native entry points and
# backends retain their toolchain stack protection, as does the host process.

add_test(NAME kobox2.common_bootstrap_surface
	COMMAND "${Python3_EXECUTABLE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/runtime/verify_boundaries.py"
		--nm "${CMAKE_NM}"
		--archive $<TARGET_FILE:kobox_boot_core>
		--archive $<TARGET_FILE:kobox_boot_package>
		--archive $<TARGET_FILE:kobox_device_codec>
		--archive $<TARGET_FILE:kobox_resource_registry>
		--archive $<TARGET_FILE:kobox_arch_elf>)
add_library(kobox_posix_qemu_remote STATIC tests/linux/qemu/qemu_remote.c)
target_compile_options(kobox_posix_qemu_remote PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_qemu_remote PUBLIC kobox_posix_qemu_pci kobox_posix_device_proxy)
add_executable(kobox_device_channel_test host/posix/device_channel_test.c)
target_compile_options(kobox_device_channel_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_device_channel_test PRIVATE kobox_posix_qemu_remote)
add_test(NAME kobox2.device_channel_wire COMMAND kobox_device_channel_test)

add_executable(kobox_posix_vm_client host/posix/vm_client.c
	tests/clients/vm_program.c arch/x86_64/vm_program.S arch/x86_64/linux_vm_syscall.S)
target_link_libraries(kobox_posix_vm_client PRIVATE kobox_arch_linux_x86)
target_compile_features(kobox_posix_vm_client PRIVATE c_std_11)
# After attachment this fixture executes with guest-controlled FS/GS. Compiler
# canaries must not interpret guest FS as the bootstrap libc's TLS, including
# in stop_event and the native fault handler. Other host targets retain SSP.
target_compile_options(kobox_posix_vm_client PRIVATE
	-Wall -Wextra -Wpedantic -Werror -fno-stack-protector)
add_executable(kobox_posix_vm_bootstrap_test
	host/posix/vm_bootstrap_test.c host/posix/vm_bootstrap_test.S)
target_compile_features(kobox_posix_vm_bootstrap_test PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_bootstrap_test PRIVATE
	-Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm_bootstrap_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_vm_bootstrap
	COMMAND kobox_posix_vm_bootstrap_test $<TARGET_FILE:kobox_posix_vm_bootstrap>)
set_tests_properties(kobox2.posix_vm_bootstrap PROPERTIES TIMEOUT 30)
add_executable(kobox_elf_client_test host/posix/elf_client_test.c host/posix/elf_client_test.S)
add_executable(kobox_elf_signal_client_test host/posix/elf_client_test.c host/posix/elf_client_test.S
	host/posix/signal_client_test.c host/posix/signal_client_test.S)
target_compile_definitions(kobox_elf_signal_client_test PRIVATE KOBOX_ELF_SIGNALS=1)
add_executable(kobox_elf_vfork_client_test host/posix/elf_client_test.c host/posix/elf_client_test.S
	host/posix/vfork_client_test.c host/posix/vfork_client_test.S)
target_compile_definitions(kobox_elf_vfork_client_test PRIVATE KOBOX_ELF_VFORK=1)
add_executable(kobox_drm_client_test host/posix/drm_client_test.c host/posix/drm_client_test.S)
set(KOBOX_MESA_SOURCE_DIR "" CACHE PATH "Pristine Mesa source for the real GL client")
set(KOBOX_MESA_BUILD_DIR "" CACHE PATH "Built virgl-only Mesa")
set(KOBOX_MESA_SYSROOT "" CACHE PATH "Mesa build dependency sysroot")
set(KOBOX_VIRGL_QEMU "" CACHE FILEPATH "QEMU with virglrenderer and SDL OpenGL")
set(KOBOX_VIRGL_HOST_DRIVER "" CACHE STRING "Optional host Mesa driver for QEMU")
if(KOBOX_MESA_BUILD_DIR)
	if(NOT KOBOX_MESA_SOURCE_DIR OR NOT KOBOX_MESA_SYSROOT)
		message(FATAL_ERROR "Real Mesa client requires source, build and dependency paths")
	endif()
	add_test(NAME kobox2.linux_display_verifier
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/display_verifier_test.py")
	add_test(NAME kobox2.linux_multi_verifier
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/multi_verifier_test.py")
	# These clients exercise the sandbox Linux ABI on every native host OS.
	add_executable(kobox_mesa_client_test tests/clients/mesa_client_test.c)
	add_executable(kobox_mesa_draw_client_test tests/clients/mesa_client_test.c
		tests/clients/mesa_draw_test.c)
	target_compile_definitions(kobox_mesa_draw_client_test PRIVATE KOBOX_MESA_DRAW=1)
	add_executable(kobox_mesa_draw_oracle_client_test tests/clients/mesa_client_test.c
		tests/clients/mesa_draw_test.c)
	target_compile_definitions(kobox_mesa_draw_oracle_client_test PRIVATE
		KOBOX_MESA_DRAW=1 KOBOX_MESA_DRAW_BAD_SHADER=1)
	add_executable(kobox_mesa_kms_client_test tests/clients/mesa_client_test.c
		tests/clients/mesa_kms_test.c)
	target_compile_definitions(kobox_mesa_kms_client_test PRIVATE KOBOX_MESA_KMS=1)
	add_executable(kobox_mesa_kms_oracle_client_test tests/clients/mesa_client_test.c
		tests/clients/mesa_kms_test.c)
	target_compile_definitions(kobox_mesa_kms_oracle_client_test PRIVATE
		KOBOX_MESA_KMS=1 KOBOX_MESA_KMS_BAD_SHADER=1)
	add_executable(kobox_mesa_multi_client_test tests/clients/mesa_multi_client_test.c
		tests/clients/mesa_kms_test.c)
	add_executable(kobox_mesa_multi_oracle_client_test tests/clients/mesa_multi_client_test.c
		tests/clients/mesa_kms_test.c)
	target_compile_definitions(kobox_mesa_multi_oracle_client_test PRIVATE KOBOX_MESA_MULTI_BAD_SHADER=1)
	add_executable(kobox_display_oracle_test tests/linux/display_oracle_test.c)
	target_compile_options(kobox_display_oracle_test PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
	target_include_directories(kobox_display_oracle_test PRIVATE "${KOBOX_MESA_SYSROOT}/usr/include")
	target_link_libraries(kobox_display_oracle_test PRIVATE
		"${KOBOX_MESA_SYSROOT}/usr/lib/x86_64-linux-gnu/libX11.so")
	foreach(_mesa_client kobox_mesa_client_test kobox_mesa_draw_client_test kobox_mesa_draw_oracle_client_test kobox_mesa_kms_client_test kobox_mesa_kms_oracle_client_test kobox_mesa_multi_client_test kobox_mesa_multi_oracle_client_test)
	target_compile_features(${_mesa_client} PRIVATE c_std_11)
	target_compile_options(${_mesa_client} PRIVATE
		-Wall -Wextra -Wpedantic -Werror -O2)
	target_include_directories(${_mesa_client} PRIVATE
		"${KOBOX_MESA_SOURCE_DIR}/include" "${KOBOX_MESA_SOURCE_DIR}/src/gbm/main"
		"${KOBOX_MESA_SYSROOT}/usr/include" "${KOBOX_MESA_SYSROOT}/usr/include/libdrm")
	target_link_libraries(${_mesa_client} PRIVATE
		"${KOBOX_MESA_BUILD_DIR}/src/egl/libEGL.so"
		"${KOBOX_MESA_BUILD_DIR}/src/gbm/libgbm.so"
		"${KOBOX_MESA_SYSROOT}/usr/lib/x86_64-linux-gnu/libdrm.so")
	endforeach()
endif()
add_executable(kobox_prime_client_test host/posix/prime_client_test.c host/posix/drm_client_test.S)
add_executable(kobox_prime_consumer_client_test host/posix/prime_client_test.c host/posix/drm_client_test.S)
target_compile_definitions(kobox_prime_consumer_client_test PRIVATE KOBOX_PRIME_CONSUMER=1)
add_executable(kobox_sync_client_test host/posix/drm_client_test.c
	host/posix/drm_client_test.S host/posix/sync_client_test.c host/posix/sync_signal_test.c)
target_compile_definitions(kobox_sync_client_test PRIVATE KOBOX_DRM_SYNC=1)
add_executable(kobox_sync_remove_client_test host/posix/drm_client_test.c
	host/posix/drm_client_test.S host/posix/sync_client_test.c host/posix/sync_signal_test.c)
target_compile_definitions(kobox_sync_remove_client_test PRIVATE
	KOBOX_DRM_SYNC=1 KOBOX_DRM_SYNC_REMOVE=1)
add_executable(kobox_sync_revoke_client_test host/posix/drm_client_test.c
	host/posix/drm_client_test.S host/posix/sync_client_test.c host/posix/sync_signal_test.c)
target_compile_definitions(kobox_sync_revoke_client_test PRIVATE
	KOBOX_DRM_SYNC=1 KOBOX_DRM_SYNC_REMOVE=1 KOBOX_DRM_SYNC_REVOKE=1)
# Export the pinned DRM UAPI with Linux's own header preparation, rather than
# copying structures or compiling kernel-only annotations into the client.
set(_drm_uapi_root "${CMAKE_CURRENT_BINARY_DIR}/drm-uapi")
add_executable(kobox_uapi_unifdef ../scripts/unifdef.c)
set_target_properties(kobox_uapi_unifdef PROPERTIES OUTPUT_NAME unifdef
	RUNTIME_OUTPUT_DIRECTORY "${_drm_uapi_root}/scripts")
set(_drm_uapi_headers)
foreach(_header drm/drm.h drm/drm_mode.h drm/virtgpu_drm.h linux/virtio_gpu.h
	linux/dma-buf.h linux/sync_file.h asm/signal.h)
	set(_source "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi/${_header}")
	if(_header MATCHES "^asm/")
		set(_source "${CMAKE_CURRENT_SOURCE_DIR}/../arch/x86/include/uapi/${_header}")
	endif()
	set(_output "${_drm_uapi_root}/include/${_header}")
	get_filename_component(_directory "${_output}" DIRECTORY)
	add_custom_command(OUTPUT "${_output}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_directory}"
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/headers_install.sh"
			"${_source}" "${_output}"
		WORKING_DIRECTORY "${_drm_uapi_root}"
		DEPENDS kobox_uapi_unifdef ../scripts/headers_install.sh
			"${_source}" VERBATIM)
	list(APPEND _drm_uapi_headers "${_output}")
endforeach()
add_custom_target(kobox_drm_uapi DEPENDS ${_drm_uapi_headers})
add_dependencies(kobox_drm_client_test kobox_drm_uapi)
add_dependencies(kobox_prime_client_test kobox_drm_uapi)
add_dependencies(kobox_prime_consumer_client_test kobox_drm_uapi)
add_dependencies(kobox_sync_client_test kobox_drm_uapi)
add_dependencies(kobox_sync_remove_client_test kobox_drm_uapi)
add_dependencies(kobox_sync_revoke_client_test kobox_drm_uapi)
target_include_directories(kobox_drm_client_test PRIVATE "${_drm_uapi_root}/include")
target_include_directories(kobox_prime_client_test PRIVATE "${_drm_uapi_root}/include")
target_include_directories(kobox_prime_consumer_client_test PRIVATE "${_drm_uapi_root}/include")
target_include_directories(kobox_sync_client_test PRIVATE "${_drm_uapi_root}/include")
target_include_directories(kobox_sync_remove_client_test PRIVATE "${_drm_uapi_root}/include")
target_include_directories(kobox_sync_revoke_client_test PRIVATE "${_drm_uapi_root}/include")
foreach(_elf_client kobox_elf_client_test kobox_elf_signal_client_test kobox_elf_vfork_client_test kobox_drm_client_test kobox_sync_client_test kobox_sync_remove_client_test kobox_sync_revoke_client_test kobox_prime_client_test kobox_prime_consumer_client_test)
target_compile_features(${_elf_client} PRIVATE c_std_11)
target_compile_options(${_elf_client} PRIVATE
	-Wall -Wextra -Wpedantic -Werror -O2 -ffreestanding -fno-builtin
	-fno-stack-protector -fno-pie -fno-sanitize=all)
target_link_options(${_elf_client} PRIVATE
	-nostdlib -static -fno-sanitize=all LINKER:--build-id=none)
endforeach()
add_executable(kobox_posix_vm_test host/posix/vm_test.c)
target_compile_features(kobox_posix_vm_test PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_vm_transport
	COMMAND kobox_posix_vm_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_vm_transport PROPERTIES TIMEOUT 30)
add_executable(kobox_posix_syscall_test host/posix/syscall_test.c)
target_compile_features(kobox_posix_syscall_test PRIVATE c_std_11)
target_compile_options(kobox_posix_syscall_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_syscall_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_syscall_transport
	COMMAND kobox_posix_syscall_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_syscall_transport PROPERTIES TIMEOUT 30)
add_executable(kobox_posix_fork_test host/posix/fork_test.c)
target_compile_features(kobox_posix_fork_test PRIVATE c_std_11)
target_compile_options(kobox_posix_fork_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_fork_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_fork_transport
	COMMAND kobox_posix_fork_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_fork_transport PROPERTIES TIMEOUT 30)
add_executable(kobox_posix_vm_service_test host/posix/vm_service_test.c)
target_compile_features(kobox_posix_vm_service_test PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_service_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_vm_service_test PRIVATE kobox_posix_vm)
add_test(NAME kobox2.posix_vm_service
	COMMAND kobox_posix_vm_service_test $<TARGET_FILE:kobox_posix_vm_client>)
set_tests_properties(kobox2.posix_vm_service PROPERTIES TIMEOUT 30)

add_executable(kobox_posix_host_test host/posix/host_test.c)
target_compile_features(kobox_posix_host_test PRIVATE c_std_11)
set_target_properties(kobox_posix_host_test PROPERTIES C_EXTENSIONS OFF)
target_compile_options(
	kobox_posix_host_test PRIVATE -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(kobox_posix_host_test PRIVATE kobox_posix_host)
add_test(NAME kobox2.posix_host_gate COMMAND kobox_posix_host_test)
add_executable(kobox_posix_memory_revoke_test host/posix/memory_revoke_test.c)
target_compile_options(kobox_posix_memory_revoke_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_memory_revoke_test PRIVATE kobox_posix_host)
add_test(NAME kobox2.posix_memory_revoke COMMAND kobox_posix_memory_revoke_test)
set_tests_properties(kobox2.posix_memory_revoke PROPERTIES TIMEOUT 15)
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
		--machine-archive $<TARGET_FILE:kobox_machine>
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
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/gates/task_smp.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/gates/task_port.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/task.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/task.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/host_call.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_port.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_gate.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/time_port.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/host.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/boot.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/current.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/fsgsbase.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/gsseg.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/task/include/asm/segment.h"
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

	add_library(kobox_linux_boot_gate STATIC
		boot/boot_test.c)
	target_compile_features(kobox_linux_boot_gate PRIVATE c_std_11)
	target_compile_options(kobox_linux_boot_gate
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_boot_gate
		PUBLIC kobox_posix_bootstrap kobox_posix_mm kobox_posix_vm)
	add_executable(kobox_linux_boot_test boot/boot_test_main.c)
	target_link_libraries(kobox_linux_boot_test PRIVATE kobox_linux_boot_gate)
	add_executable(kobox_linux_pci_test
		boot/pci_test.c boot/pci_config_fixture.c)
	target_compile_options(kobox_linux_pci_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_pci_test PRIVATE kobox_linux_boot_gate)
	add_executable(kobox_linux_pci_hardware_test boot/pci_hardware_test.c)
	target_compile_options(kobox_linux_pci_hardware_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_pci_hardware_test PRIVATE kobox_linux_boot_gate kobox_posix_qemu_pci)
	add_executable(kobox_linux_virtio_test boot/virtio_test.c)
	target_compile_options(kobox_linux_virtio_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_virtio_test PRIVATE kobox_linux_boot_gate kobox_posix_qemu_remote)
	target_link_options(kobox_linux_virtio_test PRIVATE -Wl,--wrap=kobox_qtest_command)
	add_executable(kobox_linux_irq_routing_test
		boot/irq_routing_test.c boot/pci_config_fixture.c)
	target_compile_options(kobox_linux_irq_routing_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_irq_routing_test PRIVATE kobox_linux_boot_gate)
	add_library(kobox_boot_dma_fixture STATIC boot/dma_fixture.c boot/pci_config_fixture.c)
	target_compile_options(kobox_boot_dma_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_boot_dma_fixture PUBLIC kobox_linux_boot_gate
		kobox_boot_iommu_resource kobox_boot_resources kobox_posix_resources)
	target_link_libraries(kobox_linux_irq_routing_test PRIVATE kobox_boot_dma_fixture)
	add_executable(kobox_linux_dma_test boot/dma_test.c)
	target_compile_options(kobox_linux_dma_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_dma_test PRIVATE kobox_boot_dma_fixture)
	add_executable(kobox_boot_package_process_test boot/package_process_test.c
		boot/pci_fixture.c boot/lifecycle_fixture.c)
	target_compile_options(kobox_boot_package_process_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_boot_package_process_test PRIVATE kobox_posix_package kobox_posix_resources
		kobox_linux_boot_gate kobox_boot_resources kobox2_test_transport)
	add_executable(kobox_native_sandbox_test boot/native_sandbox_test.c
		boot/pci_fixture.c boot/pci_enum_fixture.c boot/pci_config_fixture.c boot/lifecycle_fixture.c)
	target_compile_options(kobox_native_sandbox_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_native_sandbox_test PRIVATE kobox_posix_package
		kobox_linux_boot_gate kobox_boot_resources kobox_boot_pci_resource kobox2_test_transport
		kobox_boot_dma_fixture)
	if(KOBOX_LINUX_BOOT_RUNTIME_CORE)
		if(KOBOX_QEMU_SYSTEM_X86_64)
			add_test(NAME kobox2.linux_qemu_pci_transport
				COMMAND kobox_linux_pci_hardware_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					"${KOBOX_QEMU_SYSTEM_X86_64}")
			set_tests_properties(kobox2.linux_qemu_pci_transport PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 30
				LABELS "linux-runtime;integration;pci")
		endif()
		add_test(NAME kobox2.linux_irq_routing_gate
			COMMAND kobox_linux_irq_routing_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_irq_routing_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 30
			LABELS "linux-runtime;integration;irq")
		add_test(NAME kobox2.linux_dma_mapping_gate
			COMMAND kobox_linux_dma_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_dma_mapping_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 30
			LABELS "linux-runtime;integration;dma")
		add_test(NAME kobox2.linux_pci_enumeration_gate
			COMMAND kobox_linux_pci_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_pci_enumeration_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 30)
		add_test(NAME kobox2.linux_mmio_transaction_gate
			COMMAND kobox_linux_pci_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --transactions)
		set_tests_properties(kobox2.linux_mmio_transaction_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;pci" TIMEOUT 30)
		add_executable(kobox_linux_resource_port_test boot/resource_port_test.c)
		target_compile_options(kobox_linux_resource_port_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
		target_link_libraries(kobox_linux_resource_port_test PRIVATE ${CMAKE_DL_LIBS})
		add_test(NAME kobox2.linux_resource_port_boundary
			COMMAND kobox_linux_resource_port_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		get_filename_component(_kobox_boot_runtime_dir
			"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" DIRECTORY)
		add_test(NAME kobox2.linux_boot_runtime_load
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/core_load_test.py"
				--core "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
				--inputs "${_kobox_boot_runtime_dir}/linux-boot-inputs.json")
		set_tests_properties(kobox2.linux_boot_runtime_load PROPERTIES
			FIXTURES_SETUP kobox_full_boot_core TIMEOUT 30)
		add_test(NAME kobox2.linux_core_tls
			COMMAND kobox_posix_core_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_core_tls PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 30)
		add_test(NAME kobox2.linux_boot_service_gate
			COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}")
		set_tests_properties(kobox2.linux_boot_service_gate PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;go-no-go" TIMEOUT 30)
		if(KOBOX_LINUX_GEM_MODULE_DIR)
			if(TARGET kobox_mesa_client_test AND KOBOX_VIRGL_QEMU)
				set(_host_gl_driver)
				if(KOBOX_VIRGL_HOST_DRIVER)
					set(_host_gl_driver --host-driver "${KOBOX_VIRGL_HOST_DRIVER}")
				endif()
				foreach(_mesa_gate context draw draw_oracle kms kms_oracle multi multi_oracle recovery)
				set(_mesa_client kobox_mesa_client_test)
				set(_mesa_option)
				if(_mesa_gate MATCHES "^draw")
					set(_mesa_client kobox_mesa_${_mesa_gate}_client_test)
					set(_mesa_option --draw)
				endif()
				if(_mesa_gate STREQUAL "draw_oracle")
					list(APPEND _mesa_option --expect-pixel-mismatch)
				endif()
				if(_mesa_gate MATCHES "^kms")
					set(_mesa_client kobox_mesa_${_mesa_gate}_client_test)
					set(_mesa_option --kms --display-observer $<TARGET_FILE:kobox_display_oracle_test>)
				endif()
				if(_mesa_gate STREQUAL "kms_oracle")
					list(APPEND _mesa_option --expect-display-mismatch)
				endif()
				if(_mesa_gate MATCHES "^multi" OR _mesa_gate STREQUAL "recovery")
					set(_mesa_client kobox_mesa_multi_client_test)
					set(_mesa_option --multi --kms --display-observer $<TARGET_FILE:kobox_display_oracle_test>)
				endif()
				if(_mesa_gate STREQUAL "multi_oracle")
					set(_mesa_client kobox_mesa_multi_oracle_client_test)
					list(APPEND _mesa_option --expect-shared-mismatch)
				endif()
				if(_mesa_gate STREQUAL "recovery")
					list(APPEND _mesa_option --recovery)
				endif()
				add_test(NAME kobox2.linux_mesa_virgl_${_mesa_gate}
					COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/run_mesa_test.py"
						--test $<TARGET_FILE:kobox_linux_virtio_test>
						--core "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						--qemu "${KOBOX_VIRGL_QEMU}" --modules "${KOBOX_LINUX_GEM_MODULE_DIR}"
						--bootstrap $<TARGET_FILE:kobox_posix_vm_bootstrap>
						--client $<TARGET_FILE:${_mesa_client}>
						--mesa-build "${KOBOX_MESA_BUILD_DIR}" --sysroot "${KOBOX_MESA_SYSROOT}"
						${_host_gl_driver} ${_mesa_option})
				set_tests_properties(kobox2.linux_mesa_virgl_${_mesa_gate} PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 120
					RESOURCE_LOCK kobox_mesa_display
					LABELS "linux-runtime;integration;virgl;real-mesa")
				endforeach()
			endif()
			if(KOBOX_QEMU_SYSTEM_X86_64 AND
			   EXISTS "${KOBOX_LINUX_GEM_MODULE_DIR}/drivers/gpu/drm/virtio/virtio-gpu.ko")
				add_test(NAME kobox2.linux_native_virtio_bind
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}")
				add_test(NAME kobox2.linux_remote_virtio_bind
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}" --remote)
				set_tests_properties(kobox2.linux_remote_virtio_bind PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;remote-owner")
				foreach(_client IN ITEMS drm sync sync_remove prime prime_consumer)
					set(_option --delayed-irq)
					if(_client STREQUAL "drm")
						set(_option)
					elseif(_client STREQUAL "sync_remove")
						set(_option --remove-irq)
					elseif(_client STREQUAL "prime_consumer")
						set(_option --prime)
					endif()
					add_test(NAME kobox2.linux_remote_${_client}
						COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
							"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
							$<TARGET_FILE:kobox_posix_vm_bootstrap>
							$<TARGET_FILE:kobox_${_client}_client_test> ${_option} --remote)
					set_tests_properties(kobox2.linux_remote_${_client} PROPERTIES
						FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 90
						LABELS "linux-runtime;integration;virtio;remote-owner")
				endforeach()
				foreach(_revoke IN ITEMS wait fault dma dma-failure)
					set(_revoke_client kobox_sync_remove_client_test)
					set(_revoke_option --remove-irq)
					if(_revoke STREQUAL "fault")
						set(_revoke_client kobox_sync_revoke_client_test)
					elseif(_revoke MATCHES "^dma")
						set(_revoke_client kobox_prime_consumer_client_test)
						set(_revoke_option --prime)
					endif()
					add_test(NAME kobox2.linux_generation_revoke_${_revoke}
						COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
							"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
							$<TARGET_FILE:kobox_posix_vm_bootstrap>
							$<TARGET_FILE:${_revoke_client}> ${_revoke_option} --revoke-${_revoke})
					set_tests_properties(kobox2.linux_generation_revoke_${_revoke} PROPERTIES
						FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 120
						LABELS "linux-runtime;integration;virtio;remote-owner;generation;revoke")
				endforeach()
				add_test(NAME kobox2.linux_native_virtio_drm_client
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
						$<TARGET_FILE:kobox_posix_vm_bootstrap> $<TARGET_FILE:kobox_drm_client_test>)
				add_test(NAME kobox2.linux_native_sync_file_timeline
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
						$<TARGET_FILE:kobox_posix_vm_bootstrap> $<TARGET_FILE:kobox_sync_client_test>
						--delayed-irq)
				add_test(NAME kobox2.linux_native_prime_client
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
						$<TARGET_FILE:kobox_posix_vm_bootstrap> $<TARGET_FILE:kobox_prime_client_test>
						--delayed-irq)
				set_tests_properties(kobox2.linux_native_prime_client PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;prime")
				add_test(NAME kobox2.linux_native_prime_dma_consumer
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
						$<TARGET_FILE:kobox_posix_vm_bootstrap> $<TARGET_FILE:kobox_prime_consumer_client_test>
						--prime)
				set_tests_properties(kobox2.linux_native_prime_dma_consumer PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;prime;dma")
				set_tests_properties(kobox2.linux_native_sync_file_timeline PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;sync")
				add_test(NAME kobox2.linux_native_sync_device_remove
					COMMAND kobox_linux_virtio_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
						"${KOBOX_QEMU_SYSTEM_X86_64}" "${KOBOX_LINUX_GEM_MODULE_DIR}"
						$<TARGET_FILE:kobox_posix_vm_bootstrap> $<TARGET_FILE:kobox_sync_remove_client_test>
						--remove-irq)
				set_tests_properties(kobox2.linux_native_sync_device_remove PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;sync")
				set_tests_properties(kobox2.linux_native_virtio_bind
					kobox2.linux_native_virtio_drm_client PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;virtio;drm")
			endif()
			set(_kobox_gem_modules)
			foreach(_module IN ITEMS drivers/i2c/i2c-core.ko
					drivers/gpu/drm/drm_panel_orientation_quirks.ko
					drivers/gpu/drm/drm.ko drivers/gpu/drm/drm_shmem_helper.ko)
				if(NOT EXISTS "${KOBOX_LINUX_GEM_MODULE_DIR}/${_module}")
					message(FATAL_ERROR "Missing native module: ${_module}")
				endif()
				list(APPEND _kobox_gem_modules "${KOBOX_LINUX_GEM_MODULE_DIR}/${_module}")
			endforeach()
			foreach(_package_case IN ITEMS normal stale bad-grant module-failure)
				add_test(NAME kobox2.linux_boot_artifact_transfer_${_package_case}
					COMMAND kobox_boot_package_process_test $<TARGET_FILE:kobox_linux_boot_test>
						"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" ${_kobox_gem_modules} ${_package_case})
				set_tests_properties(kobox2.linux_boot_artifact_transfer_${_package_case} PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;boot-artifact-transfer")
			endforeach()
			if(NOT EXISTS "${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/resource_test.ko")
				message(FATAL_ERROR "Missing native resource-port test module; rebuild native modules")
			endif()
			foreach(_resource_case IN ITEMS resource resource-bad-fd resource-rights
					resource-stale resource-module-failure resource-init-failure
					resource-stop resource-stale-stop resource-disconnect
					resource-early-stop resource-stop-module-failure resource-stop-init-failure)
				add_test(NAME kobox2.linux_native_${_resource_case}
					COMMAND kobox_boot_package_process_test $<TARGET_FILE:kobox_linux_boot_test>
						"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" ${_kobox_gem_modules}
						"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/resource_test.ko" ${_resource_case})
				set_tests_properties(kobox2.linux_native_${_resource_case} PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 60
					LABELS "linux-runtime;integration;native-resource-transfer")
			endforeach()
			foreach(_native_case IN ITEMS normal rights stale wrong-fd init-failure spawn transfer kill
					minimal extended wrong-name pci pci-rights pci-stale pci-wrong-fd
					pci-kill pci-wrong-view dma dma-rights dma-stale dma-wrong-fd dma-kill
					dma-unmap-failure)
				set(_native_modules ${_kobox_gem_modules})
				if(_native_case STREQUAL "extended")
					list(APPEND _native_modules "${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
				endif()
				add_test(NAME kobox2.native_controller_${_native_case}
					COMMAND kobox2_native_host_integration_test $<TARGET_FILE:kobox_native_sandbox_test>
						"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" ${_native_modules}
						"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/resource_test.ko" ${_native_case})
				set_tests_properties(kobox2.native_controller_${_native_case} PROPERTIES
					FIXTURES_REQUIRED kobox_full_boot_core TIMEOUT 120
					LABELS "linux-runtime;integration;native-controller")
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
			add_test(NAME kobox2.linux_external_drm_syscall
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--gem-syscall $<TARGET_FILE:kobox_posix_vm_client> ${_kobox_gem_modules}
					"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
			set_tests_properties(kobox2.linux_external_drm_syscall PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
			add_test(NAME kobox2.linux_external_drm_fd_transfer
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--gem-fd-transfer $<TARGET_FILE:kobox_posix_vm_client> ${_kobox_gem_modules}
					"${KOBOX_LINUX_GEM_MODULE_DIR}/kobox/gem/lifetime_test.ko")
			set_tests_properties(kobox2.linux_external_drm_fd_transfer PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
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
		add_test(NAME kobox2.linux_client_task_prerequisite
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --client-task)
		set_tests_properties(kobox2.linux_client_task_prerequisite PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_syscall_dispatch
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --syscall $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_syscall_dispatch PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_fd_transfer
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --fd-transfer $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_fd_transfer PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_fd_exit_race
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --fd-exit-race $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_fd_exit_race PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_fd_inheritance_prerequisite
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --fd-inheritance $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_fd_inheritance_prerequisite PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_fork
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --fork $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_fork PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_autonomous_client
			COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
				--client-run $<TARGET_FILE:kobox_posix_vm_client>)
		add_test(NAME kobox2.linux_external_elf_exec
			COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
				--elf-exec $<TARGET_FILE:kobox_posix_vm_bootstrap>
				$<TARGET_FILE:kobox_elf_client_test>)
		set_tests_properties(kobox2.linux_external_elf_exec PROPERTIES
			TIMEOUT 60 LABELS "integration;linux-runtime;linux-client"
			FIXTURES_REQUIRED kobox_full_boot_core)
		foreach(_user_gate signal vfork)
			add_test(NAME kobox2.linux_external_${_user_gate}
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--elf-exec $<TARGET_FILE:kobox_posix_vm_bootstrap>
					$<TARGET_FILE:kobox_elf_${_user_gate}_client_test>)
			set_tests_properties(kobox2.linux_external_${_user_gate} PROPERTIES
				TIMEOUT 60 LABELS "integration;linux-runtime;linux-client"
				FIXTURES_REQUIRED kobox_full_boot_core)
		endforeach()
		set_tests_properties(kobox2.linux_external_autonomous_client PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_shared_clone
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --clone $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_shared_clone PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		add_test(NAME kobox2.linux_external_thread
			COMMAND kobox_linux_boot_test
				"${KOBOX_LINUX_BOOT_RUNTIME_CORE}" --thread $<TARGET_FILE:kobox_posix_vm_client>)
		set_tests_properties(kobox2.linux_external_thread PROPERTIES
			FIXTURES_REQUIRED kobox_full_boot_core
			LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		foreach(_exit_state IN ITEMS wait running peer)
			add_test(NAME kobox2.linux_external_thread_exit_${_exit_state}
				COMMAND kobox_linux_boot_test "${KOBOX_LINUX_BOOT_RUNTIME_CORE}"
					--thread-exit-${_exit_state} $<TARGET_FILE:kobox_posix_vm_client>)
			set_tests_properties(kobox2.linux_external_thread_exit_${_exit_state} PROPERTIES
				FIXTURES_REQUIRED kobox_full_boot_core
				LABELS "linux-runtime;integration;linux-client" TIMEOUT 60)
		endforeach()
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
		boot/exception_test.c)
	target_compile_features(kobox_linux_exception_test PRIVATE c_std_11)
	target_compile_options(kobox_linux_exception_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_exception_test PRIVATE kobox_posix_boot)
	add_test(NAME kobox2.linux_exception_transport COMMAND kobox_linux_exception_test)
	set_tests_properties(kobox2.linux_exception_transport PROPERTIES TIMEOUT 30)
	add_executable(kobox_linux_boot_image_test boot/image_test.c)
	target_compile_features(kobox_linux_boot_image_test PRIVATE c_std_11)
	target_compile_options(kobox_linux_boot_image_test
		PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_link_libraries(kobox_linux_boot_image_test
		PRIVATE kobox_posix_boot)
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
	host/posix/memory_resource.c
	host/posix/memory_resource_test.c
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
	host/posix/memory_resource.c
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
	host/posix/resource.c
	host/posix/memory_resource.c
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
	host/posix/resource.c
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
	host/posix/resource.c
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
