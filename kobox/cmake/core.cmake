# SPDX-License-Identifier: GPL-2.0-only

# External canonical inputs remain explicit. Building the core never launches
# it; native execution belongs to the selected backend's CTest launchers.
set(KB2_CORE_CANONICAL_DIR "" CACHE PATH "Canonical pinned Linux Kbuild directory")
set(KB2_CORE_PROVIDER_DIR "" CACHE PATH "Private hosted Kbuild directory")
set(KB2_CORE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime" CACHE PATH "Core artifact directory")
option(KB2_CORE_WITH_GATES "Compile conformance workloads into the core" OFF)
if(KB2_CORE_WITH_GATES AND NOT BUILD_TESTING)
	message(FATAL_ERROR "KB2_CORE_WITH_GATES requires BUILD_TESTING")
endif()
if(KB2_CORE_CANONICAL_DIR)
	if(NOT KB2_CORE_PROVIDER_DIR)
		message(FATAL_ERROR "Core build requires a private KB2_CORE_PROVIDER_DIR")
	endif()
	set(_kb2_gate_arguments)
	if(KB2_CORE_WITH_GATES)
		list(APPEND _kb2_gate_arguments --with-gates)
	endif()
	# Kbuild owns freshness across the full tree; an explicit invocation
	# rechecks it instead of trusting a partial CMake dependency list.
	add_custom_target(kobox_boot_runtime
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/boot/build_boot_runtime.py"
			--source-tree "${CMAKE_CURRENT_SOURCE_DIR}/.."
			--canonical-build-dir "${KB2_CORE_CANONICAL_DIR}"
			--provider-build-dir "${KB2_CORE_PROVIDER_DIR}"
			--output-dir "${KB2_CORE_OUTPUT_DIR}"
			--os-backend "${KB2_OS_BACKEND}" --cpu-arch "${KB2_CPU_ARCH}"
			--link ${_kb2_gate_arguments}
		BYPRODUCTS "${KB2_CORE_OUTPUT_DIR}/linux-boot-runtime.so"
			"${KB2_CORE_OUTPUT_DIR}/linux-boot-inputs.json"
			"${KB2_CORE_OUTPUT_DIR}/machine-bindings.json"
		USES_TERMINAL VERBATIM)
endif()
