cmake_minimum_required(VERSION 3.18)
project(multi_process_latency_compute_budget_profiler CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_ARCHITECTURES OFF)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --generate-code arch=compute_89,code=sm_89")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_library(BPF_LIB bpf REQUIRED)

add_library(gpu_kernel src/gpu_kernel.cu)
target_include_directories(gpu_kernel PUBLIC include)

add_executable(pipeline_stage src/pipeline_stage.cpp)
target_include_directories(pipeline_stage PRIVATE include)
target_link_libraries(pipeline_stage PRIVATE gpu_kernel)

add_executable(pipeline_launcher src/pipeline_launcher.cpp)

add_executable(profiler src/profiler.cpp)
target_include_directories(profiler PRIVATE include)
target_link_libraries(profiler PRIVATE ${BPF_LIB})

# profiler_bpf.o: BPF bytecode, built with clang -target bpf directly
# (CMake has no first-class BPF language support), not with the CXX/CUDA
# compilers this project otherwise uses.
find_program(CLANG clang REQUIRED)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/profiler_bpf.o
  COMMAND ${CLANG} -target bpf -D__TARGET_ARCH_x86 -O2 -g -c ${CMAKE_SOURCE_DIR}/src/profiler_bpf.c
          -I/usr/include/x86_64-linux-gnu -o ${CMAKE_BINARY_DIR}/profiler_bpf.o
  DEPENDS ${CMAKE_SOURCE_DIR}/src/profiler_bpf.c
)
add_custom_target(profiler_bpf ALL DEPENDS ${CMAKE_BINARY_DIR}/profiler_bpf.o)
add_dependencies(profiler profiler_bpf)

enable_testing()
add_executable(check_perfetto_writer tests/test_perfetto_writer.cpp)
target_include_directories(check_perfetto_writer PRIVATE include)
add_test(NAME check_perfetto_writer COMMAND check_perfetto_writer)
