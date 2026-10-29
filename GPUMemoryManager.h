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

#ifndef GPU_MEMORY_MANAGER_H
#define GPU_MEMORY_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

// Error types for GPU operations
enum class GPUError {
    GPU_NOT_AVAILABLE,
    GPU_INIT_FAILED,
    DEVICE_NOT_FOUND,
    DEVICE_GET_FAILED,
    CONTEXT_RETAIN_FAILED,
    CONTEXT_SET_FAILED,
    ALLOCATION_FAILED,
    FREE_FAILED,
    DEVICE_COUNT_FAILED,
    INVALID_DEVICE_ID,
    COPY_FAILED,
    EXPORT_FAILED
};

// Convert GPUError to string for logging
constexpr std::string_view gpu_error_to_string(GPUError err) {
    using namespace std::string_view_literals;
    switch (err) {
        case GPUError::GPU_NOT_AVAILABLE: return "GPU runtime not available"sv;
        case GPUError::GPU_INIT_FAILED: return "GPU runtime initialization failed"sv;
        case GPUError::DEVICE_NOT_FOUND: return "Device not found"sv;
        case GPUError::DEVICE_GET_FAILED: return "Failed to get device"sv;
        case GPUError::CONTEXT_RETAIN_FAILED: return "Failed to retain context"sv;
        case GPUError::CONTEXT_SET_FAILED: return "Failed to set context"sv;
        case GPUError::ALLOCATION_FAILED: return "Memory allocation failed"sv;
        case GPUError::FREE_FAILED: return "Memory free failed"sv;
        case GPUError::DEVICE_COUNT_FAILED: return "Failed to get device count"sv;
        case GPUError::INVALID_DEVICE_ID: return "Invalid device ID"sv;
        case GPUError::COPY_FAILED: return "Memory copy failed"sv;
        case GPUError::EXPORT_FAILED: return "dmabuf export failed"sv;
    }
    return "Unknown error"sv;
}

// PCIe BAR space for GPU-direct RDMA (BAR1 on NVIDIA, CPU-visible VRAM on AMD)
struct GPUBarInfo {
    const char* name = "BAR";  // what the driver calls it: "BAR1", "Visible VRAM"
    size_t total = 0;
    size_t free = 0;
    // true when buffers that do not fit fail registration (NVIDIA BAR1); false
    // when the driver may fall back to staging through system memory instead
    bool hard_limit = false;
};

// GPU device information
struct GPUDeviceInfo {
    int device_id;
    std::string name;
    size_t total_memory;
    size_t free_memory;
    int compute_capability_major;
    int compute_capability_minor;
    int pci_bus_id;
    int pci_device_id;
    // The PCIe BAR window the NIC reaches GPU memory through, when the driver
    // reports it: every GPU buffer registered for RDMA must fit in bar_free.
    std::optional<GPUBarInfo> bar;
};

// A dmabuf exported from device memory (see GPUMemoryManager::export_dmabuf)
struct DmabufExport {
    int fd = -1;
    uint64_t offset = 0;  // where the exported pointer starts within the dmabuf
};

// Forward declaration
class GPUMemoryManager;

// RAII wrapper for GPU memory allocation
// Automatically frees GPU memory on destruction
// Move-only to prevent accidental copying of GPU pointers
class GPUMemory {
private:
    void* ptr_ = nullptr;
    size_t size_ = 0;
    int device_id_ = -1;

    // Allow GPUMemoryManager full access (for construction and free_gpu_memory)
    friend class GPUMemoryManager;

    GPUMemory(void* ptr, size_t size, int device_id)
        : ptr_(ptr), size_(size), device_id_(device_id) {}

public:
    // Default constructor creates empty/null memory
    GPUMemory() = default;

    // Delete copy operations
    GPUMemory(const GPUMemory&) = delete;
    GPUMemory& operator=(const GPUMemory&) = delete;

    // Move operations
    GPUMemory(GPUMemory&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr))
        , size_(std::exchange(other.size_, 0))
        , device_id_(std::exchange(other.device_id_, -1)) {}

    GPUMemory& operator=(GPUMemory&& other) noexcept;

    // Destructor frees GPU memory
    ~GPUMemory();

    // Release ownership without freeing (use with caution)
    void* release() noexcept {
        size_ = 0;
        device_id_ = -1;
        return std::exchange(ptr_, nullptr);
    }

    // Manually reset/free the memory
    void reset();

    // Accessors
    void* get() const noexcept { return ptr_; }
    size_t size() const noexcept { return size_; }
    int device_id() const noexcept { return device_id_; }

    // Check if valid
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

// GPU Memory Manager: device memory allocation through one GPU runtime,
// chosen at build time by SIMPLE_S3_RDMA_GPU:
//   CUDA - GPUMemoryManager.cpp, the CUDA driver API. Links libcuda.so via the
//          stubs library at build time, needs libcuda.so.1 (NVIDIA driver) at runtime.
//   HIP  - GPUMemoryHip.cpp, the HIP runtime (libamdhip64, AMD GPUs).
//   OFF  - GPUMemoryStub.cpp, where every operation fails with GPU_NOT_AVAILABLE.
//
// If the runtime cannot initialize, every GPU operation fails cleanly.
class GPUMemoryManager {
private:
    static bool runtime_initialized;
    static bool runtime_available;

    // Initialize the GPU runtime. Called once on the first GPU operation.
    static std::expected<void, GPUError> ensure_init();

public:
    // The GPU runtime this build uses: "CUDA", "HIP" or "none"
    static const char* backend_name();

    // Enumerate all available GPU devices
    // Returns a vector of device info on success, or an error
    static std::expected<std::vector<GPUDeviceInfo>, GPUError> enumerate_devices();

    // Print GPU device information
    static void print_device_info(const GPUDeviceInfo& info);

    // Allocate GPU memory (cuMemAlloc or hipMalloc) and make the device current
    // on the calling thread. With CUDA this retains the device's primary context,
    // which the returned GPUMemory releases.
    // Returns GPUMemory RAII wrapper on success, or an error
    static std::expected<GPUMemory, GPUError> allocate_gpu_memory(int device_id, size_t size);

    // Free GPU memory
    // Note: Generally you should use GPUMemory RAII wrapper instead of calling this directly
    // Returns success or an error
    static std::expected<void, GPUError> free_gpu_memory(void* ptr);

    // Make a GPU device current on the calling thread (the CUDA primary context,
    // or hipSetDevice). Copies and cuFile/cuObjClient operations need it.
    static std::expected<void, GPUError> set_device_context(int device_id);

    // Check if a device ID is valid
    // Returns true on success, or an error
    static std::expected<bool, GPUError> is_valid_device(int device_id);

    // Copy between host and device memory. The device must be current on the
    // calling thread (see set_device_context).
    static std::expected<void, GPUError> copy_to_device(void* dst, const void* src, size_t size);
    static std::expected<void, GPUError> copy_to_host(void* dst, const void* src, size_t size);

    // Zero device memory. Same context requirement as the copies.
    static std::expected<void, GPUError> clear(void* dst, size_t size);

    // Export [ptr, ptr + size) of device memory as a dmabuf, for ibv_reg_dmabuf_mr.
    // The dmabuf can cover a larger allocation that ptr was carved out of; ptr
    // starts at DmabufExport::offset within it. Release it with close_dmabuf.
    static std::expected<DmabufExport, GPUError> export_dmabuf(void* ptr, size_t size);
    static void close_dmabuf(int fd);
};

#endif // GPU_MEMORY_MANAGER_H
