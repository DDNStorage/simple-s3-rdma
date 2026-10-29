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

#ifndef CUOBJ_TRANSPORT_H
#define CUOBJ_TRANSPORT_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <fstream>
#include <deque>
#include <vector>
#include <cstdlib>

#include <spdlog/spdlog.h>
#include <cuobjclient.h>
#include "GPUMemoryManager.h"

#include "RDMATransport.h"

// NVIDIA cuObject flavor: RDMA via cuObjClient (RDMA DC protocol).
// The token travels in the x-amz-rdma-token header of the regular
// S3 PUT/GET request.
class CuObjTransport : public RDMATransport {
private:
    std::unique_ptr<cuObjClient> client_;
    bool enabled_;

    // Track multiple registered buffers (thread-safe)
    struct BufferInfo {
        size_t size;
        int gpu_device_id;  // -1 for system memory, >= 0 for GPU memory
    };
    std::unordered_map<void*, BufferInfo> registered_buffers_;
    std::mutex buffers_mutex_;

    // Check if nvidia-peermem kernel module is loaded
    static bool is_nvidia_peermem_loaded() {
        std::ifstream modules("/proc/modules");
        if (!modules.is_open()) {
            return false;
        }

        std::string line;
        while (std::getline(modules, line)) {
            if (line.find("nvidia_peermem") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // Check cuFile log for RDMA-related errors
    static std::string get_rdma_diagnostics() {
        std::string home = getenv("HOME") ? getenv("HOME") : "";
        if (home.empty()) {
            return "";
        }

        std::string logfile = home + "/cufile.log";
        std::ifstream log(logfile);
        if (!log.is_open()) {
            return "";
        }

        // Only the tail matters: cuFile appends, so the newest entries describe the
        // failure we are diagnosing.
        constexpr size_t kDiagnosticLogTailLines = 100;
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(log, line)) {
            lines.push_back(line);
            if (lines.size() > kDiagnosticLogTailLines) {
                lines.pop_front();
            }
        }

        // Look for specific error patterns
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->find("nvidia_peermem.ko is not loaded") != std::string::npos) {
                return "nvidia-peermem kernel module is not loaded";
            }
            if (it->find("RDMA registration failed") != std::string::npos &&
                it->find("errno:Bad address") != std::string::npos) {
                return "RDMA registration failed with Bad address - GPU memory not RDMA-capable";
            }
        }

        return "";
    }

    // Context for cuObjClient callbacks
    struct RDMAContext {
        void* buffer;
        size_t size;
        std::string rdma_token;
        bool token_received = false;
    };

    // Callback for PUT operations - receives RDMA descriptor
    static ssize_t put_callback(const void* handle, [[maybe_unused]] const char* ptr, size_t size,
                                [[maybe_unused]] loff_t offset, const cufileRDMAInfo_t* rdma_info) {
        auto* ctx = static_cast<RDMAContext*>(const_cast<void*>(handle));
        if (rdma_info != nullptr && rdma_info->desc_len > 0) {
            ctx->rdma_token = std::string(rdma_info->desc_str, rdma_info->desc_len);
            ctx->token_received = true;
            spdlog::debug("RDMA PUT callback: received token length={}", rdma_info->desc_len);
        }
        return static_cast<ssize_t>(size);
    }

    // Callback for GET operations - receives RDMA descriptor
    static ssize_t get_callback(const void* handle, [[maybe_unused]] char* ptr, size_t size,
                                [[maybe_unused]] loff_t offset, const cufileRDMAInfo_t* rdma_info) {
        auto* ctx = static_cast<RDMAContext*>(const_cast<void*>(handle));
        if (rdma_info != nullptr && rdma_info->desc_len > 0) {
            ctx->rdma_token = std::string(rdma_info->desc_str, rdma_info->desc_len);
            ctx->token_received = true;
            spdlog::debug("RDMA GET callback: received token length={}", rdma_info->desc_len);
        }
        return static_cast<ssize_t>(size);
    }

public:
    CuObjTransport() : enabled_(false) {}

    ~CuObjTransport() override {
        deregister_all();
    }

    // Must run before the registered memory is freed: cuFile flags a buffer
    // freed while still registered as an error.
    void deregister_all() override {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        if (client_) {
            for (const auto& [buffer, info] : registered_buffers_) {
                client_->cuMemObjPutDescriptor(buffer);
            }
        }
        registered_buffers_.clear();
    }

    [[nodiscard]] const char* name() const override { return "cuobject"; }

    bool initialize() override {
        try {
            // Initialize cuFile driver first (required for cuMemObjGetDescriptor)
            CUfileError_t status = cuFileDriverOpen();
            if (status.err != CU_FILE_SUCCESS) {
                spdlog::warn("cuFileDriverOpen failed: {} (code {}). Attempting to continue...",
                           CUFILE_ERRSTR(status.err), static_cast<int>(status.err));
                // Don't fail here - cuFile may initialize lazily
            } else {
                spdlog::debug("cuFile driver initialized successfully");
            }

            // GPU memory can be registered through dma-buf (inbox drivers) or
            // nvidia-peermem (MLNX_OFED/DOCA-OFED only); rdma_peer_type picks one
            if (is_nvidia_peermem_loaded()) {
                spdlog::info("nvidia-peermem kernel module is loaded");
            } else {
                spdlog::info("nvidia-peermem kernel module is not loaded; GPU memory needs "
                             "rdma_peer_type \"dmabuf\" in the cuFile JSON");
            }

            // Set up cuObjClient callbacks
            CUObjOps_t ops{};
            ops.put = put_callback;
            ops.get = get_callback;

            // Create cuObjClient with RDMA DC protocol
            client_ = std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1);

            if (!client_->isConnected()) {
                spdlog::warn("cuObjClient created but not connected to RDMA server");
                enabled_ = false;
                return false;
            }

            enabled_ = true;
            spdlog::info("RDMA initialized successfully via cuObjClient");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Failed to initialize RDMA: {}", e.what());
            enabled_ = false;
            return false;
        }
    }

    [[nodiscard]] bool is_enabled() const override {
        return enabled_ && client_ && client_->isConnected();
    }

    // Register buffer for RDMA operations (thread-safe)
    // NOTE: cuObjClient is NOT thread-safe, so we serialize all registration calls
    // gpu_device_id: -1 for system memory, >= 0 for GPU memory on specified device
    bool register_buffer(void* buffer, size_t size, int gpu_device_id) override {
        if (!is_enabled()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(buffers_mutex_);

        // Check if already registered
        if (registered_buffers_.find(buffer) != registered_buffers_.end()) {
            spdlog::debug("Buffer {} already registered for RDMA", buffer);
            return true;
        }

        spdlog::info("Registering buffer for RDMA: buffer={}, size={}, gpu_device={}",
                    buffer, size, gpu_device_id);

        // For GPU memory, we need to ensure the CUDA context is current
        if (gpu_device_id >= 0) {
            auto ctx_result = GPUMemoryManager::set_device_context(gpu_device_id);
            if (!ctx_result) {
                spdlog::error("Failed to set CUDA context for GPU device {}: {}",
                            gpu_device_id, gpu_error_to_string(ctx_result.error()));
                return false;
            }
        }

        // cuObjClient is NOT thread-safe - must be called with mutex held
        cuObjErr_t err = client_->cuMemObjGetDescriptor(buffer, size);
        if (err != CU_OBJ_SUCCESS) {
            spdlog::error("cuMemObjGetDescriptor failed for buffer {}: error code {}",
                         buffer, static_cast<int>(err));

            // Provide detailed diagnostics for GPU memory registration failures
            if (gpu_device_id >= 0) {
                spdlog::error("GPU memory RDMA registration failed. Possible causes:");
                spdlog::error("  - Not enough free BAR1 space to map the buffer for the NIC: all "
                              "registered GPU buffers (threads x object size) must fit in BAR1 "
                              "(see nvidia-smi -q -d MEMORY)");
                if (!is_nvidia_peermem_loaded()) {
                    spdlog::error("  - rdma_peer_type is \"nvidia_peermem\" but the module is not "
                                  "loaded; it needs MLNX_OFED/DOCA-OFED, use \"dmabuf\" with inbox drivers");
                }
                spdlog::error("  - rdma_peer_type in the cuFile JSON is not supported by this driver stack");

                // Check cuFile logs for more details
                std::string diagnostics = get_rdma_diagnostics();
                if (!diagnostics.empty()) {
                    spdlog::error("cuFile diagnostic: {}", diagnostics);
                }

                spdlog::error("Alternative: use system memory instead of GPU memory (remove --gpu flag)");
            }

            return false;
        }

        registered_buffers_[buffer] = BufferInfo{size, gpu_device_id};
        spdlog::info("Buffer registered successfully for RDMA");
        return true;
    }

    // Generate RDMA token for PUT operation (thread-safe)
    std::string generate_put_token(void* buffer, size_t size) {
        if (!is_enabled()) {
            return "";
        }

        {
            std::lock_guard<std::mutex> lock(buffers_mutex_);
            auto it = registered_buffers_.find(buffer);
            if (it == registered_buffers_.end()) {
                spdlog::error("Buffer {} not registered for RDMA", buffer);
                return "";
            }

            if (size > it->second.size) {
                spdlog::error("Requested size {} exceeds registered size {}", size, it->second.size);
                return "";
            }
        }

        spdlog::debug("Generating RDMA PUT token: buffer={}, size={}", buffer, size);

        // Get RDMA token
        char* desc_str = nullptr;
        cuObjErr_t err = client_->cuMemObjGetRDMAToken(buffer, size, 0, CUOBJ_PUT, &desc_str);
        if (err != CU_OBJ_SUCCESS || desc_str == nullptr) {
            spdlog::warn("cuMemObjGetRDMAToken failed for PUT: {}", static_cast<int>(err));
            return "";
        }

        std::string token(desc_str);
        client_->cuMemObjPutRDMAToken(desc_str);

        spdlog::debug("Generated RDMA PUT token: {} bytes", token.size());
        return token;
    }

    // Generate RDMA token for GET operation
    std::string generate_get_token(void* buffer, size_t size);

    std::unique_ptr<RDMASession> create_session(void* buffer, size_t size) override;
};

#endif // CUOBJ_TRANSPORT_H

