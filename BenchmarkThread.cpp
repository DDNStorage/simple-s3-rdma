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

#include "BenchmarkThread.h"
#include "Common.h"
#include "GPUMemoryManager.h"
#include "SimpleTracer.h"
#include "RDMATransport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>

// OpenTelemetry includes
#include "opentelemetry/trace/provider.h"

namespace trace_api = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// Hex MD5 of a byte range. S3 reports this as the ETag of a single-part object,
// so it lets us check what the server actually stored without a second GET.
std::string md5_hex(const uint8_t* data, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(data, len, digest, &digest_len, EVP_md5(), nullptr) != 1) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(digest_len) * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

// Case-insensitive check for an HTTP header name prefix. Header names are
// case-insensitive per RFC 7230, and servers differ on "ETag" vs "etag".
static bool header_is(const std::string& header, const char* name) {
    size_t i = 0;
    for (; name[i] != '\0'; ++i) {
        if (i >= header.size()) {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(header[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

// READFUNCTION for PUT
size_t read_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *ctx = static_cast<XferCtx *>(userdata);
    if (!ctx || !ctx->buf) {
        return 0;
    }

    size_t avail = size * nmemb;
    size_t n = (avail < ctx->remaining) ? avail : ctx->remaining;
    if (n == 0) {
        return 0;
    }

    if (ctx->memory_type == MemoryType::GPU && ctx->staging_buf != nullptr) {
        // Stage through host memory: GPU -> staging -> CURL's buffer. Returning less
        // than n is fine for a read callback; libcurl just calls us again until it
        // has the CURLOPT_INFILESIZE_LARGE bytes it was promised.
        size_t chunk_size = (n < ctx->staging_size) ? n : ctx->staging_size;
        if (!GPUMemoryManager::copy_to_host(ctx->staging_buf, ctx->buf + ctx->offset, chunk_size)) {
            spdlog::error("read_callback: GPU copy failed at offset {}", ctx->offset);
            // 0 would mean a clean end-of-data; ABORT reports the failure instead
            return CURL_READFUNC_ABORT;
        }
        memcpy(ptr, ctx->staging_buf, chunk_size);
        ctx->offset += chunk_size;
        ctx->remaining -= chunk_size;
        return chunk_size;
    } else {
        // System memory: direct copy
        memcpy(ptr, ctx->buf + ctx->offset, n);
        ctx->offset += n;
        ctx->remaining -= n;
        return n;
    }
}

// WRITEFUNCTION for GET
size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *ctx = static_cast<XferCtx *>(userdata);
    if (!ctx || !ctx->buf) {
        return 0;
    }

    size_t n = size * nmemb;
    if (n == 0) {
        return 0;
    }

    // Unlike a read callback, a write callback has no short-write convention:
    // libcurl treats any return value other than n as an error. So every branch
    // below must consume all of n or deliberately fail.
    if (ctx->offset + n > ctx->remaining) {
        // Does not fit. This is the normal shape of an error response, whose XML
        // body is unrelated to the object, so swallow it but record how much was
        // dropped; run_single_operation decides whether that mattered.
        ctx->discarded += n;
        return n;
    }

    if (ctx->memory_type == MemoryType::GPU && ctx->staging_buf != nullptr) {
        // Stage through host memory in staging-sized pieces until all of n has
        // landed in device memory. libcurl hands over at most CURL_MAX_WRITE_SIZE
        // per call, so today this loops once, but it no longer depends on that.
        size_t written = 0;
        while (written < n) {
            const size_t left = n - written;
            const size_t chunk_size = (left < ctx->staging_size) ? left : ctx->staging_size;
            memcpy(ctx->staging_buf, ptr + written, chunk_size);
            if (!GPUMemoryManager::copy_to_device(ctx->buf + ctx->offset + written, ctx->staging_buf, chunk_size)) {
                spdlog::error("write_callback: GPU copy failed at offset {}", ctx->offset + written);
                return 0;  // != n, so libcurl fails the transfer
            }
            written += chunk_size;
        }
        ctx->offset += n;
        return n;
    }

    // System memory: direct copy
    memcpy(ctx->buf + ctx->offset, ptr, n);
    ctx->offset += n;
    return n;
}

// Header callback to capture RDMA response headers
size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    auto *info = static_cast<RDMAResponseInfo *>(userdata);
    size_t n = size * nitems;
    std::string header(buffer, n);

    // Check for x-amz-rdma-reply header
    if (header.find("x-amz-rdma-reply:") == 0) {
        size_t colon_pos = header.find(':');
        if (colon_pos != std::string::npos) {
            std::string value = header.substr(colon_pos + 1);
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            info->rdma_reply_code = std::stoi(value);
            info->rdma_reply_received = true;
            spdlog::debug("Received x-amz-rdma-reply: {}", info->rdma_reply_code);
        }
    }

    // Check for x-amz-rdma-bytes-transferred header
    if (header.find("x-amz-rdma-bytes-transferred:") == 0) {
        size_t colon_pos = header.find(':');
        if (colon_pos != std::string::npos) {
            std::string value = header.substr(colon_pos + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            info->rdma_bytes_transferred = std::stoull(value);
            spdlog::debug("Received x-amz-rdma-bytes-transferred: {}", info->rdma_bytes_transferred);
        }
    }

    // Capture the ETag so --verify can compare it against the digest of our buffer
    if (header_is(header, "etag:")) {
        size_t colon_pos = header.find(':');
        if (colon_pos != std::string::npos) {
            std::string value = header.substr(colon_pos + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            const size_t last = value.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                value.erase(last + 1);
            }
            // Strip the quotes S3 wraps around the ETag value
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            info->etag = value;
            spdlog::debug("Received ETag: {}", info->etag);
        }
    }

    return n;
}

BenchmarkThread::BenchmarkThread(int thread_id, const Config& config, SimpleTracer& tracer, RDMATransport* rdma_transport)
    : thread_id_(thread_id)
    , config_(config)
    , tracer_(tracer)
    , rdma_transport_(rdma_transport)
    , buffer_(nullptr)
    , curl_(nullptr)
    , init_success_(false)
    , logger_(nullptr)
    , resolve_list_(nullptr)
{
    // Set thread name for logging
    thread_name_ = "thread-" + std::to_string(thread_id);

    // Create thread-specific logger
    auto default_logger = spdlog::default_logger();
    if (default_logger) {
        logger_ = default_logger->clone(thread_name_);
    }
}

BenchmarkThread::~BenchmarkThread() {
    cleanup();
}

void BenchmarkThread::cleanup() {
    rdma_session_.reset();
    if (curl_ != nullptr) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
    if (resolve_list_ != nullptr) {
        curl_slist_free_all(resolve_list_);
        resolve_list_ = nullptr;
    }
    // Note: buffer is NOT freed here - it's managed by the caller
}

void BenchmarkThread::set_buffer(uint8_t* buffer) {
    buffer_ = buffer;
}

bool BenchmarkThread::init() {
    logger_->debug("Initializing thread {}", thread_id_);

    // Buffer should already be set by caller
    if (buffer_ == nullptr) {
        logger_->error("Buffer not set - call set_buffer() before init()");
        return false;
    }

    // GPU setup. The context set here covers the payload copy below, which runs on
    // whichever thread called init(); run() binds a context again for the worker
    // thread that performs the transfers. Both are once-per-thread, not per-operation.
    if (config_.memory_type == MemoryType::GPU) {
        auto ctx_result = GPUMemoryManager::set_device_context(config_.gpu_id.value());
        if (!ctx_result) {
            logger_->error("Failed to make GPU {} current", config_.gpu_id.value());
            cleanup();
            return false;
        }

        // Staging buffer for GPU<->host copies, sized once rather than per operation.
        // libcurl moves at most CURL_MAX_WRITE_SIZE per callback, so this chunk size
        // is never what limits a transfer.
        constexpr size_t STAGING_CHUNK_SIZE = 1024 * 1024;  // 1MiB
        staging_buffer_.resize((config_.obj_size < STAGING_CHUNK_SIZE) ? config_.obj_size
                                                                       : STAGING_CHUNK_SIZE);
        logger_->debug("Allocated staging buffer: {} bytes", staging_buffer_.size());

        // --verify hashes GPU memory by first copying it back to the host
        if (config_.verify && config_.operation != "put") {
            verify_readback_.resize(config_.obj_size);
        }
    }

    // Fill buffer with random data for PUT
    if (config_.operation == "put") {
        FILE* f = fopen("/dev/urandom", "rb");
        if (f == nullptr) {
            logger_->error("Failed to open /dev/urandom");
            cleanup();
            return false;
        }

        // For GPU memory the payload is built on the host, then copied to the device
        if (config_.memory_type == MemoryType::GPU) {
            std::vector<uint8_t> payload(config_.obj_size);

            size_t done = 0;
            while (done < config_.obj_size) {
                size_t n = fread(payload.data() + done, 1, config_.obj_size - done, f);
                if (n == 0) {
                    logger_->error("Failed to read from /dev/urandom");
                    fclose(f);
                    cleanup();
                    return false;
                }
                done += n;
            }
            fclose(f);

            // Hash the payload while it is still on the host, before it makes the
            // trip through device memory and the staging path we want to check.
            if (config_.verify) {
                expected_md5_ = md5_hex(payload.data(), config_.obj_size);
            }

            if (!GPUMemoryManager::copy_to_device(buffer_, payload.data(), config_.obj_size)) {
                logger_->error("Copying the payload to GPU memory failed");
                cleanup();
                return false;
            }
            logger_->debug("Copied {} bytes to GPU memory", config_.obj_size);
        } else {
            // System memory: read directly into buffer
            size_t done = 0;
            while (done < config_.obj_size) {
                size_t n = fread(buffer_ + done, 1, config_.obj_size - done, f);
                if (n == 0) {
                    logger_->error("Failed to read from /dev/urandom");
                    fclose(f);
                    cleanup();
                    return false;
                }
                done += n;
            }
            fclose(f);

            if (config_.verify) {
                expected_md5_ = md5_hex(buffer_, config_.obj_size);
            }
        }

        if (config_.verify && expected_md5_.empty()) {
            logger_->error("--verify requested but the payload digest could not be computed");
            cleanup();
            return false;
        }
    }

    // Initialize curl handle
    curl_ = curl_easy_init();
    if (curl_ == nullptr) {
        logger_->error("Failed to initialize curl");
        cleanup();
        return false;
    }

    // Configure load balancing: assign a fixed IP to this thread based on thread_id_.
    // This is done once here so that libcurl's connection keep-alive works in our favour
    // (all operations from this thread reuse the same connection to its assigned IP).
    if (!config_.load_balance_ips.empty()) {
        // Extract hostname and port from endpoint
        std::string host;
        size_t proto_end = config_.endpoint.find("://");
        if (proto_end != std::string::npos) {
            host = config_.endpoint.substr(proto_end + 3);
        } else {
            host = config_.endpoint;
        }

        std::string hostname = host;
        std::string port;
        size_t colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            hostname = host.substr(0, colon_pos);
            port = host.substr(colon_pos + 1);
        } else {
            port = (config_.endpoint.find("https://") == 0) ? "443" : "80";
        }

        size_t ip_index = static_cast<size_t>(thread_id_) % config_.load_balance_ips.size();
        const std::string& assigned_ip = config_.load_balance_ips[ip_index];

        std::string resolve_entry = hostname + ":" + port + ":" + assigned_ip;
        resolve_list_ = curl_slist_append(resolve_list_, resolve_entry.c_str());
        curl_easy_setopt(curl_, CURLOPT_RESOLVE, resolve_list_);

        logger_->info("Load balancing: thread {} assigned to {} (IP {}/{})",
                      thread_id_, assigned_ip, ip_index + 1, config_.load_balance_ips.size());
    }

    // Per-thread RDMA state (the buffer was registered by the caller)
    if (rdma_transport_ != nullptr) {
        rdma_session_ = rdma_transport_->create_session(buffer_, config_.obj_size);
        if (!rdma_session_) {
            logger_->error("Failed to create {} RDMA session", rdma_transport_->name());
            cleanup();
            return false;
        }
    }

    init_success_ = true;
    logger_->debug("Thread {} initialized successfully", thread_id_);
    return true;
}

void BenchmarkThread::run(const nostd::shared_ptr<trace_api::Span>& parent_span) {
    if (!init_success_) {
        logger_->error("Thread {} not initialized, skipping run", thread_id_);
        return;
    }

    // The current GPU device (a CUDA context, or HIP's current device) is per-thread
    // state, and init() ran on a different thread than this one. Bind it once for this thread --
    // every GPU copy in the transfer callbacks below depends on it.
    if (config_.memory_type == MemoryType::GPU) {
        auto ctx_result = GPUMemoryManager::set_device_context(config_.gpu_id.value());
        if (!ctx_result) {
            logger_->error("Failed to make GPU {} current on worker thread",
                           config_.gpu_id.value());
            return;
        }
    }

    // Start thread-level span as child of parent
    nostd::shared_ptr<trace_api::Span> thread_span(nullptr);
    if (tracer_.should_sample()) {
        thread_span = tracer_.start_span("thread_" + std::to_string(thread_id_), parent_span);
        SimpleTracer::set_span_attribute(thread_span, "thread_id", static_cast<int64_t>(thread_id_));
        SimpleTracer::set_span_attribute(thread_span, "operation", config_.operation);
        SimpleTracer::set_span_attribute(thread_span, "count", static_cast<int64_t>(config_.count));
    }

    // Build sigv4 string
    std::string sigv4 = "aws:amz:" + config_.region + ":s3";
    std::string userpwd = config_.access_key + ":" + config_.secret_key;

    // Run operations
    for (int i = 0; i < config_.count; ++i) {
        LatencyStats stats = run_single_operation(i, sigv4, userpwd, thread_span);
        if (stats.total_time.count() > 0) {
            stats_.push_back(stats);
            total_bytes_ += config_.obj_size;
        } else {
            failed_count_++;
        }
    }

    // End thread-level span
    if (thread_span) {
        SimpleTracer::set_span_attribute(thread_span, "successful_operations", static_cast<int64_t>(stats_.size()));
        SimpleTracer::set_span_attribute(thread_span, "failed_operations", static_cast<int64_t>(failed_count_));
        SimpleTracer::set_span_attribute(thread_span, "total_bytes", static_cast<int64_t>(total_bytes_));
        SimpleTracer::set_span_status(thread_span, failed_count_ == 0);
        SimpleTracer::end_span(thread_span);
    }
}

LatencyStats BenchmarkThread::run_single_operation(int op_index, const std::string &sigv4,
                                                   const std::string &userpwd,
                                                   const nostd::shared_ptr<trace_api::Span> &parent_span) {
    // Generate object name: test_{thread}_{size}_{num}.bin
    std::array<char, 256> obj_name_buf{};
    snprintf(obj_name_buf.data(), obj_name_buf.size(), "test_%d_%zu_%d.bin",
             thread_id_, config_.obj_size, op_index);
    std::string obj_name(obj_name_buf.data());

    std::string url = config_.endpoint + "/" + config_.bucket_name + "/" + obj_name;

    logger_->debug("Operation {}/{}: {} {}", op_index + 1, config_.count, config_.operation, obj_name);

    // Start operation span as child of thread span (with sampling)
    nostd::shared_ptr<trace_api::Span> op_span(nullptr);
    if (tracer_.should_sample()) {
        op_span = tracer_.start_span(config_.operation + "_object", parent_span);
        SimpleTracer::set_span_attribute(op_span, "object_name", obj_name);
        SimpleTracer::set_span_attribute(op_span, "object_index", static_cast<int64_t>(op_index));
        SimpleTracer::set_span_attribute(op_span, "thread_id", static_cast<int64_t>(thread_id_));
        SimpleTracer::set_span_attribute(op_span, "url", url);
    }

    // --verify: start each GET from a cleared buffer so stale data from
    // an earlier operation cannot pass the check
    if (config_.verify && config_.operation == "get") {
        if (config_.memory_type == MemoryType::GPU) {
            if (!GPUMemoryManager::clear(buffer_, config_.obj_size)) {
                logger_->error("Operation {}: clearing the GPU buffer failed", op_index + 1);
                if (op_span) {
                    SimpleTracer::set_span_status(op_span, false);
                    SimpleTracer::end_span(op_span);
                }
                return LatencyStats{};
            }
        } else {
            memset(buffer_, 0, config_.obj_size);
        }
    }

    LatencyStats stats;
    auto op_start = std::chrono::high_resolution_clock::now();

    // The GPU device and staging buffer are established once per thread in init(),
    // so nothing GPU-specific needs doing here.

    // Set common options
    curl_easy_setopt(curl_, CURLOPT_AWS_SIGV4, sigv4.c_str());
    curl_easy_setopt(curl_, CURLOPT_USERPWD, userpwd.c_str());
    curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);

    // Enable verbose output for TRACE level
    if (logger_->level() == spdlog::level::trace) {
        curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);
    }

    // Add X-Ray trace ID header if we have a span
    std::vector<std::string> extra_headers;
    std::string xray_trace_id = SimpleTracer::get_xray_trace_id(op_span);
    if (!xray_trace_id.empty()) {
        extra_headers.push_back("x-amzn-trace-id: " + xray_trace_id);
        logger_->debug("Added trace header: {}", extra_headers.back());
    }

    CURLcode res = CURLE_OK;
    long http_code = 0;
    bool success = false;
    bool use_tcp = true;
    std::string etag;
    size_t discarded = 0;

    if (rdma_session_) {
        RDMARequest req;
        req.curl = curl_;
        req.logger = logger_.get();
        req.is_put = (config_.operation == "put");
        req.endpoint = config_.endpoint;
        req.bucket = config_.bucket_name;
        req.key = obj_name;
        req.size = config_.obj_size;
        req.extra_headers = extra_headers;
        req.body = make_xfer_ctx();

        RDMAResult result = rdma_session_->execute(req);
        if (result.status != RDMAStatus::Fallback) {
            use_tcp = false;
            res = result.curl_code;
            http_code = result.http_code;
            success = (result.status == RDMAStatus::Ok);
            stats.setup_time = result.setup_time;
            stats.transfer_time = result.transfer_time;
            etag = result.etag;
            discarded = result.discarded;
        }
    }

    if (use_tcp) {
        XferCtx xfer_ctx = make_xfer_ctx();
        RDMAResponseInfo response;
        res = run_tcp_transfer(url, extra_headers, stats, http_code, xfer_ctx, response);
        success = (res == CURLE_OK && http_code >= 200 && http_code < 300);
        etag = response.etag;
        discarded = xfer_ctx.discarded;
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    stats.total_time = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start);
    stats.completion_time = op_end;  // Record when the operation completed

    // A discarded response body means what arrived did not fit the buffer we sized
    // for this object. On a failed request that is just the error XML; on a
    // successful one it means the object is not the size we asked for.
    if (discarded > 0) {
        if (success) {
            logger_->error("Operation {}: {} bytes of response did not fit the {}-byte buffer",
                           op_index + 1, discarded, config_.obj_size);
            success = false;
        } else {
            logger_->debug("Operation {}: discarded {} bytes of error response body",
                           op_index + 1, discarded);
        }
    }

    // Content verification against the ETag the server reports.
    // The ETag depends on how the object was uploaded. Infinia always gives an
    // object written over RDMA a UUID rather than an MD5, deliberately, so that
    // computing a digest does not serialize the transfer; an object written over
    // TCP gets an MD5 unless its dataset is configured to use UUIDs. So PUTs that
    // actually went over RDMA are skipped, and their payload digest is logged
    // instead, so it can be checked against a later download. A PUT the server sent
    // back to TCP is a TCP upload and is verified. GETs, over RDMA or TCP, are
    // checked against the stored object's ETag, so they pass only for objects
    // written over TCP to a dataset that uses MD5 ETags.
    if (success && config_.verify) {
        if (!use_tcp && config_.operation == "put") {
            verify_skipped_count_++;
            logger_->info("--verify: {} was written over RDMA, whose ETag {} is not a content "
                          "digest; payload MD5 {}", obj_name, etag, expected_md5_);
        } else if (!verify_against_etag(etag, obj_name)) {
            success = false;
            verify_failed_count_++;
        }
    }

    // End span
    if (op_span) {
        SimpleTracer::set_span_attribute(op_span, "http_code", static_cast<int64_t>(http_code));
        SimpleTracer::set_span_attribute(op_span, "success", static_cast<int64_t>(success ? 1 : 0));
        SimpleTracer::end_span(op_span);
    }

    if (!success) {
        logger_->error("Operation {} failed: curl={} http={}", op_index + 1, curl_easy_strerror(res), http_code);
        return LatencyStats{}; // Return empty stats to indicate failure
    }

    logger_->debug("Operation {} completed successfully (status: {}, time: {:.2f} ms)",
                 op_index + 1, http_code, stats.total_time.count() / 1000.0);
    return stats;
}

XferCtx BenchmarkThread::make_xfer_ctx() {
    XferCtx xfer_ctx{};  // Zero-initialize first
    xfer_ctx.buf = buffer_;
    xfer_ctx.offset = 0;
    xfer_ctx.remaining = config_.obj_size;
    xfer_ctx.memory_type = config_.memory_type;
    xfer_ctx.staging_buf = staging_buffer_.empty() ? nullptr : staging_buffer_.data();
    xfer_ctx.staging_size = staging_buffer_.size();

    logger_->debug("XferCtx: buf={}, remaining={}, memory_type={}, staging_buf={}, staging_size={}",
                   static_cast<void*>(xfer_ctx.buf), xfer_ctx.remaining,
                   xfer_ctx.memory_type == MemoryType::GPU ? "GPU" : "SYSTEM",
                   static_cast<void*>(xfer_ctx.staging_buf), xfer_ctx.staging_size);
    return xfer_ctx;
}

CURLcode BenchmarkThread::run_tcp_transfer(const std::string& url, const std::vector<std::string>& extra_headers,
                                           LatencyStats& stats, long& http_code, XferCtx& xfer_ctx,
                                           RDMAResponseInfo& response) {
    auto setup_start = std::chrono::high_resolution_clock::now();

    struct curl_slist* headers = nullptr;

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &response);

    for (const auto& h : extra_headers) {
        headers = curl_slist_append(headers, h.c_str());
    }

    if (config_.operation == "put") {
        // TCP PUT: send data in body
        headers = curl_slist_append(headers, "Expect:");
        headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
        curl_easy_setopt(curl_, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl_, CURLOPT_INFILESIZE_LARGE, (curl_off_t)config_.obj_size);
        curl_easy_setopt(curl_, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl_, CURLOPT_READDATA, &xfer_ctx);
    } else {
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &xfer_ctx);
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    auto setup_end = std::chrono::high_resolution_clock::now();
    stats.setup_time = std::chrono::duration_cast<std::chrono::microseconds>(setup_end - setup_start);

    // Perform request
    CURLcode res = curl_easy_perform(curl_);
    auto transfer_end = std::chrono::high_resolution_clock::now();
    stats.transfer_time = std::chrono::duration_cast<std::chrono::microseconds>(transfer_end - setup_end);

    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    return res;
}

// Compare the object content against the server's ETag. For a single-part upload S3
// sets the ETag to the hex MD5 of the stored bytes, so this catches corruption
// anywhere in the path -- including the GPU staging copies -- without a second GET.
bool BenchmarkThread::verify_against_etag(const std::string& etag, const std::string& obj_name) {
    if (etag.empty()) {
        logger_->error("--verify: no ETag in the response for {}, cannot verify content", obj_name);
        return false;
    }

    // A multipart ETag is "<digest>-<partcount>" and is not a digest of the object
    if (etag.find('-') != std::string::npos) {
        logger_->warn("--verify: ETag {} for {} is multipart, skipping content check", etag, obj_name);
        return true;
    }

    // Neither is anything that is not 32 hex digits
    if (etag.size() != 32 || etag.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        logger_->warn("--verify: ETag {} for {} is not an MD5, skipping content check", etag, obj_name);
        return true;
    }
    std::string expected = etag;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string actual;
    if (config_.operation == "put") {
        // Digest of the payload, taken on the host in init() before it was staged
        // through device memory
        actual = expected_md5_;
    } else if (config_.memory_type == MemoryType::GPU) {
        if (!GPUMemoryManager::copy_to_host(verify_readback_.data(), buffer_, config_.obj_size)) {
            logger_->error("--verify: reading back GPU memory failed");
            return false;
        }
        actual = md5_hex(verify_readback_.data(), config_.obj_size);
    } else {
        actual = md5_hex(buffer_, config_.obj_size);
    }

    if (actual.empty()) {
        logger_->error("--verify: could not compute a digest for {}", obj_name);
        return false;
    }

    if (actual != expected) {
        logger_->error("--verify FAILED for {}: local digest {} does not match server ETag {}",
                       obj_name, actual, etag);
        return false;
    }

    logger_->debug("--verify passed for {}: {}", obj_name, actual);
    return true;
}
