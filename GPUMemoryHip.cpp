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

// GPUMemoryManager on the HIP runtime (SIMPLE_S3_RDMA_GPU=HIP), for AMD GPUs.
#include "GPUMemoryManager.h"

#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace {

// The CPU-visible part of VRAM (the BAR the NIC reaches it through) from the
// amdgpu sysfs attributes of the device's PCI function.
std::optional<GPUBarInfo> query_visible_vram(int device_id) {
    char bus_id[32] = {};
    if (hipDeviceGetPCIBusId(bus_id, sizeof(bus_id), device_id) != hipSuccess) {
        return std::nullopt;
    }
    std::string dir = std::string("/sys/bus/pci/devices/") + bus_id;
    std::transform(dir.begin(), dir.end(), dir.begin(), [](unsigned char c) { return std::tolower(c); });

    auto read = [&](const char* name) -> std::optional<size_t> {
        std::ifstream in(dir + "/" + name);
        size_t value = 0;
        if (in >> value) {
            return value;
        }
        return std::nullopt;
    };
    auto total = read("mem_info_vis_vram_total");
    auto used = read("mem_info_vis_vram_used");
    if (!total || !used || *used > *total) {
        spdlog::debug("No visible-VRAM information under {}", dir);
        return std::nullopt;
    }
    return GPUBarInfo{"Visible VRAM", *total, *total - *used, false};
}

} // namespace

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
        (void)GPUMemoryManager::free_gpu_memory(ptr_);
        ptr_ = nullptr;
        size_ = 0;
        device_id_ = -1;
    }
}

// GPUMemoryManager implementation

const char* GPUMemoryManager::backend_name() {
    return "HIP";
}

std::expected<void, GPUError> GPUMemoryManager::ensure_init() {
    if (runtime_initialized) {
        if (runtime_available) {
            return {};
        }
        return std::unexpected(GPUError::GPU_NOT_AVAILABLE);
    }

    runtime_initialized = true;

    hipError_t err = hipInit(0);
    if (err != hipSuccess) {
        spdlog::warn("HIP not available: hipInit failed: {}", hipGetErrorString(err));
        spdlog::warn("GPU features will not be available. Install the amdgpu driver and ROCm to enable GPU support.");
        runtime_available = false;
        return std::unexpected(GPUError::GPU_INIT_FAILED);
    }

    runtime_available = true;
    spdlog::debug("HIP initialized successfully");
    return {};
}

std::expected<std::vector<GPUDeviceInfo>, GPUError> GPUMemoryManager::enumerate_devices() {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    int device_count = 0;
    hipError_t err = hipGetDeviceCount(&device_count);
    if (err == hipErrorNoDevice) {
        spdlog::debug("No HIP devices found");
        return std::vector<GPUDeviceInfo>{};  // Empty vector is valid, not an error
    }
    if (err != hipSuccess) {
        spdlog::warn("hipGetDeviceCount failed: {}", hipGetErrorString(err));
        return std::unexpected(GPUError::DEVICE_COUNT_FAILED);
    }

    std::vector<GPUDeviceInfo> devices;

    for (int i = 0; i < device_count; ++i) {
        hipDeviceProp_t props{};
        if (hipGetDeviceProperties(&props, i) != hipSuccess) {
            continue;
        }

        // hipMemGetInfo reports on the current device, so switch to this one
        // and back again
        size_t free_mem = props.totalGlobalMem;
        size_t total_mem = props.totalGlobalMem;
        int prev_device = 0;
        if (hipGetDevice(&prev_device) == hipSuccess && hipSetDevice(i) == hipSuccess) {
            size_t total_check = 0;
            if (hipMemGetInfo(&free_mem, &total_check) == hipSuccess && total_check > 0) {
                total_mem = total_check;
            }
            (void)hipSetDevice(prev_device);
        }

        GPUDeviceInfo info;
        info.device_id = i;
        info.name = props.name;
        info.total_memory = total_mem;
        info.free_memory = free_mem;
        info.compute_capability_major = props.major;
        info.compute_capability_minor = props.minor;
        info.pci_bus_id = props.pciBusID;
        info.pci_device_id = props.pciDeviceID;
        info.bar = query_visible_vram(i);

        devices.push_back(info);
    }

    return devices;
}

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

std::expected<GPUMemory, GPUError> GPUMemoryManager::allocate_gpu_memory(int device_id, size_t size) {
    auto ctx_result = set_device_context(device_id);
    if (!ctx_result) {
        return std::unexpected(ctx_result.error());
    }

    void* ptr = nullptr;
    spdlog::debug("Calling hipMalloc for {} bytes on device {}", size, device_id);
    hipError_t err = hipMalloc(&ptr, size);
    if (err != hipSuccess) {
        spdlog::error("hipMalloc failed for {} bytes on device {}: {}",
                     size, device_id, hipGetErrorString(err));
        return std::unexpected(GPUError::ALLOCATION_FAILED);
    }

    spdlog::info("Allocated {} bytes on GPU {} using hipMalloc: ptr={}", size, device_id, ptr);
    return GPUMemory(ptr, size, device_id);
}

std::expected<void, GPUError> GPUMemoryManager::free_gpu_memory(void* ptr) {
    if (ptr == nullptr) {
        return {};  // Freeing nullptr is a no-op, not an error
    }

    hipError_t err = hipFree(ptr);
    if (err != hipSuccess) {
        spdlog::error("hipFree failed for ptr {}: {}", ptr, hipGetErrorString(err));
        return std::unexpected(GPUError::FREE_FAILED);
    }

    spdlog::debug("Freed GPU memory: ptr={}", ptr);
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::set_device_context(int device_id) {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    hipError_t err = hipSetDevice(device_id);
    if (err != hipSuccess) {
        spdlog::error("hipSetDevice({}) failed: {}", device_id, hipGetErrorString(err));
        return std::unexpected(GPUError::CONTEXT_SET_FAILED);
    }

    spdlog::debug("Set HIP device {} current", device_id);
    return {};
}

std::expected<bool, GPUError> GPUMemoryManager::is_valid_device(int device_id) {
    auto init_result = ensure_init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }

    int device_count = 0;
    hipError_t err = hipGetDeviceCount(&device_count);
    if (err == hipErrorNoDevice) {
        return false;
    }
    if (err != hipSuccess) {
        return std::unexpected(GPUError::DEVICE_COUNT_FAILED);
    }

    return device_id >= 0 && device_id < device_count;
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_device(void* dst, const void* src, size_t size) {
    hipError_t err = hipMemcpy(dst, src, size, hipMemcpyHostToDevice);
    if (err != hipSuccess) {
        spdlog::error("hipMemcpy (host to device) of {} bytes to {} failed: {}", size, dst, hipGetErrorString(err));
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_host(void* dst, const void* src, size_t size) {
    hipError_t err = hipMemcpy(dst, src, size, hipMemcpyDeviceToHost);
    if (err != hipSuccess) {
        spdlog::error("hipMemcpy (device to host) of {} bytes from {} failed: {}", size, src, hipGetErrorString(err));
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::clear(void* dst, size_t size) {
    hipError_t err = hipMemset(dst, 0, size);
    if (err != hipSuccess) {
        spdlog::error("hipMemset of {} bytes at {} failed: {}", size, dst, hipGetErrorString(err));
        return std::unexpected(GPUError::COPY_FAILED);
    }
    // hipMemset on device memory can return before the fill is done; wait, or the
    // zeros could land on top of the next RDMA write into this buffer
    err = hipDeviceSynchronize();
    if (err != hipSuccess) {
        spdlog::error("hipDeviceSynchronize after hipMemset failed: {}", hipGetErrorString(err));
        return std::unexpected(GPUError::COPY_FAILED);
    }
    return {};
}

std::expected<DmabufExport, GPUError> GPUMemoryManager::export_dmabuf(void* ptr, size_t size) {
    // Not hipMemGetHandleForAddressRange: hipMalloc carves small buffers out of
    // larger allocations, and that call exports the whole allocation without
    // saying where ptr sits in it, so every such buffer would register as the
    // start of its allocation. The HSA call reports the offset.
    DmabufExport out;
    hsa_status_t st = hsa_amd_portable_export_dmabuf(ptr, size, &out.fd, &out.offset);
    if (st != HSA_STATUS_SUCCESS) {
        const char* err_str = nullptr;
        hsa_status_string(st, &err_str);
        spdlog::error("hsa_amd_portable_export_dmabuf of {} bytes at {} failed: {}",
                      size, ptr, err_str ? err_str : "unknown error");
        return std::unexpected(GPUError::EXPORT_FAILED);
    }
    return out;
}

void GPUMemoryManager::close_dmabuf(int fd) {
    (void)hsa_amd_portable_close_dmabuf(fd);
}
