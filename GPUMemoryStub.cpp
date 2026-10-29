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

// GPUMemoryManager for builds without a GPU runtime (SIMPLE_S3_RDMA_GPU=OFF):
// there is no GPU memory, so every operation reports GPU_NOT_AVAILABLE.
#include "GPUMemoryManager.h"

bool GPUMemoryManager::runtime_initialized = true;
bool GPUMemoryManager::runtime_available = false;

GPUMemory& GPUMemory::operator=(GPUMemory&& other) noexcept {
    if (this != &other) {
        ptr_ = std::exchange(other.ptr_, nullptr);
        size_ = std::exchange(other.size_, 0);
        device_id_ = std::exchange(other.device_id_, -1);
    }
    return *this;
}

GPUMemory::~GPUMemory() = default;

void GPUMemory::reset() {
    ptr_ = nullptr;
    size_ = 0;
    device_id_ = -1;
}

std::expected<void, GPUError> GPUMemoryManager::ensure_init() {
    spdlog::error("GPU memory support was not compiled in (SIMPLE_S3_RDMA_GPU=OFF)");
    return std::unexpected(GPUError::GPU_NOT_AVAILABLE);
}

const char* GPUMemoryManager::backend_name() {
    return "none";
}

std::expected<std::vector<GPUDeviceInfo>, GPUError> GPUMemoryManager::enumerate_devices() {
    return std::unexpected(ensure_init().error());
}

void GPUMemoryManager::print_device_info([[maybe_unused]] const GPUDeviceInfo& info) {}

std::expected<GPUMemory, GPUError> GPUMemoryManager::allocate_gpu_memory([[maybe_unused]] int device_id,
                                                                         [[maybe_unused]] size_t size) {
    return std::unexpected(ensure_init().error());
}

std::expected<void, GPUError> GPUMemoryManager::free_gpu_memory([[maybe_unused]] void* ptr) {
    return {};
}

std::expected<void, GPUError> GPUMemoryManager::set_device_context([[maybe_unused]] int device_id) {
    return std::unexpected(ensure_init().error());
}

std::expected<bool, GPUError> GPUMemoryManager::is_valid_device([[maybe_unused]] int device_id) {
    return std::unexpected(ensure_init().error());
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_device([[maybe_unused]] void* dst,
                                                               [[maybe_unused]] const void* src,
                                                               [[maybe_unused]] size_t size) {
    return std::unexpected(ensure_init().error());
}

std::expected<void, GPUError> GPUMemoryManager::copy_to_host([[maybe_unused]] void* dst,
                                                             [[maybe_unused]] const void* src,
                                                             [[maybe_unused]] size_t size) {
    return std::unexpected(ensure_init().error());
}

std::expected<void, GPUError> GPUMemoryManager::clear([[maybe_unused]] void* dst, [[maybe_unused]] size_t size) {
    return std::unexpected(ensure_init().error());
}

std::expected<DmabufExport, GPUError> GPUMemoryManager::export_dmabuf([[maybe_unused]] void* ptr,
                                                                     [[maybe_unused]] size_t size) {
    return std::unexpected(ensure_init().error());
}

void GPUMemoryManager::close_dmabuf([[maybe_unused]] int fd) {}
