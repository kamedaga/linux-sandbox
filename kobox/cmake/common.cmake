# SPDX-License-Identifier: GPL-2.0-only

add_library(kobox_boot_package STATIC boot/package.c)

target_compile_features(kobox_boot_package PRIVATE c_std_11)

target_compile_options(kobox_boot_package PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_boot_package PUBLIC kobox2::protocol)

add_library(kobox_boot_pci_resource STATIC boot/pci_resource.c)

target_compile_options(kobox_boot_pci_resource PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_boot_pci_resource PUBLIC kobox2::protocol)

add_library(kobox_boot_iommu_resource STATIC boot/iommu_resource.c)

target_compile_options(kobox_boot_iommu_resource PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_boot_iommu_resource PUBLIC kobox2::protocol)

add_library(kobox_resource_registry STATIC runtime/resource_runtime.c)

target_compile_features(kobox_resource_registry PRIVATE c_std_11)

target_compile_options(kobox_resource_registry PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_resource_registry PUBLIC kobox2::protocol)

add_library(kobox_boot_resources STATIC boot/resource_registry.c)

target_include_directories(kobox_boot_resources PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi")

target_compile_options(kobox_boot_resources PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_boot_resources PUBLIC kobox_resource_registry)

add_library(kobox_machine STATIC machine/domain.c)

target_compile_features(kobox_machine PRIVATE c_std_11)

target_compile_options(kobox_machine PRIVATE
-Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-stack-protector)

add_library(kobox_boot_core STATIC boot/core.c)

target_compile_features(kobox_boot_core PRIVATE c_std_11)

target_compile_options(kobox_boot_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(kobox_device_codec STATIC runtime/device_codec.c runtime/device_session.c)

target_compile_features(kobox_device_codec PRIVATE c_std_11)

target_compile_options(kobox_device_codec PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_device_codec PUBLIC kobox2::protocol)
