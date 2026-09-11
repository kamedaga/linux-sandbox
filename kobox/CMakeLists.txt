# SPDX-License-Identifier: GPL-2.0-only

include(cmake/common.cmake)
include(arch/${KB2_CPU_ARCH}/targets.cmake)
include(arch/${KB2_CPU_ARCH}/os/${KB2_OS_BACKEND}.cmake)
include(host/${KB2_OS_BACKEND}/targets.cmake)
include(cmake/core.cmake)
get_directory_property(_kobox_production_targets BUILDSYSTEM_TARGETS)

foreach(_common kobox_boot_core kobox_boot_package kobox_device_codec kobox_resource_registry kobox_arch_elf)
	target_compile_options(${_common} PRIVATE -ffreestanding -fno-stack-protector)
endforeach()

if(BUILD_TESTING)
	add_test(NAME kobox2.common_mesa_verdict
		COMMAND "${CMAKE_COMMAND}" -E env
			"PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tests"
			"${Python3_EXECUTABLE}" -m gates.mesa_test)
	enable_language(ASM)
	include(tests/${KB2_OS_BACKEND}/targets.cmake)
endif()

include(cmake/boundaries.cmake)
kobox_check_production_targets(${_kobox_production_targets})
