cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(stx_auxlib VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(STX_AUXLIB_TESTS_DEFAULT ON)
else()
    set(STX_AUXLIB_TESTS_DEFAULT ${GUI_FORMS_BUILD_TESTS})
endif()
option(STX_AUXLIB_TESTS "Build standalone stx_auxlib tests" ${STX_AUXLIB_TESTS_DEFAULT})

find_package(Threads REQUIRED)

# Integer checks: every unsigned wrap, signed overflow, shift out of range
# and value-changing implicit conversion halts the program, except in
# functions marked STX_AUXLIB_MODULAR (src/checked.hpp) and in the code
# tests/integer_ignorelist.txt names. A Clang-only verification mode beside
# the address, thread and undefined-behaviour builds.
option(STX_AUXLIB_INTEGER_CHECKS "Build with Clang's integer sanitizers, halting on any report" OFF)
if(STX_AUXLIB_INTEGER_CHECKS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "STX_AUXLIB_INTEGER_CHECKS requires Clang")
    endif()
    set(stx_auxlib_integer_flags
        -fsanitize=signed-integer-overflow,unsigned-integer-overflow,shift,implicit-integer-truncation,implicit-integer-sign-change,float-cast-overflow,pointer-overflow
        -fno-sanitize-recover=all
        -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/tests/integer_ignorelist.txt)
    add_compile_options(${stx_auxlib_integer_flags})
    add_link_options(${stx_auxlib_integer_flags})
endif()

# The memory backend (include/stx_auxlib/memory.hpp). Sanitizers track only
# the host allocator, so a build whose flags enable address or thread
# sanitizers defaults to the host backend, and src/hosted/memory_mimalloc.cpp
# refuses to compile in one.
if("${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_INIT}" MATCHES "-fsanitize=[^ ]*(address|thread)")
    set(STX_AUXLIB_ALLOCATOR_DEFAULT host)
else()
    set(STX_AUXLIB_ALLOCATOR_DEFAULT mimalloc)
endif()
set(STX_AUXLIB_ALLOCATOR ${STX_AUXLIB_ALLOCATOR_DEFAULT} CACHE STRING
    "stx_auxlib memory backend: mimalloc or host")
set_property(CACHE STX_AUXLIB_ALLOCATOR PROPERTY STRINGS mimalloc host)
if(NOT STX_AUXLIB_ALLOCATOR MATCHES "^(mimalloc|host)$")
    message(FATAL_ERROR "STX_AUXLIB_ALLOCATOR must be mimalloc or host")
endif()
option(STX_AUXLIB_MIMALLOC_SECURE
    "Build mimalloc with its security mitigations (MI_SECURE=4; guard pages, encoded free lists, double-free detection; mimalloc cites under 25% cost)"
    OFF)

# src/hosted/ implements the library over the C++ standard library and its
# host. A freestanding implementation would sit beside it and replace it
# whole; public headers name no platform type.
# The host backends: one of src/hosted/<service>_posix.cpp and
# <service>_windows.cpp per host service.
if(WIN32)
    set(STX_AUXLIB_HOST windows)
else()
    set(STX_AUXLIB_HOST posix)
endif()
# The owned transcendental kernels (include/stx_auxlib/math.hpp), one
# source and table header per family under src/math/. They are compiled with
# the library's private -ffp-contract=off, which never reaches consumers.
set(STX_AUXLIB_MATH_SOURCES
    src/math/atan.cpp
    src/math/cbrt.cpp
    src/math/constants.cpp
    src/math/exp.cpp
    src/math/hyperbolic.cpp
    src/math/hypot.cpp
    src/math/log.cpp
    src/math/pow.cpp
    src/math/sincos.cpp
    src/math/tan.cpp)
add_library(stx_auxlib STATIC
    ${STX_AUXLIB_MATH_SOURCES}
    src/calendar.cpp
    src/hash.cpp
    src/paths.cpp
    src/precedence.cpp
    src/random.cpp
    src/text.cpp
    src/unicode.cpp
    src/voting.cpp
    src/hosted/calendar_${STX_AUXLIB_HOST}.cpp
    src/hosted/children_common.cpp
    src/hosted/children_${STX_AUXLIB_HOST}.cpp
    src/hosted/clocks.cpp
    src/hosted/console.cpp
    src/hosted/console_${STX_AUXLIB_HOST}.cpp
    src/hosted/environment.cpp
    src/hosted/desktop_${STX_AUXLIB_HOST}.cpp
    src/hosted/entropy_${STX_AUXLIB_HOST}.cpp
    src/hosted/environment_${STX_AUXLIB_HOST}.cpp
    src/hosted/events_${STX_AUXLIB_HOST}.cpp
    src/hosted/file_${STX_AUXLIB_HOST}.cpp
    src/hosted/files.cpp
    src/hosted/process.cpp
    src/hosted/terminal_${STX_AUXLIB_HOST}.cpp
    src/hosted/memory.cpp
    src/hosted/memory_${STX_AUXLIB_ALLOCATOR}.cpp
    src/hosted/thread_pool.cpp
    src/hosted/threads.cpp)
add_library(stx_auxlib::stx_auxlib ALIAS stx_auxlib)
target_compile_features(stx_auxlib PUBLIC cxx_std_20)
set_target_properties(stx_auxlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# btree/include holds stx_btree (header-only; btree/PROVENANCE.md), whose
# headers keep their own "stx_btree/..." paths.
target_include_directories(stx_auxlib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/btree/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(stx_auxlib PRIVATE Threads::Threads)
if(WIN32)
    # BCryptGenRandom (src/hosted/entropy_windows.cpp) and ShellExecuteW
    # (src/hosted/desktop_windows.cpp).
    target_link_libraries(stx_auxlib PRIVATE bcrypt shell32)
    # Windows 10 API level, as GUI.Forms uses (SystemTimeToTzSpecificLocalTimeEx,
    # PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
    target_compile_definitions(stx_auxlib PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(stx_auxlib PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    # The owned kernels' bit-identical results need floating-point
    # contraction off. It stays private: consumers keep fused arithmetic,
    # which is desirable outside the replaced calls (owner, 2026-10-31).
    # -fno-fast-math turns math-errno back on for GNU and MinGW targets, which
    # leaves a C library call (to set errno) behind std::sqrt; the kernels
    # never read errno.
    target_compile_options(stx_auxlib PRIVATE -ffp-contract=off -fno-fast-math -fno-math-errno)
elseif(MSVC)
    target_compile_options(stx_auxlib PRIVATE /W4 /fp:precise)
endif()
option(STX_AUXLIB_BUILD_TOOLS "Build stx_auxlib's table generators" OFF)

if(STX_AUXLIB_ALLOCATOR STREQUAL "mimalloc")
    # mimalloc v3.5.3 (third_party/mimalloc, MIT; PROVENANCE.md), compiled
    # as its one translation unit (src/static.c) into stx_auxlib, with the
    # definitions and flags its own static-library build uses. It replaces
    # no C library function: MI_MALLOC_OVERRIDE stays undefined.
    #
    # It is compiled as C++ (mimalloc's MI_USE_CXX). Compiled as C, it
    # defines a weak fallback std::get_new_handler that returns null; with
    # hidden visibility that fallback binds in place of the C++ library's,
    # and every std::get_new_handler call in the program then reports no
    # handler (tests/memory_tests.cpp checks the readback).
    set_source_files_properties(third_party/mimalloc/src/static.c PROPERTIES LANGUAGE CXX)
    add_library(stx_auxlib_mimalloc OBJECT third_party/mimalloc/src/static.c)
    set_target_properties(stx_auxlib_mimalloc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(stx_auxlib_mimalloc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include)
    target_compile_definitions(stx_auxlib_mimalloc PRIVATE
        MI_STATIC_LIB MI_BUILD_RELEASE NDEBUG=1)
    if(STX_AUXLIB_MIMALLOC_SECURE)
        target_compile_definitions(stx_auxlib_mimalloc PRIVATE MI_SECURE=4)
    endif()
    # Vendored code: its warnings are upstream's, so a consumer's -Werror
    # must not reject it (-w).
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(stx_auxlib_mimalloc PRIVATE
            -w -fvisibility=hidden -ftls-model=initial-exec)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(stx_auxlib_mimalloc PRIVATE -mno-outline)
    endif()
    target_sources(stx_auxlib PRIVATE $<TARGET_OBJECTS:stx_auxlib_mimalloc>)
    target_include_directories(stx_auxlib PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include)
    if(WIN32)
        target_link_libraries(stx_auxlib PRIVATE psapi shell32 user32 advapi32 bcrypt)
    endif()

    # The replacement operator new and delete. Only an executable links it
    # (include/stx_auxlib/memory.hpp, "One allocator per process").
    add_library(stx_auxlib_global_allocator STATIC src/hosted/global_allocator.cpp)
    target_link_libraries(stx_auxlib_global_allocator PUBLIC stx_auxlib)
else()
    # The host backend leaves operator new and delete to the toolchain.
    add_library(stx_auxlib_global_allocator INTERFACE)
    target_link_libraries(stx_auxlib_global_allocator INTERFACE stx_auxlib)
endif()
add_library(stx_auxlib::global_allocator ALIAS stx_auxlib_global_allocator)
set_target_properties(stx_auxlib_global_allocator PROPERTIES EXPORT_NAME global_allocator)

if(STX_AUXLIB_TESTS)
    enable_testing()
    # The reference thread pool is C (tests/reference/atomic_pool.c).
    enable_language(C)

    # Every test executable runs on the program allocator it would ship with.
    add_executable(stx_auxlib_memory_tests tests/memory_tests.cpp)
    target_link_libraries(stx_auxlib_memory_tests PRIVATE stx_auxlib::global_allocator)
    target_compile_definitions(stx_auxlib_memory_tests PRIVATE
        STX_AUXLIB_EXPECT_$<UPPER_CASE:${STX_AUXLIB_ALLOCATOR}>=1)
    add_test(NAME stx_auxlib_memory_tests COMMAND stx_auxlib_memory_tests)

    add_executable(stx_auxlib_paths_tests tests/paths_tests.cpp)
    target_link_libraries(stx_auxlib_paths_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_paths_tests COMMAND stx_auxlib_paths_tests)

    add_executable(stx_auxlib_files_tests tests/files_tests.cpp)
    target_link_libraries(stx_auxlib_files_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_files_tests COMMAND stx_auxlib_files_tests)

    add_executable(stx_auxlib_environment_tests tests/environment_tests.cpp)
    target_link_libraries(stx_auxlib_environment_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_environment_tests COMMAND stx_auxlib_environment_tests)

    # Console and process behaviour is visible only from outside the
    # process, so a script runs the program's modes and checks their output,
    # input handling, exit status and fault report.
    add_executable(stx_auxlib_console_process_tests tests/console_process_tests.cpp)
    target_link_libraries(stx_auxlib_console_process_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_console_process_tests
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:stx_auxlib_console_process_tests>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/console_process_work
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/console_process_check.cmake)

    add_executable(stx_auxlib_precedence_tests tests/precedence_tests.cpp)
    target_link_libraries(stx_auxlib_precedence_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_precedence_tests COMMAND stx_auxlib_precedence_tests)
    set_tests_properties(stx_auxlib_precedence_tests PROPERTIES TIMEOUT 300)

    add_executable(stx_auxlib_random_tests tests/random_tests.cpp)
    target_link_libraries(stx_auxlib_random_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_random_tests COMMAND stx_auxlib_random_tests)

    # Every outcome of the reference count (tests/reference/prv_cases.txt,
    # made by prv_reference.mjs from the original prv-core.mjs) must match.
    # stx_btree's own tests (btree/tests), unchanged.
    foreach(btree_test IN ITEMS unsync_map bbtree_map stress optimal_tree optimal_map)
        add_executable(stx_btree_${btree_test}_test btree/tests/${btree_test}_test.cpp)
        target_include_directories(stx_btree_${btree_test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/btree/tests)
        target_link_libraries(stx_btree_${btree_test}_test PRIVATE stx_auxlib::global_allocator)
        # Under ThreadSanitizer, inlining can erase an optimistic reader's
        # frame (the read is attributed to the test's thread body), and the
        # suppressions below match readers by name; keep every frame.
        if("${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize=[^ ]*thread")
            target_compile_options(stx_btree_${btree_test}_test PRIVATE -fno-inline)
        endif()
        # The stress test's argument, as stx_btree's own build passes it.
        set(btree_arguments)
        if(btree_test STREQUAL "stress")
            set(btree_arguments 2)
        endif()
        add_test(NAME stx_btree_${btree_test}_test COMMAND stx_btree_${btree_test}_test ${btree_arguments})
        # Under ThreadSanitizer, the shared map's deliberate optimistic
        # reads are suppressed by reader name (tests/tsan_btree_suppressions.txt);
        # any other race still fails the test.
        set_tests_properties(stx_btree_${btree_test}_test PROPERTIES TIMEOUT 600 ENVIRONMENT
            "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan_btree_suppressions.txt:halt_on_error=1")
    endforeach()

    add_executable(stx_auxlib_voting_tests tests/voting_tests.cpp)
    target_link_libraries(stx_auxlib_voting_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_voting_tests
        COMMAND stx_auxlib_voting_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/prv_cases.txt)
    set_tests_properties(stx_auxlib_voting_tests PROPERTIES TIMEOUT 300)

    add_executable(stx_auxlib_text_tests tests/text_tests.cpp)
    target_link_libraries(stx_auxlib_text_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_text_tests COMMAND stx_auxlib_text_tests)

    add_executable(stx_auxlib_calendar_tests tests/calendar_tests.cpp)
    target_link_libraries(stx_auxlib_calendar_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_calendar_tests COMMAND stx_auxlib_calendar_tests)

    add_executable(stx_auxlib_capabilities_tests tests/capabilities_tests.cpp)
    target_link_libraries(stx_auxlib_capabilities_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_capabilities_tests COMMAND stx_auxlib_capabilities_tests)
    set_tests_properties(stx_auxlib_capabilities_tests PROPERTIES TIMEOUT 120)

    add_executable(stx_auxlib_clocks_tests tests/clocks_tests.cpp)
    target_link_libraries(stx_auxlib_clocks_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_clocks_tests COMMAND stx_auxlib_clocks_tests)

    add_executable(stx_auxlib_threads_tests tests/threads_tests.cpp)
    target_link_libraries(stx_auxlib_threads_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_threads_tests COMMAND stx_auxlib_threads_tests)
    set_tests_properties(stx_auxlib_threads_tests PROPERTIES TIMEOUT 60)

    # math_tests checks frozen output hashes, so a flag that changes any bit
    # fails it. The Dekker build compiles its own copies of the kernels with
    # the no-FMA exact product forced and must give the same bits: the
    # cross-architecture determinism evidence on an FMA machine. Its kernel
    # objects define the library's math symbols, so the linker never pulls
    # the library's own math objects into it. `--exhaustive` and `--bench`
    # stay out of ctest.
    add_executable(stx_auxlib_math_tests tests/math_tests.cpp)
    add_executable(stx_auxlib_math_dekker_tests tests/math_tests.cpp ${STX_AUXLIB_MATH_SOURCES})
    target_compile_definitions(stx_auxlib_math_dekker_tests PRIVATE STX_AUXLIB_MATH_DEKKER)
    foreach(math_test IN ITEMS stx_auxlib_math_tests stx_auxlib_math_dekker_tests)
        target_link_libraries(${math_test} PRIVATE stx_auxlib::global_allocator)
        target_include_directories(${math_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        # The test's inlined double-double reference (tests/reference/wide.hpp)
        # needs contraction off too: contracted, its own self-check fails.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${math_test} PRIVATE -ffp-contract=off -fno-fast-math -fno-math-errno)
        endif()
        add_test(NAME ${math_test} COMMAND ${math_test})
    endforeach()

    add_executable(stx_auxlib_thread_pool_tests tests/thread_pool_tests.cpp)
    target_link_libraries(stx_auxlib_thread_pool_tests PRIVATE stx_auxlib::global_allocator)
    add_test(NAME stx_auxlib_thread_pool_tests COMMAND stx_auxlib_thread_pool_tests)

    # One conformance source, written against the reference's C interface,
    # checks the library (through tests/reference/atomic_pool_adapter.cpp) and
    # the reference itself; both executables must pass.
    add_executable(stx_auxlib_thread_pool_conformance
        tests/thread_pool_conformance.cpp tests/reference/atomic_pool_adapter.cpp)
    target_link_libraries(stx_auxlib_thread_pool_conformance PRIVATE stx_auxlib::global_allocator)
    add_executable(stx_auxlib_thread_pool_reference_conformance
        tests/thread_pool_conformance.cpp tests/reference/atomic_pool.c)
    foreach(conformance IN ITEMS stx_auxlib_thread_pool_conformance
            stx_auxlib_thread_pool_reference_conformance)
        target_include_directories(${conformance} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference)
        target_compile_features(${conformance} PRIVATE cxx_std_20)
        # The reference calls pthreads directly; CLANG64's libc++ uses native
        # Windows threads and supplies no implicit pthread dependency.
        if(MINGW)
            target_link_libraries(${conformance} PRIVATE winpthread)
        else()
            target_link_libraries(${conformance} PRIVATE Threads::Threads)
        endif()
        add_test(NAME ${conformance} COMMAND ${conformance})
        # A hang detector: a broken claim or lock deadlocks rather than fails.
        set_tests_properties(${conformance} PROPERTIES TIMEOUT 60)
    endforeach()
endif()

if(STX_AUXLIB_BUILD_TOOLS AND NOT WIN32)
    # Times each operation against its platform equivalent (POSIX hosts);
    # exits nonzero when any is over twice the platform's time.
    add_executable(stx_auxlib_performance_bench tests/performance_bench.cpp)
    target_link_libraries(stx_auxlib_performance_bench PRIVATE stx_auxlib::stx_auxlib)
endif()

if(STX_AUXLIB_BUILD_TOOLS)
    # Each regenerates its family's table header in src/math/ byte for byte,
    # at the path given as its one argument.
    foreach(table IN ITEMS atan exp log sincos)
        add_executable(stx_auxlib_gen_${table}_table tools/gen_${table}_table.cpp)
        target_compile_features(stx_auxlib_gen_${table}_table PRIVATE cxx_std_20)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(stx_auxlib_gen_${table}_table PRIVATE
                -ffp-contract=off -fno-fast-math)
        endif()
    endforeach()
endif()

install(TARGETS stx_auxlib stx_auxlib_global_allocator EXPORT stx_auxlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY btree/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT stx_auxlibTargets NAMESPACE stx_auxlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stx_auxlib)
configure_package_config_file(cmake/stx_auxlibConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/stx_auxlibConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stx_auxlib)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/stx_auxlibConfigVersion.cmake
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stx_auxlibConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/stx_auxlibConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stx_auxlib)
install(FILES LICENSE PROVENANCE.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/stx_auxlib)
install(FILES btree/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/stx_auxlib RENAME stx_btree-LICENSE.txt)
install(FILES btree/NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/stx_auxlib RENAME stx_btree-NOTICE.txt)
install(FILES third_party/mimalloc/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/stx_auxlib RENAME mimalloc-LICENSE.txt)
