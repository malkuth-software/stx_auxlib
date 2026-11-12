# Order-statistic sum research harness (btree/docs/ORDER_STATISTIC_RESEARCH.md
# §5 and §8). Standalone: not part of the stx_auxlib package build.
#
#   cmake -S btree/research/order_statistic -B <build> -DCMAKE_BUILD_TYPE=Release \
#         [-DGUI_FORMS_ROOT=<GUI.Forms checkout>] [-DORDER_STATISTIC_SANITIZE=ON]
#
# GUI_FORMS_ROOT adds the B0 baseline (GUI.Forms' RowHeightIndex, compiled
# from that checkout by path for this experiment only).
cmake_minimum_required(VERSION 3.20)
project(order_statistic_research LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORDER_STATISTIC_SANITIZE "Build with ASan and UBSan" OFF)
set(GUI_FORMS_ROOT "" CACHE PATH "GUI.Forms checkout providing the B0 baseline")

set(research_options -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
if(ORDER_STATISTIC_SANITIZE)
    list(APPEND research_options -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    set(research_link_options -fsanitize=address,undefined)
endif()

add_executable(order_statistic_exhaustive order_statistic_exhaustive.cpp)
target_compile_options(order_statistic_exhaustive PRIVATE ${research_options})
target_link_options(order_statistic_exhaustive PRIVATE ${research_link_options})

set(bench_sources order_statistic_bench.cpp)
if(GUI_FORMS_ROOT)
    list(APPEND bench_sources ${GUI_FORMS_ROOT}/src/controls/collections/row_height_index.cpp)
endif()

foreach(bench IN ITEMS order_statistic_bench order_statistic_bench_counting)
    add_executable(${bench} ${bench_sources})
    # The package's headers, for the S5 and S6 variants.
    target_include_directories(${bench} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_compile_options(${bench} PRIVATE ${research_options})
    target_link_options(${bench} PRIVATE ${research_link_options})
    if(GUI_FORMS_ROOT)
        target_include_directories(${bench} PRIVATE ${GUI_FORMS_ROOT})
        target_compile_definitions(${bench} PRIVATE ORDER_STATISTIC_HAS_BASELINE=1)
    endif()
endforeach()
# The counting build records descents and node visits; time only the other.
target_compile_definitions(order_statistic_bench_counting PRIVATE ORDER_STATISTIC_COUNTING=1)
