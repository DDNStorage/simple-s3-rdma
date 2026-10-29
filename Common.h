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
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Protocol enum
enum class Protocol {
    TCP,
    RDMA
};

// Memory type enum
enum class MemoryType {
    SYSTEM,  // System memory (huge pages)
    GPU      // GPU memory (cuMemAlloc or hipMalloc)
};

// RDMA flavor (--flavor)
enum class RdmaFlavor {
    NVIDIA,   // cuObject (libcuobjclient)
    AMD       // hipObject hipobj-rc-v2 (libibverbs RC)
};

// Configuration
struct Config {
    std::string operation;      // "put" or "get"
    Protocol protocol = Protocol::RDMA;
    RdmaFlavor flavor = RdmaFlavor::NVIDIA;
    std::string rc_device;      // verbs device for --flavor amd (empty: first device)
    int rc_gid_index = -1;      // GID index for --flavor amd (negative: auto)
    std::string access_key;
    std::string secret_key;
    std::string endpoint;
    std::string bucket_name = "test-bucket";
    std::string region = "us-east-1";
    size_t obj_size = 0;
    int count = 1;
    int threads = 1;
    std::optional<std::chrono::microseconds> stagger_start;
    std::optional<std::string> trace_output;
    double trace_sampling = 1.0;
    std::optional<std::string> log_output;
    std::string log_level = "INFO";
    std::vector<std::string> load_balance_ips;  // IP addresses for load balancing
    std::chrono::seconds sample_interval{10};  // Sample interval for statistics aggregation (default: 10 seconds)
    std::optional<int> gpu_id;  // GPU device ID for GPU memory allocation
    MemoryType memory_type = MemoryType::SYSTEM;  // Type of memory to allocate
    bool verify = false;  // Verify object content against the server's ETag
};

// Latency statistics
struct LatencyStats {
    std::chrono::microseconds setup_time{0};
    std::chrono::microseconds transfer_time{0};
    std::chrono::microseconds total_time{0};
    std::chrono::high_resolution_clock::time_point completion_time;  // When the operation completed
};

// Transfer context for CURL callbacks
struct XferCtx {
    uint8_t* buf;          // Buffer pointer (GPU or system memory)
    size_t offset;
    size_t remaining;
    MemoryType memory_type;  // Type of memory (SYSTEM or GPU)
    uint8_t* staging_buf;  // Staging buffer for GPU memory transfers (nullptr for system memory)
    size_t staging_size;   // Size of staging buffer
    size_t discarded;      // Response bytes that did not fit in buf (usually an error body)
};

// RDMA response information
struct RDMAResponseInfo {
    bool rdma_reply_received = false;
    int rdma_reply_code = 0;
    size_t rdma_bytes_transferred = 0;
    std::string etag;  // ETag from the response, unquoted (used by --verify)
};

// Hex MD5 of a byte range, matching the ETag S3 reports for a single-part object.
// Returns an empty string if the digest could not be computed.
std::string md5_hex(const uint8_t* data, size_t len);

// Memory allocation
void* allocate_huge_pages(size_t size);

// libcurl callbacks shared by the transfer paths (BenchmarkThread.cpp)
size_t read_callback(char* ptr, size_t size, size_t nmemb, void* userdata);     // XferCtx
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);    // XferCtx
size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata); // RDMAResponseInfo

