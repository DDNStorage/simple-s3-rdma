# Copyright 2026 DataDirect Networks, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)

# Set vcpkg toolchain if not already set
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    elseif(EXISTS "/tmp/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "/tmp/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
        message(STATUS "Using vcpkg from /tmp/vcpkg")
    endif()
endif()

project(simple_s3_rdma CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Suppress false positive warnings from fmt library in GCC 14
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14.0)
    add_compile_options(-Wno-array-bounds -Wno-stringop-overflow)
endif()

# Statically link C++ standard library for portability
# This avoids needing specific libstdc++ versions on target systems
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Find required packages
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# spdlog and fmt - use header-only mode for portability
# This avoids needing libspdlog.so and libfmt.so on target systems
add_compile_definitions(SPDLOG_HEADER_ONLY)
add_compile_definitions(FMT_HEADER_ONLY)
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    message(STATUS "spdlog not found, will use header-only mode if available")
endif()

# OpenTelemetry - required
find_package(opentelemetry-cpp CONFIG REQUIRED)
message(STATUS "OpenTelemetry found")

# CUDA/GDS paths for cuObjClient (only needed for the NVIDIA cuObject flavor)
# Note: This may be a minimal GDS installation without full CUDA toolkit
set(CUDA_TOOLKIT_ROOT_DIR "/usr/local/cuda" CACHE PATH "CUDA toolkit root directory")

# Detect architecture-specific include and lib paths
# Modern CUDA (13.0+) uses: targets/${ARCH}/include and targets/${ARCH}/lib
# where ${ARCH} can be:
#   - x86_64-linux (x86_64/AMD64 systems)
#   - sbsa-linux (ARM64 server platforms like NVIDIA DGX)
#   - aarch64-linux (ARM64 embedded/desktop)
#   - ppc64le-linux (IBM POWER)
# Older CUDA uses: include and lib64

# First, try to auto-detect the target architecture
set(CUDA_TARGET_ARCH "")
if(EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/targets")
    file(GLOB CUDA_TARGET_DIRS "${CUDA_TOOLKIT_ROOT_DIR}/targets/*-linux")
    list(LENGTH CUDA_TARGET_DIRS NUM_TARGETS)
    if(NUM_TARGETS EQUAL 1)
        # Only one target directory found, use it
        list(GET CUDA_TARGET_DIRS 0 CUDA_TARGET_PATH)
        get_filename_component(CUDA_TARGET_ARCH "${CUDA_TARGET_PATH}" NAME)
        message(STATUS "Auto-detected CUDA target architecture: ${CUDA_TARGET_ARCH}")
    elseif(NUM_TARGETS GREATER 1)
        # Multiple targets found, try to pick the right one based on CMAKE_SYSTEM_PROCESSOR
        message(STATUS "Multiple CUDA targets found, selecting based on system architecture...")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
            set(CUDA_TARGET_ARCH "x86_64-linux")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            # For ARM64, prefer sbsa-linux (server) over aarch64-linux (embedded)
            if(EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/targets/sbsa-linux")
                set(CUDA_TARGET_ARCH "sbsa-linux")
            elseif(EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/targets/aarch64-linux")
                set(CUDA_TARGET_ARCH "aarch64-linux")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
            set(CUDA_TARGET_ARCH "ppc64le-linux")
        endif()
    endif()
endif()

# Now set the include and lib directories
if(CUDA_TARGET_ARCH AND EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/targets/${CUDA_TARGET_ARCH}/include")
    set(CUDA_INCLUDE_DIR "${CUDA_TOOLKIT_ROOT_DIR}/targets/${CUDA_TARGET_ARCH}/include")
    set(CUDA_LIB_DIR "${CUDA_TOOLKIT_ROOT_DIR}/targets/${CUDA_TARGET_ARCH}/lib")
    message(STATUS "Using modern CUDA layout: ${CUDA_TOOLKIT_ROOT_DIR}/targets/${CUDA_TARGET_ARCH}")
elseif(EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/include")
    set(CUDA_INCLUDE_DIR "${CUDA_TOOLKIT_ROOT_DIR}/include")
    # Try lib64 first (common on x86_64), then lib (common on other architectures)
    if(EXISTS "${CUDA_TOOLKIT_ROOT_DIR}/lib64")
        set(CUDA_LIB_DIR "${CUDA_TOOLKIT_ROOT_DIR}/lib64")
    else()
        set(CUDA_LIB_DIR "${CUDA_TOOLKIT_ROOT_DIR}/lib")
    endif()
    message(STATUS "Using traditional CUDA layout: ${CUDA_TOOLKIT_ROOT_DIR}")
else()
    message(STATUS "No CUDA headers found at ${CUDA_TOOLKIT_ROOT_DIR}")
endif()

# GPU memory (--gpu): one GPU runtime per build. CUDA needs the CUDA driver
# API (cuda.h and the libcuda stub), HIP needs the HIP runtime (libamdhip64).
if(CUDA_INCLUDE_DIR AND EXISTS "${CUDA_INCLUDE_DIR}/cuda.h" AND EXISTS "${CUDA_LIB_DIR}/stubs/libcuda.so")
    set(CUDA_DRIVER_FOUND ON)
else()
    set(CUDA_DRIVER_FOUND OFF)
endif()
set(ROCM_PATH "/opt/rocm" CACHE PATH "ROCm root directory (the HIP runtime may also come from the distribution)")
find_path(HIP_INCLUDE_DIR hip/hip_runtime_api.h HINTS ${ROCM_PATH}/include)
find_library(HIP_LIBRARY amdhip64 HINTS ${ROCM_PATH}/lib)
find_library(HSA_LIBRARY hsa-runtime64 HINTS ${ROCM_PATH}/lib)  # dmabuf export with its offset
if(HIP_INCLUDE_DIR AND HIP_LIBRARY AND HSA_LIBRARY)
    set(HIP_FOUND ON)
else()
    set(HIP_FOUND OFF)
endif()
if(CUDA_DRIVER_FOUND)
    set(GPU_DEFAULT CUDA)
elseif(HIP_FOUND)
    set(GPU_DEFAULT HIP)
else()
    set(GPU_DEFAULT OFF)
endif()
set(SIMPLE_S3_RDMA_GPU ${GPU_DEFAULT} CACHE STRING "GPU runtime for GPU memory support (--gpu): CUDA, HIP or OFF")
set_property(CACHE SIMPLE_S3_RDMA_GPU PROPERTY STRINGS CUDA HIP OFF)
if(SIMPLE_S3_RDMA_GPU STREQUAL "CUDA")
    if(NOT CUDA_DRIVER_FOUND)
        message(FATAL_ERROR "SIMPLE_S3_RDMA_GPU is CUDA but cuda.h/stubs/libcuda.so were not found "
                            "under ${CUDA_TOOLKIT_ROOT_DIR}; set CUDA_TOOLKIT_ROOT_DIR or pick another SIMPLE_S3_RDMA_GPU")
    endif()
    message(STATUS "GPU memory support: CUDA (${CUDA_INCLUDE_DIR})")
elseif(SIMPLE_S3_RDMA_GPU STREQUAL "HIP")
    if(NOT HIP_FOUND)
        message(FATAL_ERROR "SIMPLE_S3_RDMA_GPU is HIP but hip/hip_runtime_api.h/libamdhip64/libhsa-runtime64 were not found; "
                            "install ROCm (or libamdhip64-dev), set ROCM_PATH, or pick another SIMPLE_S3_RDMA_GPU")
    endif()
    message(STATUS "GPU memory support: HIP (${HIP_LIBRARY})")
elseif(SIMPLE_S3_RDMA_GPU STREQUAL "OFF")
    message(STATUS "GPU memory support: disabled")
else()
    message(FATAL_ERROR "SIMPLE_S3_RDMA_GPU must be CUDA, HIP or OFF, not '${SIMPLE_S3_RDMA_GPU}'")
endif()

find_path(CUOBJ_INCLUDE_DIR cuobjclient.h HINTS ${CUDA_INCLUDE_DIR})
find_library(CUOBJ_LIBRARY cuobjclient HINTS ${CUDA_LIB_DIR})
find_library(CUFILE_LIBRARY cufile HINTS ${CUDA_LIB_DIR})
if(CUOBJ_INCLUDE_DIR AND CUOBJ_LIBRARY AND CUFILE_LIBRARY)
    set(CUOBJ_FOUND ON)
else()
    set(CUOBJ_FOUND OFF)
endif()

# NVIDIA cuObject flavor (--flavor nvidia): on by default when
# libcuobjclient is found
if(NOT SIMPLE_S3_RDMA_GPU STREQUAL "CUDA")
    set(CUOBJ_FOUND OFF)  # cuObject needs the CUDA GPU runtime
endif()
option(SIMPLE_S3_RDMA_WITH_CUOBJ "Build the NVIDIA cuObject RDMA flavor (needs SIMPLE_S3_RDMA_GPU=CUDA and libcuobjclient)" ${CUOBJ_FOUND})
if(SIMPLE_S3_RDMA_WITH_CUOBJ)
    if(NOT SIMPLE_S3_RDMA_GPU STREQUAL "CUDA")
        message(FATAL_ERROR "SIMPLE_S3_RDMA_WITH_CUOBJ needs SIMPLE_S3_RDMA_GPU=CUDA")
    endif()
    if(NOT CUOBJ_FOUND)
        message(FATAL_ERROR "SIMPLE_S3_RDMA_WITH_CUOBJ is ON but cuobjclient.h/libcuobjclient were not found "
                            "under ${CUDA_TOOLKIT_ROOT_DIR}; set CUDA_TOOLKIT_ROOT_DIR or pass -DSIMPLE_S3_RDMA_WITH_CUOBJ=OFF")
    endif()
    message(STATUS "cuObject flavor: enabled (${CUOBJ_LIBRARY})")
else()
    message(STATUS "cuObject flavor: disabled")
endif()

# AMD hipObject flavor (--flavor amd): hipobj-rc-v2 over libibverbs RC,
# on by default when rdma-core is found
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
find_library(IBVERBS_LIBRARY ibverbs)
if(IBVERBS_INCLUDE_DIR AND IBVERBS_LIBRARY)
    set(IBVERBS_FOUND ON)
else()
    set(IBVERBS_FOUND OFF)
endif()
option(SIMPLE_S3_RDMA_WITH_RC "Build the AMD hipObject RC RDMA flavor (needs libibverbs)" ${IBVERBS_FOUND})
if(SIMPLE_S3_RDMA_WITH_RC)
    if(NOT IBVERBS_FOUND)
        message(FATAL_ERROR "SIMPLE_S3_RDMA_WITH_RC is ON but infiniband/verbs.h/libibverbs were not found; "
                            "install rdma-core (libibverbs-dev) or pass -DSIMPLE_S3_RDMA_WITH_RC=OFF")
    endif()
    message(STATUS "hipObject RC flavor: enabled (${IBVERBS_LIBRARY})")
else()
    message(STATUS "hipObject RC flavor: disabled")
endif()

# CLI11 - header-only, will be included in source
include_directories(${CMAKE_SOURCE_DIR})

# Main executable
add_executable(simple_s3_rdma
    main.cpp
    RDMATransport.cpp
    BenchmarkThread.cpp
)

if(SIMPLE_S3_RDMA_GPU STREQUAL "CUDA")
    target_sources(simple_s3_rdma PRIVATE GPUMemoryManager.cpp)
    target_include_directories(simple_s3_rdma PRIVATE ${CUDA_INCLUDE_DIR})
    # libcuda.so stub for systems without the driver. Passed as a raw -L rather than
    # link_directories() so it stays out of RUNPATH: otherwise the stub shadows the
    # real driver's libcuda.so.1 at runtime and every CUDA call fails with
    # CUDA_ERROR_STUB_LIBRARY.
    target_link_options(simple_s3_rdma PRIVATE "-L${CUDA_LIB_DIR}/stubs")
    target_link_libraries(simple_s3_rdma cuda)  # links via the stub, needs libcuda.so.1 at runtime
elseif(SIMPLE_S3_RDMA_GPU STREQUAL "HIP")
    target_sources(simple_s3_rdma PRIVATE GPUMemoryHip.cpp)
    # The HIP headers only need the platform; no hipcc, since nothing here is a kernel
    set_source_files_properties(GPUMemoryHip.cpp PROPERTIES COMPILE_DEFINITIONS __HIP_PLATFORM_AMD__)
    # A distribution HIP lives in /usr/include; naming that explicitly breaks #include_next
    if(NOT HIP_INCLUDE_DIR IN_LIST CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        set_source_files_properties(GPUMemoryHip.cpp PROPERTIES INCLUDE_DIRECTORIES ${HIP_INCLUDE_DIR})
    endif()
    target_link_libraries(simple_s3_rdma ${HIP_LIBRARY} ${HSA_LIBRARY})
else()
    target_sources(simple_s3_rdma PRIVATE GPUMemoryStub.cpp)
endif()

if(SIMPLE_S3_RDMA_WITH_CUOBJ)
    target_sources(simple_s3_rdma PRIVATE CuObjTransport.cpp)
    target_compile_definitions(simple_s3_rdma PRIVATE SIMPLE_S3_RDMA_HAVE_CUOBJ)
    target_include_directories(simple_s3_rdma PRIVATE ${CUOBJ_INCLUDE_DIR})
    target_link_libraries(simple_s3_rdma
        ${CUOBJ_LIBRARY}  # GDS library for GPU Direct Storage
        ${CUFILE_LIBRARY} # cuFile library for cuFileDriverOpen
    )
endif()

if(SIMPLE_S3_RDMA_WITH_RC)
    target_sources(simple_s3_rdma PRIVATE RcTransport.cpp)
    target_compile_definitions(simple_s3_rdma PRIVATE SIMPLE_S3_RDMA_HAVE_RC)
    target_include_directories(simple_s3_rdma PRIVATE ${IBVERBS_INCLUDE_DIR})
    target_link_libraries(simple_s3_rdma ${IBVERBS_LIBRARY})
endif()

target_link_libraries(simple_s3_rdma
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    rt   # for shm_open if needed
    -static-libstdc++
    -static-libgcc
)

# spdlog is header-only, no linking needed
if(spdlog_FOUND)
    target_include_directories(simple_s3_rdma PRIVATE ${spdlog_INCLUDE_DIRS})
endif()

target_link_libraries(simple_s3_rdma
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_file_exporter
    opentelemetry-cpp::resources
)

# Compiler flags
target_compile_options(simple_s3_rdma PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
)
