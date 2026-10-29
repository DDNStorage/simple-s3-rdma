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

#include "CuObjTransport.h"
#include "Common.h"

#include <chrono>

std::string CuObjTransport::generate_get_token(void* buffer, size_t size) {
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

    spdlog::debug("Generating RDMA GET token: buffer={}, size={}", buffer, size);

    // Get RDMA token
    char* desc_str = nullptr;
    cuObjErr_t err = client_->cuMemObjGetRDMAToken(buffer, size, 0, CUOBJ_GET, &desc_str);
    if (err != CU_OBJ_SUCCESS || desc_str == nullptr) {
        spdlog::warn("cuMemObjGetRDMAToken failed for GET: {}", static_cast<int>(err));
        return "";
    }

    std::string token(desc_str);
    client_->cuMemObjPutRDMAToken(desc_str);

    spdlog::debug("Generated RDMA GET token: {} bytes", token.size());
    return token;
}


namespace {

// One S3 request carrying the cuObject token; the server moves the
// data over RDMA and answers with x-amz-rdma-reply.
class CuObjSession : public RDMASession {
public:
    CuObjSession(CuObjTransport& transport, void* buffer)
        : transport_(transport), buffer_(buffer) {}

    RDMAResult execute(const RDMARequest& req) override {
        RDMAResult result;
        spdlog::logger& log = *req.logger;
        auto setup_start = std::chrono::high_resolution_clock::now();

        std::string rdma_token = req.is_put
            ? transport_.generate_put_token(buffer_, req.size)
            : transport_.generate_get_token(buffer_, req.size);
        if (rdma_token.empty()) {
            log.warn("Failed to generate RDMA token, falling back to TCP for this request");
            result.status = RDMAStatus::Fallback;
            return result;
        }

        std::string url = req.endpoint + "/" + req.bucket + "/" + req.key;
        // An RDMA GET has no body on success, so this only ever sees an error body
        XferCtx xfer_ctx = req.body;
        RDMAResponseInfo rdma_info;

        struct curl_slist* headers = nullptr;
        std::string rdma_header = "x-amz-rdma-token: " + rdma_token;
        headers = curl_slist_append(headers, rdma_header.c_str());
        log.debug("Added RDMA token header ({} bytes)", rdma_token.size());
        for (const auto& h : req.extra_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }

        curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(req.curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(req.curl, CURLOPT_HEADERDATA, &rdma_info);
        if (req.is_put) {
            headers = curl_slist_append(headers, "Expect:");
            headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
            // RDMA PUT sends an empty body (Content-Length: 0)
            curl_easy_setopt(req.curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(req.curl, CURLOPT_INFILESIZE_LARGE, 0L);
        } else {
            curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &xfer_ctx);
        }
        curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, headers);

        auto setup_end = std::chrono::high_resolution_clock::now();
        result.setup_time = std::chrono::duration_cast<std::chrono::microseconds>(setup_end - setup_start);

        result.curl_code = curl_easy_perform(req.curl);
        auto transfer_end = std::chrono::high_resolution_clock::now();
        result.transfer_time = std::chrono::duration_cast<std::chrono::microseconds>(transfer_end - setup_end);
        curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &result.http_code);

        curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headers);
        result.etag = rdma_info.etag;
        result.discarded = xfer_ctx.discarded;

        bool success = (result.curl_code == CURLE_OK && result.http_code >= 200 && result.http_code < 300);
        if (rdma_info.rdma_reply_received) {
            if (rdma_info.rdma_reply_code == 200 || rdma_info.rdma_reply_code == 204 || rdma_info.rdma_reply_code == 206) {
                log.debug("RDMA transfer successful (reply: {})", rdma_info.rdma_reply_code);
                if (rdma_info.rdma_bytes_transferred > 0) {
                    log.debug("RDMA bytes transferred: {}", rdma_info.rdma_bytes_transferred);
                }
            } else if (rdma_info.rdma_reply_code == 501) {
                // Server declined RDMA - this is an error for benchmarking purposes
                log.error("Server declined RDMA (reply: 501) - RDMA not supported by server");
                success = false;
            } else {
                log.error("Unexpected RDMA reply code: {}", rdma_info.rdma_reply_code);
                success = false;
            }
        } else {
            log.error("RDMA token sent but no x-amz-rdma-reply received");
            success = false;
        }

        result.status = success ? RDMAStatus::Ok : RDMAStatus::Failed;
        return result;
    }

private:
    CuObjTransport& transport_;
    void* buffer_;
};

} // namespace

std::unique_ptr<RDMASession> CuObjTransport::create_session(void* buffer, [[maybe_unused]] size_t size) {
    return std::make_unique<CuObjSession>(*this, buffer);
}
