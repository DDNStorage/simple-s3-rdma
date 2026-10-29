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

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "Common.h"

// One object transfer handed to an RDMA session by BenchmarkThread.
// The curl handle already carries the options every request shares
// (SigV4 credentials, TLS and socket settings, load-balance resolve
// list); the session sets the URL, method, headers and callbacks.
struct RDMARequest {
    CURL* curl = nullptr;
    spdlog::logger* logger = nullptr;        // the calling thread's logger
    bool is_put = false;
    std::string endpoint;                    // e.g. http://host:port
    std::string bucket;
    std::string key;
    size_t size = 0;
    std::vector<std::string> extra_headers;  // e.g. the X-Ray trace header
    XferCtx body{};                          // where a response body goes (GPU staging included)
};

enum class RDMAStatus {
    Ok,        // transfer completed over RDMA
    Failed,    // transfer failed
    Fallback   // no RDMA attempted; caller should use plain TCP
};

struct RDMAResult {
    RDMAStatus status = RDMAStatus::Failed;
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;                          // status of the final request
    std::chrono::microseconds setup_time{0};     // token/session setup
    std::chrono::microseconds transfer_time{0};  // data movement
    std::string etag;                            // object ETag, when the server reports one
    size_t discarded = 0;                        // response body bytes that did not fit the buffer
};

// Per-thread transfer state. Not thread-safe; each BenchmarkThread
// owns one.
class RDMASession {
public:
    virtual ~RDMASession() = default;
    virtual RDMAResult execute(const RDMARequest& req) = 0;
};

// An RDMA flavor (NVIDIA cuObject, AMD hipObject RC, ...).
class RDMATransport {
public:
    virtual ~RDMATransport() = default;

    [[nodiscard]] virtual const char* name() const = 0;
    virtual bool initialize() = 0;
    [[nodiscard]] virtual bool is_enabled() const = 0;

    // Register a transfer buffer. Called serially from the main
    // thread before the benchmark threads start. gpu_device_id is -1
    // for host memory, or the CUDA device that owns the buffer.
    virtual bool register_buffer(void* buffer, size_t size, int gpu_device_id) = 0;

    // Release every registration. Called before the buffers are freed.
    virtual void deregister_all() = 0;

    // Create the per-thread session for a registered buffer. Called
    // from each BenchmarkThread's init(), possibly concurrently.
    virtual std::unique_ptr<RDMASession> create_session(void* buffer, size_t size) = 0;
};

// Build the transport for config.flavor, or nullptr (with an error
// logged) when that flavor was not compiled in.
std::unique_ptr<RDMATransport> create_rdma_transport(const Config& config);
