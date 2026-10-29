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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/trace/span.h"

#include "Common.h"
#include "RDMATransport.h"

namespace trace_api = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// Forward declarations
class SimpleTracer;

class BenchmarkThread {
public:
    BenchmarkThread(int thread_id, const Config& config, SimpleTracer& tracer, RDMATransport* rdma_transport);
    ~BenchmarkThread();

    // Set pre-allocated buffer (must be called before init)
    void set_buffer(uint8_t* buffer);

    // Initialize thread resources (called before starting benchmark)
    bool init();

    // Run the benchmark
    void run(const nostd::shared_ptr<trace_api::Span>& parent_span);

    // Get statistics
    const std::vector<LatencyStats>& get_stats() const { return stats_; }
    size_t get_total_bytes() const { return total_bytes_; }
    size_t get_failed_count() const { return failed_count_; }
    size_t get_verify_failed_count() const { return verify_failed_count_; }
    size_t get_verify_skipped_count() const { return verify_skipped_count_; }
    bool is_initialized() const { return init_success_; }

private:
    void cleanup();

    LatencyStats run_single_operation(int op_index, const std::string &sigv4,
                                      const std::string &userpwd,
                                      const nostd::shared_ptr<trace_api::Span> &parent_span);

    // Transfer context for the curl body callbacks, GPU staging included
    XferCtx make_xfer_ctx();

    // Plain HTTP transfer with the object in the request/response body
    CURLcode run_tcp_transfer(const std::string& url, const std::vector<std::string>& extra_headers,
                              LatencyStats& stats, long& http_code, XferCtx& xfer_ctx,
                              RDMAResponseInfo& response);

    // --verify support. Compares the object content against the ETag the server
    // reports, which for a single-part object is the hex MD5 of the stored bytes.
    // Returns true when verification passes or is not applicable.
    bool verify_against_etag(const std::string& etag, const std::string& obj_name);

    int thread_id_;
    std::string thread_name_;
    const Config& config_;
    SimpleTracer& tracer_;
    RDMATransport* rdma_transport_;              // nullptr when using TCP
    std::unique_ptr<RDMASession> rdma_session_;
    uint8_t* buffer_;
    CURL* curl_;
    bool init_success_;
    std::shared_ptr<spdlog::logger> logger_;

    struct curl_slist* resolve_list_;  // Persistent resolve list for load balancing

    // Staging buffer for GPU transfers, allocated once in init() rather than per
    // operation. Empty when memory_type is SYSTEM.
    std::vector<uint8_t> staging_buffer_;

    // --verify state. expected_md5_ is the digest of the payload we upload (PUT);
    // verify_readback_ is a host-side landing area used to hash GPU memory (GET).
    std::string expected_md5_;
    std::vector<uint8_t> verify_readback_;

    std::vector<LatencyStats> stats_;
    size_t total_bytes_ = 0;
    size_t failed_count_ = 0;
    size_t verify_failed_count_ = 0;
    size_t verify_skipped_count_ = 0;
};

