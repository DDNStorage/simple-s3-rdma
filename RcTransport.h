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

#ifndef RC_TRANSPORT_H
#define RC_TRANSPORT_H

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "RDMATransport.h"

struct ibv_context;
struct ibv_pd;
struct ibv_mr;

// AMD hipObject flavor: the hipobj-rc-v2 two-phase protocol over a
// reliable-connected QP (libibverbs). Each transfer is PREPARE, a QP
// pairing against the server's per-session QP, then READY, which
// carries the data phase (RDMA WRITE_WITH_IMM in either direction).
class RcTransport : public RDMATransport {
public:
    // device: verbs device name, empty for the first device.
    // gid_index: GID table index, negative to pick automatically.
    RcTransport(std::string device, int gid_index);
    ~RcTransport() override;

    [[nodiscard]] const char* name() const override { return "hipobj-rc-v2"; }
    bool initialize() override;
    [[nodiscard]] bool is_enabled() const override { return enabled_; }
    bool register_buffer(void* buffer, size_t size, int gpu_device_id) override;
    void deregister_all() override;
    std::unique_ptr<RDMASession> create_session(void* buffer, size_t size) override;

    // Device attributes the sessions pair their QPs with
    struct Port {
        uint8_t num = 1;
        uint16_t lid = 0;
        int gid_index = 0;
        std::array<uint8_t, 16> gid{};
        int active_mtu = 0;        // enum ibv_mtu
        uint32_t max_msg_size = 0;
    };

    [[nodiscard]] ibv_context* context() const { return ctx_; }
    [[nodiscard]] ibv_pd* pd() const { return pd_; }
    [[nodiscard]] const Port& port() const { return port_; }
    ibv_mr* find_mr(void* buffer, size_t size);

private:
    std::string device_name_;
    int requested_gid_index_;
    ibv_context* ctx_ = nullptr;
    ibv_pd* pd_ = nullptr;
    Port port_;
    bool enabled_ = false;

    std::mutex mrs_mutex_;
    std::unordered_map<void*, ibv_mr*> mrs_;
};

#endif // RC_TRANSPORT_H
