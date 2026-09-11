# SPDX-License-Identifier: GPL-2.0-only

find_package(Threads REQUIRED)

add_library(kobox_posix_package STATIC host/posix/package.c)

target_compile_options(kobox_posix_package PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_posix_package PUBLIC kobox_boot_package kobox_arch_elf)

add_library(kobox_posix_resources STATIC host/posix/resource.c)

target_compile_options(kobox_posix_resources PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_posix_resources PUBLIC kobox_resource_registry)

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

target_link_libraries(kobox_posix_host PUBLIC kobox_machine)

add_library(kobox_posix_core STATIC host/posix/core.c)

target_compile_options(kobox_posix_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_posix_core PUBLIC kobox_boot_core ${CMAKE_DL_LIBS})

add_library(kobox_posix_boot STATIC host/posix/image.c host/posix/exception.c)

target_compile_features(kobox_posix_boot PRIVATE c_std_11)

target_compile_options(kobox_posix_boot PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_posix_boot PUBLIC kobox_posix_core kobox_posix_host kobox_arch_linux_x86)

add_library(kobox_posix_vm STATIC host/posix/vm.c host/posix/vm_service.c)

target_compile_features(kobox_posix_vm PRIVATE c_std_11)

target_compile_options(kobox_posix_vm PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_posix_vm PUBLIC kobox_posix_host kobox_arch_linux_x86)

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

add_library(kobox_linux_posix_machine STATIC task/posix_machine.c)

target_compile_features(kobox_linux_posix_machine PRIVATE c_std_11)

target_compile_options(kobox_linux_posix_machine
	PRIVATE -Wall -Wextra -Wpedantic -Werror)

target_link_libraries(kobox_linux_posix_machine PUBLIC kobox_posix_host)

add_library(kobox_posix_mm STATIC mm/posix.c)
target_compile_features(kobox_posix_mm PRIVATE c_std_11)
target_compile_options(kobox_posix_mm PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_mm PUBLIC kobox_posix_vm kobox_linux_posix_machine)

add_library(kobox_posix_device_channel STATIC host/posix/device_channel.c)
target_compile_options(kobox_posix_device_channel PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_device_channel PUBLIC kobox_device_codec)

add_library(kobox_posix_device_proxy STATIC host/posix/device_proxy.c)
target_compile_options(kobox_posix_device_proxy PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_device_proxy PUBLIC kobox_posix_device_channel kobox_posix_host)

add_library(kobox_posix_bootstrap STATIC host/posix/bootstrap.c)
target_compile_features(kobox_posix_bootstrap PRIVATE c_std_11)
target_compile_options(kobox_posix_bootstrap PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kobox_posix_bootstrap PUBLIC kobox_posix_boot kobox_linux_posix_machine)

add_executable(kobox_posix_vm_bootstrap host/posix/vm_bootstrap.c arch/x86_64/linux_vm_bootstrap.S)
set(_kobox_vm_bootstrap_script "${CMAKE_CURRENT_BINARY_DIR}/vm_bootstrap.lds")
add_custom_command(OUTPUT "${_kobox_vm_bootstrap_script}"
	COMMAND "${CMAKE_C_COMPILER}" -E -P -x c
		"${CMAKE_CURRENT_SOURCE_DIR}/host/posix/vm_bootstrap.lds.S"
		-o "${_kobox_vm_bootstrap_script}"
	DEPENDS host/posix/vm_bootstrap.lds.S arch/x86_64/user_layout.h
	VERBATIM)
target_sources(kobox_posix_vm_bootstrap PRIVATE "${_kobox_vm_bootstrap_script}")
set_property(TARGET kobox_posix_vm_bootstrap APPEND PROPERTY LINK_DEPENDS
	"${_kobox_vm_bootstrap_script}")
target_compile_features(kobox_posix_vm_bootstrap PRIVATE c_std_11)
target_compile_options(kobox_posix_vm_bootstrap PRIVATE
	-Wall -Wextra -Wpedantic -Werror -O2 -ffreestanding -fno-builtin
	-fno-stack-protector -fno-pie -mcmodel=large -fno-sanitize=all)
target_link_options(kobox_posix_vm_bootstrap PRIVATE
	-nostdlib -static -fno-sanitize=all
	"LINKER:-T,${_kobox_vm_bootstrap_script}" LINKER:--build-id=none)
