# SPDX-License-Identifier: GPL-2.0-only

find_package(Threads REQUIRED)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The fixture ELF loader currently requires x86_64")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "The fixture module currently requires Clang or GCC")
endif()

set(KOBOX_FIXTURE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/linux-sandbox-fixture")
set(KOBOX_FIXTURE_MODULE
    "${KOBOX_FIXTURE_OUTPUT_DIRECTORY}/fixture_module.ko"
)

add_library(
    kobox_fixture_core SHARED
    core/fixture_core.c
    host/test/host_test.c
)
target_include_directories(kobox_fixture_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kobox_fixture_core PRIVATE Threads::Threads)
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

add_custom_command(
    OUTPUT "${KOBOX_FIXTURE_MODULE}"
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
        -c "${CMAKE_CURRENT_SOURCE_DIR}/fixture/module.c"
        -o "${KOBOX_FIXTURE_MODULE}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/fixture/module.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixture/fixture.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/linux/compiler_attributes.h"
    VERBATIM
)
add_custom_target(kobox_fixture_module ALL DEPENDS "${KOBOX_FIXTURE_MODULE}")

add_executable(
    kobox_fixture_sandbox
    fixture/runtime.c
    fixture/sandbox.c
    loader/elf64_loader.c
)
target_include_directories(kobox_fixture_sandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    kobox_fixture_sandbox
    PRIVATE kobox2_test_transport ${CMAKE_DL_LIBS}
)
target_compile_features(kobox_fixture_sandbox PRIVATE c_std_11)
target_compile_options(kobox_fixture_sandbox PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(
    kobox_fixture_sandbox
    PRIVATE
        KOBOX_FIXTURE_CORE_PATH="$<TARGET_FILE:kobox_fixture_core>"
        KOBOX_FIXTURE_MODULE_PATH="${KOBOX_FIXTURE_MODULE}"
)
add_dependencies(kobox_fixture_sandbox kobox_fixture_core kobox_fixture_module)

add_executable(
    kobox_fixture_loader_test
    loader/elf64_loader.c
    loader/elf64_loader_test.c
)
target_compile_features(kobox_fixture_loader_test PRIVATE c_std_11)
target_compile_options(kobox_fixture_loader_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(kobox_fixture_loader_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(
    kobox_fixture_loader_test kobox_fixture_core kobox_fixture_module
)
add_test(
    NAME kobox2.fixture_loader
    COMMAND
        kobox_fixture_loader_test
        $<TARGET_FILE:kobox_fixture_core>
        "${KOBOX_FIXTURE_MODULE}"
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
else()
    target_link_options(kobox_fixture_core PRIVATE -Wl,-z,defs)
endif()
