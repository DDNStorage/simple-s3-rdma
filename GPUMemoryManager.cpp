/*
 * Copyright 2026 DataDirect Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GPUMemoryManager.h"

#include <cuda.h>
#include <dlfcn.h>
#include <unistd.h>

namespace {

// BAR1 size and use from NVML, which ships with the driver (libnvidia-ml.so.1)
// but whose header the CUDA toolkit may not have. Loaded at runtime, so a host
// without it just reports no BAR information. The declarations match nvml.h.
struct NvmlBar1Memory {
    unsigned long long bar1Total;
    unsigned long long bar1Free;
    unsigned long long bar1Used;
};
using NvmlDevice = struct nvmlDevice_st*;

std::optional<GPUBarInfo> query_bar1(CUdevice device) {
    char bus_id[32] = {};
    if (cuDeviceGetPCIBusId(bus_id, sizeof(bus_id), device) != CUDA_SUCCESS) {
        return std::nullopt;
    }

    void* nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (nvml == nullptr) {
        spdlog::debug("NVML not available ({}); no BAR1 information", dlerror());
        return std::nullopt;
    }
    auto init = reinterpret_cast<int (*)()>(dlsym(nvml, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<int (*)()>(dlsym(nvml, "nvmlShutdown"));
    auto by_bus_id = reinterpret_cast<int (*)(const char*, NvmlDevice*)>(
        dlsym(nvml, "nvmlDeviceGetHandleByPciBusId_v2"));
    auto bar1_info = reinterpret_cast<int (*)(NvmlDevice, NvmlBar1Memory*)>(
        dlsym(nvml, "nvmlDeviceGetBAR1MemoryInfo"));

    std::optional<GPUBarInfo> bar;
    if (init && shutdown && by_bus_id && bar1_info && init() == 0) {
        NvmlDevice handle = nullptr;
        NvmlBar1Memory mem{};
        if (by_bus_id(bus_id, &handle) == 0 && bar1_info(handle, &mem) == 0) {
            bar = GPUBarInfo{"BAR1", mem.bar1Total, mem.bar1Free, true};
        } else {
            spdlog::debug("NVML has no BAR1 information for {}", bus_id);
        }
        shutdown();
    }
    dlclose(nvml);
    return bar;
}

} // namespace

// Define static members
bool GPUMemoryManager::runtime_initialized = false;
bool GPUMemoryManager::runtime_available = false;

// GPUMemory implementation

GPUMemory& GPUMemory::operator=(GPUMemory&& other) noexcept {
    if (this != &other) {
        reset();
        ptr_ = std::exchange(other.ptr_, nullptr);
        size_ = std::exchange(other.size_, 0);
        device_id_ = std::exchange(other.device_id_, -1);
    }
    return *this;
}

GPUMemory::~GPUMemory() {
    reset();
}

void GPUMemory::reset() {
    if (ptr_) {
        // Ignore errors during cleanup in destructor
        // Access private method via friend relationship
        (void)GPUMemoryManager::free_gpu_memory(ptr_);

        // Release the primary-context reference that allocate_gpu_memory took on our
        // behalf. That retain exists to keep the context alive for exactly as long as
        // this allocation, so the two are paired here rather than left to process exit.
        if (device_id_ >= 0) {
            CUdevice device;
            if (cuDeviceGet(&device, device_id_) == CUDA_SUCCESS) {
                (void)cuDevicePrimaryCtxRelease(device);
            }
        }

        ptr_ = nullptr;
        size_ = 0;
        device_id_ = -1;
    }
}

// GPUMemoryManager implementation

const char* GPUMemoryManager::backend_name() {
    return "CUDA";
}

// Check if CUDA is available by trying to initialize it
std::expected<void, GPUError> GPUMemoryManager::ensure_init() {
    if (runtime_initialized) {
        if (runtime_available) {
            return {};
        }
        return std::unexpected(GPUError::GPU_NOT_AVAILABLE);
    }

    runtime_initialized = true;

    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::warn("CUDA not available: cuInit failed: {}", err_str ? err_str : "unknown error");
        spdlog::warn("GPU features will not be available. Install NVIDIA drivers to enable GPU support.");
        runtime_available = false;
        return std::unexpected(GPUError::GPU_INIT_FAILED);
    }

    runtime_available = true;
    spdlog::debug("CUDA initialized successfully");
    return {};
}

// Enumerate all available CUDA devices
std::expected<std::vector<GPUDeviceInfo>, GPUError> GPUMemoryManager::enumerate_devices() {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    int device_count = 0;
    CUresult err = cuDeviceGetCount(&device_count);

    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::warn("cuDeviceGetCount failed: {}", err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::DEVICE_COUNT_FAILED);
    }

    if (device_count == 0) {
        spdlog::debug("No CUDA devices found");
        return std::vector<GPUDeviceInfo>{};  // Empty vector is valid, not an error
    }

    std::vector<GPUDeviceInfo> devices;

    for (int i = 0; i < device_count; ++i) {
        CUdevice device;
        err = cuDeviceGet(&device, i);
        if (err != CUDA_SUCCESS) {
            continue;
        }

        char name[256];
        cuDeviceGetName(name, sizeof(name), device);

        size_t total_mem = 0;
        cuDeviceTotalMem(&total_mem, device);

        int major = 0, minor = 0;
        cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device);
        cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device);

        int pci_bus_id = 0, pci_device_id = 0;
        cuDeviceGetAttribute(&pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, device);
        cuDeviceGetAttribute(&pci_device_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, device);

        // Get free memory by temporarily setting the primary context as current
        // Note: We retain and release to keep ref count balanced
        CUcontext ctx;
        size_t free_mem = 0;
        size_t total_mem_check = 0;
        if (cuDevicePrimaryCtxRetain(&ctx, device) == CUDA_SUCCESS) {
            CUresult prev_ctx_result = cuCtxSetCurrent(ctx);
            if (prev_ctx_result == CUDA_SUCCESS) {
                cuMemGetInfo(&free_mem, &total_mem_check);
                // Use the total from cuMemGetInfo if it's non-zero
                if (total_mem_check > 0) {
                    total_mem = total_mem_check;
                }
            }
            // Release the reference we just acquired
            // This keeps the ref count balanced - if someone else retained it, it stays alive
            cuDevicePrimaryCtxRelease(device);
        } else {
            free_mem = total_mem; // Assume all free if we can't query
        }

        GPUDeviceInfo info;
        info.device_id = i;
        info.name = name;
        info.total_memory = total_mem;
        info.free_memory = free_mem;
        info.compute_capability_major = major;
        info.compute_capability_minor = minor;
        info.pci_bus_id = pci_bus_id;
        info.pci_device_id = pci_device_id;
        info.bar = query_bar1(device);

        devices.push_back(info);
    }

    return devices;
}

// Print GPU device information
void GPUMemoryManager::print_device_info(const GPUDeviceInfo& info) {
    spdlog::info("GPU {}: {}", info.device_id, info.name);
    spdlog::info("  Compute Capability: {}.{}",
                info.compute_capability_major, info.compute_capability_minor);
    spdlog::info("  Total Memory: {:.2f} GiB ({} bytes)",
                info.total_memory / (1024.0 * 1024.0 * 1024.0), info.total_memory);
    spdlog::info("  Free Memory: {:.2f} GiB ({} bytes)",
                info.free_memory / (1024.0 * 1024.0 * 1024.0), info.free_memory);
    spdlog::info("  PCI Bus ID: {:02x}:{:02x}", info.pci_bus_id, info.pci_device_id);
    if (info.bar) {
        spdlog::info("  {} (GPU-direct RDMA window): {:.0f} MiB total, {:.0f} MiB free",
                     info.bar->name, info.bar->total / (1024.0 * 1024.0), info.bar->free / (1024.0 * 1024.0));
    }
}

// Allocate GPU memory using cuMemAlloc
std::expected<GPUMemory, GPUError> GPUMemoryManager::allocate_gpu_memory(int device_id, size_t size) {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    CUdevice device;
    CUresult err = cuDeviceGet(&device, device_id);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuDeviceGet({}) failed: {}", device_id, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::DEVICE_GET_FAILED);
    }

    // Retain primary context for the device (increments ref count)
    // This keeps the context alive even if set to NULL elsewhere
    CUcontext ctx;
    err = cuDevicePrimaryCtxRetain(&ctx, device);
    spdlog::debug("cuDevicePrimaryCtxRetain: err={}, ctx={}", static_cast<int>(err), reinterpret_cast<void*>(ctx));
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuDevicePrimaryCtxRetain failed for device {}: {}", device_id, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::CONTEXT_RETAIN_FAILED);
    }

    // Make the context current
    err = cuCtxSetCurrent(ctx);
    spdlog::debug("cuCtxSetCurrent: err={}", static_cast<int>(err));
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuCtxSetCurrent failed: {}", err_str ? err_str : "unknown error");
        cuDevicePrimaryCtxRelease(device);
        return std::unexpected(GPUError::CONTEXT_SET_FAILED);
    }

    CUdeviceptr dptr = 0;
    spdlog::debug("Calling cuMemAlloc for {} bytes on device {}", size, device_id);
    err = cuMemAlloc(&dptr, size);
    spdlog::debug("cuMemAlloc: err={}, dptr={:#x}", static_cast<int>(err), static_cast<uint64_t>(dptr));
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemAlloc failed for {} bytes on device {}: {}",
                     size, device_id, err_str ? err_str : "unknown error");
        // Don't release context on allocation failure - it might be used by other allocations
        return std::unexpected(GPUError::ALLOCATION_FAILED);
    }

    spdlog::info("Allocated {} bytes on GPU {} using cuMemAlloc: ptr={:#x}",
                size, device_id, static_cast<uint64_t>(dptr));
    // The retain above is deliberately still held: the context must outlive this call
    // so that cuMemFree can run later. GPUMemory::reset() releases it, which pairs the
    // reference with the lifetime of the allocation it protects.

    // Return GPUMemory RAII wrapper
    return GPUMemory(reinterpret_cast<void*>(dptr), size, device_id);
}

// Free GPU memory
std::expected<void, GPUError> GPUMemoryManager::free_gpu_memory(void* ptr) {
    if (ptr == nullptr) {
        return {};  // Freeing nullptr is a no-op, not an error
    }

    CUdeviceptr dptr = reinterpret_cast<CUdeviceptr>(ptr);
    CUresult err = cuMemFree(dptr);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemFree failed for ptr {}: {}", ptr, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::FREE_FAILED);
    }

    spdlog::debug("Freed GPU memory: ptr={}", ptr);
    return {};
}

// Set the CUDA context for a specific GPU device
std::expected<void, GPUError> GPUMemoryManager::set_device_context(int device_id) {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    CUdevice device;
    CUresult err = cuDeviceGet(&device, device_id);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuDeviceGet({}) failed: {}", device_id, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::DEVICE_GET_FAILED);
    }

    // Retain and set the primary context for this device
    CUcontext ctx;
    err = cuDevicePrimaryCtxRetain(&ctx, device);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuDevicePrimaryCtxRetain failed for device {}: {}", device_id, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::CONTEXT_RETAIN_FAILED);
    }

    err = cuCtxSetCurrent(ctx);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuCtxSetCurrent failed: {}", err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::CONTEXT_SET_FAILED);
    }

    // This reference is held for the remainder of the process. Releasing it here
    // could destroy the primary context we just made current if no allocation holds
    // a reference yet, and primary contexts are shared and torn down at exit anyway.
    // The cost is one refcount, not one context per call.
    spdlog::debug("Set CUDA context current for device {}", device_id);
    return {};
}

// Check if a device ID is valid
std::expected<bool, GPUError> GPUMemoryManager::is_valid_device(int device_id) {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    int device_count = 0;
    CUresult err = cuDeviceGetCount(&device_count);

    if (err != CUDA_SUCCESS) {
        return std::unexpected(GPUError::DEVICE_COUNT_FAILED);
    }

    return device_id >= 0 && device_id < device_count;
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_device(void* dst, const void* src, size_t size) {
    CUresult err = cuMemcpyHtoD(reinterpret_cast<CUdeviceptr>(dst), src, size);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemcpyHtoD of {} bytes to {} failed: {}", size, dst, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_host(void* dst, const void* src, size_t size) {
    CUresult err = cuMemcpyDtoH(dst, reinterpret_cast<CUdeviceptr>(src), size);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemcpyDtoH of {} bytes from {} failed: {}", size, src, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::clear(void* dst, size_t size) {
    CUresult err = cuMemsetD8(reinterpret_cast<CUdeviceptr>(dst), 0, size);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemsetD8 of {} bytes at {} failed: {}", size, dst, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::COPY_FAILED);
    }
    // cuMemsetD8 on device memory can return before the fill is done; wait, or the
    // zeros could land on top of the next RDMA write into this buffer
    err = cuCtxSynchronize();
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuCtxSynchronize after cuMemsetD8 failed: {}", err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<DmabufExport, GPUError> GPUMemoryManager::export_dmabuf(void* ptr, size_t size) {
    // cuMemGetHandleForAddressRange wants a page-aligned length; cuMemAlloc
    // rounds allocations up well past that, so the tail is still ours. The
    // dmabuf covers exactly the requested range, so the offset is 0.
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t len = (size + page - 1) / page * page;
    DmabufExport out;
    CUresult err = cuMemGetHandleForAddressRange(&out.fd, reinterpret_cast<CUdeviceptr>(ptr), len,
                                                 CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
    if (err != CUDA_SUCCESS) {
        const char* err_str = nullptr;
        cuGetErrorString(err, &err_str);
        spdlog::error("cuMemGetHandleForAddressRange of {} bytes at {} failed: {}",
                      len, ptr, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::EXPORT_FAILED);
    }
    return out;
}

void GPUMemoryManager::close_dmabuf(int fd) {
    close(fd);
}
