# SPDX-License-Identifier: GPL-2.0-only

find_package(Threads REQUIRED)

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
