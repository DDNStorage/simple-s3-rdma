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

#include "RcTransport.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <infiniband/verbs.h>

#include "GPUMemoryManager.h"

// hipobj-rc-v2 client.
//
// Per transfer:
//   1. PREPARE  POST /.hipobj-rc/prepare  (client token, PSN, cookie)
//               -> server token (QPN/GID/LID), session id, server PSN,
//                  and for PUT the server staging MR
//   2. pair the client QP with the server QP (INIT -> RTR -> RTS)
//   3. READY    POST /.hipobj-rc/ready    (client QPN and MR)
//               GET: the server RDMA-writes into the client MR with the
//                    cookie as immediate; the response follows the data
//               PUT: once READY is on the wire the client RDMA-writes
//                    into the staging MR with the cookie as immediate
//   4. CANCEL   POST /.hipobj-rc/cancel   on any client-side failure
//
// All requests are SigV4-signed POSTs with an empty body.

namespace {

constexpr const char* kProtocolHeader = "x-amz-rdma-protocol: hipobj-rc-v2";
constexpr uint8_t kTransportRc = 1;
constexpr size_t kTokenBytes = 1 + 4 + 16 + 4 + 8 + 8 + 1 + 2;
constexpr uint32_t kMaxPsn = 0xffffff;
constexpr int kCqDepth = 16;
constexpr int kQpDepth = 4;
constexpr uint64_t kWrRecv = 1;
constexpr uint64_t kWrWrite = 2;
// READY on a GET answers 409 RdmaSessionConflict when the server
// could not deliver (our receive was not armed); the session stays
// prepared and READY may be retried.
constexpr int kReadyAttempts = 3;
constexpr auto kReadyRetryDelay = std::chrono::milliseconds(1);
constexpr auto kCompletionTimeout = std::chrono::seconds(10);
constexpr size_t kMaxErrorBody = 8192;

// 44-byte endpoint record, hex-encoded; integers little-endian
struct RcToken {
    uint8_t transport = 0;
    uint32_t qpn = 0;
    std::array<uint8_t, 16> gid{};
    uint32_t rkey = 0;
    uint64_t remote_addr = 0;
    uint64_t length = 0;
    uint8_t port = 0;
    uint16_t lid = 0;
};

void put_le(uint8_t*& p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        *p++ = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint64_t get_le(const uint8_t*& p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(*p++) << (8 * i);
    }
    return v;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string encode_token(const RcToken& t) {
    std::array<uint8_t, kTokenBytes> buf{};
    uint8_t* p = buf.data();
    put_le(p, t.transport, 1);
    put_le(p, t.qpn, 4);
    std::memcpy(p, t.gid.data(), 16);
    p += 16;
    put_le(p, t.rkey, 4);
    put_le(p, t.remote_addr, 8);
    put_le(p, t.length, 8);
    put_le(p, t.port, 1);
    put_le(p, t.lid, 2);

    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(kTokenBytes * 2);
    for (uint8_t b : buf) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 0xf]);
    }
    return hex;
}

bool decode_token(const std::string& hex, RcToken& out, bool& all_zero) {
    if (hex.size() != kTokenBytes * 2) {
        return false;
    }
    std::array<uint8_t, kTokenBytes> buf{};
    all_zero = true;
    for (size_t i = 0; i < kTokenBytes; ++i) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        buf[i] = static_cast<uint8_t>((hi << 4) | lo);
        all_zero = all_zero && buf[i] == 0;
    }
    const uint8_t* p = buf.data();
    out.transport = static_cast<uint8_t>(get_le(p, 1));
    out.qpn = static_cast<uint32_t>(get_le(p, 4));
    std::memcpy(out.gid.data(), p, 16);
    p += 16;
    out.rkey = static_cast<uint32_t>(get_le(p, 4));
    out.remote_addr = get_le(p, 8);
    out.length = get_le(p, 8);
    out.port = static_cast<uint8_t>(get_le(p, 1));
    out.lid = static_cast<uint16_t>(get_le(p, 2));
    return true;
}

// Strict bare-hex / decimal parsers for response header values
bool parse_uint(const std::string& s, int base, uint64_t max, uint64_t& out) {
    if (s.empty() || s.size() > 20) {
        return false;
    }
    uint64_t v = 0;
    for (char c : s) {
        int d = (base == 16) ? hex_nibble(c) : ((c >= '0' && c <= '9') ? c - '0' : -1);
        if (d < 0) {
            return false;
        }
        if (v > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / static_cast<uint64_t>(base)) {
            return false;
        }
        v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
    }
    if (v > max) {
        return false;
    }
    out = v;
    return true;
}

// Percent-encode a path segment (keep_slash for object keys)
std::string uri_encode(const std::string& s, bool keep_slash) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xf]);
        }
    }
    return out;
}

std::string format_gid(const std::array<uint8_t, 16>& gid) {
    std::string s;
    for (size_t i = 0; i < 16; i += 2) {
        if (i > 0) s += ':';
        s += fmt::format("{:02x}{:02x}", gid[i], gid[i + 1]);
    }
    return s;
}

bool is_ipv4_mapped(const uint8_t* gid) {
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(gid, prefix, sizeof(prefix)) == 0;
}

// Prefer a RoCEv2 IPv4 GID, then any RoCEv2 GID; InfiniBand uses 0
int pick_gid_index(ibv_context* ctx, uint8_t port, const ibv_port_attr& pa) {
    if (pa.link_layer == IBV_LINK_LAYER_INFINIBAND) {
        return 0;
    }
    int any_v2 = -1;
    for (int i = 0; i < pa.gid_tbl_len; ++i) {
        ibv_gid_entry entry{};
        if (ibv_query_gid_ex(ctx, port, static_cast<uint32_t>(i), &entry, 0) != 0) {
            continue;
        }
        if (entry.gid_type != IBV_GID_TYPE_ROCE_V2) {
            continue;
        }
        if (is_ipv4_mapped(entry.gid.raw)) {
            return i;
        }
        if (any_v2 < 0) {
            any_v2 = i;
        }
    }
    return any_v2 >= 0 ? any_v2 : 0;
}

// Response of one control request
struct RcResponse {
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;
    std::map<std::string, std::string> headers;  // x-amz-rdma-*, lowercase names
    std::string body;

    [[nodiscard]] std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }

    // <Code> of an S3 error body, if any
    [[nodiscard]] std::string error_code() const {
        size_t b = body.find("<Code>");
        size_t e = body.find("</Code>");
        if (b == std::string::npos || e == std::string::npos || e < b) {
            return "";
        }
        return body.substr(b + 6, e - b - 6);
    }
};

size_t rc_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* resp = static_cast<RcResponse*>(userdata);
    size_t n = size * nitems;
    std::string line(buffer, n);

    // A new status line (e.g. after 100 Continue) starts a new header set
    if (line.rfind("HTTP/", 0) == 0) {
        resp->headers.clear();
        return n;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return n;
    }
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.rfind("x-amz-rdma-", 0) != 0) {
        return n;
    }
    std::string value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(" \t\r\n"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);
    resp->headers[name] = value;
    return n;
}

size_t rc_body_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* resp = static_cast<RcResponse*>(userdata);
    size_t n = size * nmemb;
    if (resp->body.size() < kMaxErrorBody) {
        resp->body.append(ptr, std::min(n, kMaxErrorBody - resp->body.size()));
    }
    return n;
}

// Fires the PUT data phase once the READY request is on the wire
struct WriteTrigger {
    std::function<bool()> fire;
    bool posted = false;
    bool failed = false;
};

struct DebugCtx {
    spdlog::logger* logger;
    WriteTrigger* trigger;
};

int rc_debug_callback([[maybe_unused]] CURL* handle, curl_infotype type, char* data, size_t size, void* userdata) {
    auto* ctx = static_cast<DebugCtx*>(userdata);
    // libcurl reports CURLINFO_HEADER_OUT after the request headers
    // have been handed to the socket
    if (type == CURLINFO_HEADER_OUT && ctx->trigger != nullptr &&
        !ctx->trigger->posted && !ctx->trigger->failed) {
        if (ctx->trigger->fire()) {
            ctx->trigger->posted = true;
        } else {
            ctx->trigger->failed = true;
        }
    }
    if (ctx->logger->level() == spdlog::level::trace &&
        (type == CURLINFO_TEXT || type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT)) {
        std::string text(data, size);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        ctx->logger->trace("curl: {}", text);
    }
    return 0;
}

int rc_xferinfo_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    // Abort READY when the RDMA write could not be posted
    return static_cast<WriteTrigger*>(userdata)->failed ? 1 : 0;
}

class RcSession : public RDMASession {
public:
    RcSession(RcTransport& transport, void* buffer, size_t size, ibv_mr* mr)
        : transport_(transport), buffer_(buffer), size_(size), mr_(mr), rng_(std::random_device{}()) {}

    ~RcSession() override {
        if (qp_ != nullptr) {
            ibv_destroy_qp(qp_);
        }
        if (cq_ != nullptr) {
            ibv_destroy_cq(cq_);
        }
    }

    bool open() {
        cq_ = ibv_create_cq(transport_.context(), kCqDepth, nullptr, nullptr, 0);
        if (cq_ == nullptr) {
            spdlog::error("ibv_create_cq failed: {}", strerror(errno));
            return false;
        }
        return true;
    }

    RDMAResult execute(const RDMARequest& req) override;

private:
    bool new_qp();
    bool connect_qp(uint32_t dest_qpn, uint32_t rq_psn, const std::array<uint8_t, 16>& dgid,
                    uint16_t dlid, uint32_t sq_psn);
    bool post_recv();
    bool post_write_imm(uint64_t remote_addr, uint32_t rkey, size_t len, uint32_t imm);
    bool wait_completion(ibv_wc_opcode opcode, uint32_t cookie, size_t len, spdlog::logger& log);
    void drain_cq();
    bool post(const RDMARequest& req, const char* route, const std::vector<std::string>& headers,
              RcResponse& resp, WriteTrigger* trigger = nullptr);
    void cancel(const RDMARequest& req, const std::string& session);

    RcTransport& transport_;
    void* buffer_;
    size_t size_;
    ibv_mr* mr_;
    ibv_cq* cq_ = nullptr;
    ibv_qp* qp_ = nullptr;
    std::mt19937 rng_;
};

// Every transfer pairs with a new server QP, so it gets a new local QP
// too. Recycling one QP through RESET is legal verbs, but on ionic
// (AMD Pollara) a QP brought back that way intermittently never
// transmits the work requests posted on it: the write sits in the send
// queue with the PSN unchanged and the transfer hangs.
bool RcSession::new_qp() {
    if (qp_ != nullptr) {
        ibv_destroy_qp(qp_);
        qp_ = nullptr;
    }
    ibv_qp_init_attr init{};
    init.qp_type = IBV_QPT_RC;
    init.send_cq = cq_;
    init.recv_cq = cq_;
    init.cap.max_send_wr = kQpDepth;
    init.cap.max_recv_wr = kQpDepth;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    qp_ = ibv_create_qp(transport_.pd(), &init);
    if (qp_ == nullptr) {
        return false;
    }
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = transport_.port().num;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    return ibv_modify_qp(qp_, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) == 0;
}

bool RcSession::connect_qp(uint32_t dest_qpn, uint32_t rq_psn, const std::array<uint8_t, 16>& dgid,
                           uint16_t dlid, uint32_t sq_psn) {
    const RcTransport::Port& port = transport_.port();
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = static_cast<ibv_mtu>(std::clamp(port.active_mtu, static_cast<int>(IBV_MTU_256),
                                                    static_cast<int>(IBV_MTU_4096)));
    attr.dest_qp_num = dest_qpn;
    attr.rq_psn = rq_psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.dlid = dlid;  // routes on InfiniBand; unused on RoCE
    std::memcpy(attr.ah_attr.grh.dgid.raw, dgid.data(), 16);
    attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(port.gid_index);
    attr.ah_attr.grh.hop_limit = 64;
    attr.ah_attr.port_num = port.num;
    if (ibv_modify_qp(qp_, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
        spdlog::error("QP RTR transition failed: {}", strerror(errno));
        return false;
    }
    attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = sq_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(qp_, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
        spdlog::error("QP RTS transition failed: {}", strerror(errno));
        return false;
    }
    return true;
}

bool RcSession::post_recv() {
    // Zero-SGE receive: consumed by the server's WRITE_WITH_IMM
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = kWrRecv;
    return ibv_post_recv(qp_, &wr, &bad) == 0;
}

bool RcSession::post_write_imm(uint64_t remote_addr, uint32_t rkey, size_t len, uint32_t imm) {
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uintptr_t>(buffer_);
    sge.length = static_cast<uint32_t>(len);
    sge.lkey = mr_->lkey;
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = kWrWrite;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(imm);
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    return ibv_post_send(qp_, &wr, &bad) == 0;
}

bool RcSession::wait_completion(ibv_wc_opcode opcode, uint32_t cookie, size_t len, spdlog::logger& log) {
    auto deadline = std::chrono::steady_clock::now() + kCompletionTimeout;
    while (true) {
        ibv_wc wc{};
        int n = ibv_poll_cq(cq_, 1, &wc);
        if (n < 0) {
            log.error("ibv_poll_cq failed");
            return false;
        }
        if (n == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                log.error("Timed out waiting for the RDMA completion");
                return false;
            }
            std::this_thread::yield();
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            log.error("RDMA completion failed: {} (wr_id {})", ibv_wc_status_str(wc.status), wc.wr_id);
            return false;
        }
        if (wc.opcode != opcode) {
            log.debug("Ignoring RDMA completion with opcode {}", static_cast<int>(wc.opcode));
            continue;
        }
        if (opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            if ((wc.wc_flags & IBV_WC_WITH_IMM) == 0 || ntohl(wc.imm_data) != cookie) {
                log.error("RDMA receive carries the wrong immediate (expected cookie {:08X})", cookie);
                return false;
            }
            // The receive reports only the write that carried the
            // immediate. A server may stream the object in several writes
            // and signal with the last one, so this can be less than the
            // object; READY's x-amz-rdma-bytes-transferred gives the total.
            if (wc.byte_len == 0 || wc.byte_len > len) {
                log.error("RDMA receive length {} is not within the object size {}", wc.byte_len, len);
                return false;
            }
        }
        return true;
    }
}

void RcSession::drain_cq() {
    ibv_wc wc{};
    while (ibv_poll_cq(cq_, 1, &wc) > 0) {
    }
}

bool RcSession::post(const RDMARequest& req, const char* route, const std::vector<std::string>& headers,
                     RcResponse& resp, WriteTrigger* trigger) {
    std::string url = req.endpoint + "/.hipobj-rc/" + route;
    struct curl_slist* list = nullptr;
    for (const auto& h : req.extra_headers) {
        list = curl_slist_append(list, h.c_str());
    }
    list = curl_slist_append(list, kProtocolHeader);
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }
    list = curl_slist_append(list, "Expect:");

    CURL* curl = req.curl;
    bool trace = req.logger->level() == spdlog::level::trace;
    DebugCtx debug{req.logger, trigger};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, rc_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rc_body_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    if (trigger != nullptr || trace) {
        // The debug callback only runs with VERBOSE set
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, rc_debug_callback);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &debug);
    }
    if (trigger != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, rc_xferinfo_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, trigger);
    }

    resp.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, trace ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nullptr);
    curl_slist_free_all(list);

    if (resp.curl_code != CURLE_OK) {
        req.logger->error("hipobj-rc {} request failed: {}", route, curl_easy_strerror(resp.curl_code));
        return false;
    }
    if (resp.http_code != 200) {
        std::string code = resp.error_code();
        req.logger->error("hipobj-rc {} returned HTTP {}{}", route, resp.http_code,
                          code.empty() ? "" : " (" + code + ")");
        return false;
    }
    return true;
}

void RcSession::cancel(const RDMARequest& req, const std::string& session) {
    if (session.empty()) {
        return;
    }
    RcResponse resp;
    if (post(req, "cancel", {"x-amz-rdma-session: " + session}, resp)) {
        req.logger->debug("Cancelled hipobj-rc session {}", session);
    }
}

RDMAResult RcSession::execute(const RDMARequest& req) {
    RDMAResult result;
    spdlog::logger& log = *req.logger;
    auto setup_start = std::chrono::high_resolution_clock::now();
    const RcTransport::Port& port = transport_.port();

    if (req.size > size_ || req.size > port.max_msg_size || req.size > std::numeric_limits<uint32_t>::max()) {
        log.error("Object size {} exceeds the RC message limit ({} bytes)", req.size,
                  std::min<uint64_t>(port.max_msg_size, size_));
        return result;
    }

    drain_cq();
    if (!new_qp()) {
        log.error("Failed to create the RC QP: {}", strerror(errno));
        return result;
    }
    drain_cq();

    uint32_t cookie = std::uniform_int_distribution<uint32_t>(1, std::numeric_limits<uint32_t>::max())(rng_);
    uint32_t client_psn = std::uniform_int_distribution<uint32_t>(1, kMaxPsn)(rng_);

    RcToken token;
    token.transport = kTransportRc;
    token.qpn = qp_->qp_num;
    token.gid = port.gid;
    token.rkey = mr_->rkey;
    token.remote_addr = reinterpret_cast<uintptr_t>(buffer_);
    token.length = req.size;
    token.port = port.num;
    token.lid = port.lid;

    // Phase 1: PREPARE
    RcResponse prep;
    bool ok = post(req, "prepare",
                   {
                       std::string("x-amz-rdma-op: ") + (req.is_put ? "PUT" : "GET"),
                       "x-amz-rdma-target: /" + uri_encode(req.bucket, false) + "/" + uri_encode(req.key, true),
                       "x-amz-rdma-size: " + std::to_string(req.size),
                       "x-amz-rdma-offset: 0",
                       fmt::format("x-amz-rdma-psn: {:06X}", client_psn),
                       fmt::format("x-amz-rdma-cookie: {:08X}", cookie),
                       "x-amz-rdma-token: " + encode_token(token),
                   },
                   prep);
    result.curl_code = prep.curl_code;
    result.http_code = prep.http_code;
    if (!ok) {
        return result;
    }

    std::string session = prep.header("x-amz-rdma-session");
    std::string reply = prep.header("x-amz-rdma-reply");
    RcToken server;
    bool loopback = false;
    uint64_t server_psn = 0;
    uint64_t server_qpn = 0;
    uint64_t staging_addr = 0;
    uint64_t staging_rkey = 0;
    if (session.empty()) {
        log.error("PREPARE response carries no x-amz-rdma-session");
        return result;
    }
    if (reply.rfind("200:", 0) != 0 || !decode_token(reply.substr(4), server, loopback) ||
        (!loopback && server.transport != kTransportRc)) {
        log.error("PREPARE response carries an invalid x-amz-rdma-reply: '{}'", reply);
        cancel(req, session);
        return result;
    }
    if (!parse_uint(prep.header("x-amz-rdma-psn"), 16, kMaxPsn, server_psn)) {
        log.error("PREPARE response carries an invalid x-amz-rdma-psn: '{}'", prep.header("x-amz-rdma-psn"));
        cancel(req, session);
        return result;
    }
    if (!parse_uint(prep.header("x-amz-rdma-qpn"), 16, 0xffffff, server_qpn)) {
        server_qpn = server.qpn;
    }
    if (server_qpn == 0) {
        log.error("PREPARE response names no server QP");
        cancel(req, session);
        return result;
    }
    if (req.is_put &&
        (!parse_uint(prep.header("x-amz-rdma-mr-addr"), 16, std::numeric_limits<uint64_t>::max(), staging_addr) ||
         !parse_uint(prep.header("x-amz-rdma-mr-rkey"), 16, std::numeric_limits<uint32_t>::max(), staging_rkey))) {
        log.error("PUT PREPARE response carries no valid staging MR (addr '{}', rkey '{}')",
                  prep.header("x-amz-rdma-mr-addr"), prep.header("x-amz-rdma-mr-rkey"));
        cancel(req, session);
        return result;
    }

    // Phase 2: pair with the server QP. An all-zero reply token means
    // the server shares our HCA, so its GID and LID are ours.
    std::array<uint8_t, 16> dgid = loopback ? port.gid : server.gid;
    uint16_t dlid = loopback ? port.lid : server.lid;
    log.debug("hipobj-rc session {}: server qpn {:x} psn {:06X} gid {}", session, server_qpn, server_psn,
              format_gid(dgid));
    if (!connect_qp(static_cast<uint32_t>(server_qpn), static_cast<uint32_t>(server_psn), dgid, dlid,
                    client_psn)) {
        cancel(req, session);
        return result;
    }

    auto setup_end = std::chrono::high_resolution_clock::now();
    result.setup_time = std::chrono::duration_cast<std::chrono::microseconds>(setup_end - setup_start);

    // Phase 3: READY and the data phase
    std::vector<std::string> ready_headers = {
        "x-amz-rdma-session: " + session,
        fmt::format("x-amz-rdma-cookie: {:08X}", cookie),
        fmt::format("x-amz-rdma-qpn: {:x}", qp_->qp_num),
        fmt::format("x-amz-rdma-mr-addr: {:x}", reinterpret_cast<uintptr_t>(buffer_)),
        fmt::format("x-amz-rdma-mr-rkey: {:x}", mr_->rkey),
    };
    RcResponse ready;
    WriteTrigger trigger;
    if (req.is_put) {
        // The server posts its receive while handling READY; RNR
        // retry covers a write that arrives first
        trigger.fire = [&]() {
            return post_write_imm(staging_addr, static_cast<uint32_t>(staging_rkey), req.size, cookie);
        };
        ok = post(req, "ready", ready_headers, ready, &trigger);
        if (trigger.failed) {
            log.error("Failed to post the RDMA write: {}", strerror(errno));
        }
        log.debug("hipobj-rc session {}: RDMA write {}", session,
                  trigger.posted ? "posted" : (trigger.failed ? "failed to post" : "never triggered"));
    } else {
        // The server's WRITE_WITH_IMM consumes this receive
        if (!post_recv()) {
            log.error("Failed to post the RDMA receive: {}", strerror(errno));
            cancel(req, session);
            return result;
        }
        for (int attempt = 1;; ++attempt) {
            ready = RcResponse{};
            ok = post(req, "ready", ready_headers, ready);
            if (ok || ready.http_code != 409 || attempt >= kReadyAttempts) {
                break;
            }
            log.warn("READY answered 409 {} (attempt {}/{}), retrying", ready.error_code(), attempt, kReadyAttempts);
            std::this_thread::sleep_for(kReadyRetryDelay);
        }
    }
    result.curl_code = ready.curl_code;
    result.http_code = ready.http_code;

    if (ok) {
        uint64_t bytes = 0;
        uint64_t cookie_echo = 0;
        if (!parse_uint(ready.header("x-amz-rdma-bytes-transferred"), 10, std::numeric_limits<uint64_t>::max(),
                        bytes) ||
            bytes != req.size) {
            log.error("READY reports {} bytes transferred, expected {}",
                      ready.header("x-amz-rdma-bytes-transferred"), req.size);
            ok = false;
        } else if (!parse_uint(ready.header("x-amz-rdma-cookie"), 16, std::numeric_limits<uint32_t>::max(),
                               cookie_echo) ||
                   cookie_echo != cookie) {
            log.error("READY cookie echo '{}' does not match {:08X}", ready.header("x-amz-rdma-cookie"), cookie);
            ok = false;
        }
    }
    if (ok) {
        ok = req.is_put ? wait_completion(IBV_WC_RDMA_WRITE, cookie, req.size, log)
                        : wait_completion(IBV_WC_RECV_RDMA_WITH_IMM, cookie, req.size, log);
    }

    auto transfer_end = std::chrono::high_resolution_clock::now();
    result.transfer_time = std::chrono::duration_cast<std::chrono::microseconds>(transfer_end - setup_end);

    if (!ok) {
        /* Report what our side saw of the data phase: a PUT whose write
         * exhausted its retries fails here, not in the READY response. */
        ibv_qp_attr qa{};
        ibv_qp_init_attr qi{};
        if (ibv_query_qp(qp_, &qa, IBV_QP_STATE | IBV_QP_DEST_QPN | IBV_QP_SQ_PSN | IBV_QP_RQ_PSN, &qi) == 0) {
            log.error("hipobj-rc session {}: local qp {} state {} dest_qpn {:x} sq_psn {:06X} rq_psn {:06X}",
                      session, qp_->qp_num, static_cast<int>(qa.qp_state), qa.dest_qp_num, qa.sq_psn,
                      qa.rq_psn);
        }
        ibv_wc wc{};
        while (ibv_poll_cq(cq_, 1, &wc) > 0) {
            log.error("hipobj-rc session {}: local completion {} opcode {} (wr_id {})", session,
                      ibv_wc_status_str(wc.status), static_cast<int>(wc.opcode), wc.wr_id);
        }
        cancel(req, session);
        return result;
    }

    log.debug("hipobj-rc {} of {} bytes complete (session {})", req.is_put ? "PUT" : "GET", req.size, session);
    result.etag = ready.header("x-amz-rdma-etag");
    result.status = RDMAStatus::Ok;
    return result;
}

} // namespace

RcTransport::RcTransport(std::string device, int gid_index)
    : device_name_(std::move(device)), requested_gid_index_(gid_index) {}

RcTransport::~RcTransport() {
    deregister_all();
    if (pd_ != nullptr) {
        ibv_dealloc_pd(pd_);
    }
    if (ctx_ != nullptr) {
        ibv_close_device(ctx_);
    }
}

bool RcTransport::initialize() {
    int num_devices = 0;
    ibv_device** list = ibv_get_device_list(&num_devices);
    if (list == nullptr || num_devices == 0) {
        spdlog::error("No RDMA devices found");
        if (list != nullptr) {
            ibv_free_device_list(list);
        }
        return false;
    }
    for (int i = 0; i < num_devices && ctx_ == nullptr; ++i) {
        const char* name = ibv_get_device_name(list[i]);
        if (device_name_.empty() || device_name_ == name) {
            device_name_ = name;
            ctx_ = ibv_open_device(list[i]);
        }
    }
    ibv_free_device_list(list);
    if (ctx_ == nullptr) {
        spdlog::error("Could not open RDMA device '{}'", device_name_.empty() ? "(first)" : device_name_);
        return false;
    }

    ibv_port_attr pa{};
    if (ibv_query_port(ctx_, port_.num, &pa) != 0) {
        spdlog::error("ibv_query_port({}, {}) failed", device_name_, port_.num);
        return false;
    }
    if (pa.state != IBV_PORT_ACTIVE) {
        spdlog::warn("RDMA device {} port {} is not active", device_name_, port_.num);
    }
    port_.lid = pa.lid;
    port_.active_mtu = pa.active_mtu;
    port_.max_msg_size = pa.max_msg_sz;
    port_.gid_index = requested_gid_index_ >= 0 ? requested_gid_index_ : pick_gid_index(ctx_, port_.num, pa);

    ibv_gid gid{};
    if (ibv_query_gid(ctx_, port_.num, port_.gid_index, &gid) != 0) {
        spdlog::error("ibv_query_gid({}, index {}) failed", device_name_, port_.gid_index);
        return false;
    }
    std::memcpy(port_.gid.data(), gid.raw, 16);
    if (std::all_of(port_.gid.begin(), port_.gid.end(), [](uint8_t b) { return b == 0; })) {
        spdlog::error("GID index {} on {} is empty; pick another with --rc-gid-index", port_.gid_index, device_name_);
        return false;
    }

    pd_ = ibv_alloc_pd(ctx_);
    if (pd_ == nullptr) {
        spdlog::error("ibv_alloc_pd failed: {}", strerror(errno));
        return false;
    }

    std::string ipv4;
    if (is_ipv4_mapped(port_.gid.data())) {
        ipv4 = fmt::format(" ({}.{}.{}.{})", port_.gid[12], port_.gid[13], port_.gid[14], port_.gid[15]);
    }
    spdlog::info("RC transport on {} port {}: gid[{}] {}{}, lid {}, {}, mtu {}", device_name_, port_.num,
                 port_.gid_index, format_gid(port_.gid), ipv4, port_.lid,
                 pa.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "Ethernet",
                 128 << pa.active_mtu);
    enabled_ = true;
    return true;
}

bool RcTransport::register_buffer(void* buffer, size_t size, int gpu_device_id) {
    if (!enabled_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mrs_mutex_);
    if (mrs_.find(buffer) != mrs_.end()) {
        return true;
    }
    const int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ibv_mr* mr = nullptr;
    if (gpu_device_id >= 0) {
        // GPU memory goes through dmabuf, so no peer-memory kernel module is
        // needed. The iova is the device pointer itself, which is the address
        // the tokens advertise to the server.
        auto dmabuf = GPUMemoryManager::export_dmabuf(buffer, size);
        if (!dmabuf) {
            spdlog::error("Cannot register GPU buffer {} for RC: {}", buffer, gpu_error_to_string(dmabuf.error()));
            return false;
        }
        mr = ibv_reg_dmabuf_mr(pd_, dmabuf->offset, size, reinterpret_cast<uintptr_t>(buffer), dmabuf->fd, access);
        int saved_errno = errno;
        GPUMemoryManager::close_dmabuf(dmabuf->fd);  // the MR holds its own reference to the dmabuf
        if (mr == nullptr) {
            spdlog::error("ibv_reg_dmabuf_mr failed for GPU {} buffer {} ({} bytes at dmabuf offset {:#x}): {}",
                          gpu_device_id, buffer, size, dmabuf->offset, strerror(saved_errno));
            spdlog::error("  The NIC reaches GPU memory through the GPU's PCIe BAR: all registered GPU buffers "
                          "(threads x object size) must fit in its free space (NVIDIA: BAR1 in "
                          "nvidia-smi -q -d MEMORY)");
            return false;
        }
    } else {
        mr = ibv_reg_mr(pd_, buffer, size, access);
        if (mr == nullptr) {
            spdlog::error("ibv_reg_mr failed for buffer {} ({} bytes): {}", buffer, size, strerror(errno));
            return false;
        }
    }
    mrs_[buffer] = mr;
    spdlog::debug("Registered {} buffer {} ({} bytes) for RC: rkey {:x}",
                  gpu_device_id >= 0 ? "GPU" : "host", buffer, size, mr->rkey);
    return true;
}

void RcTransport::deregister_all() {
    std::lock_guard<std::mutex> lock(mrs_mutex_);
    for (const auto& [buffer, mr] : mrs_) {
        ibv_dereg_mr(mr);
    }
    mrs_.clear();
}

ibv_mr* RcTransport::find_mr(void* buffer, size_t size) {
    std::lock_guard<std::mutex> lock(mrs_mutex_);
    auto it = mrs_.find(buffer);
    if (it == mrs_.end() || it->second->length < size) {
        return nullptr;
    }
    return it->second;
}

std::unique_ptr<RDMASession> RcTransport::create_session(void* buffer, size_t size) {
    ibv_mr* mr = find_mr(buffer, size);
    if (mr == nullptr) {
        spdlog::error("Buffer {} is not registered for RC", buffer);
        return nullptr;
    }
    auto session = std::make_unique<RcSession>(*this, buffer, size, mr);
    if (!session->open()) {
        return nullptr;
    }
    return session;
}
