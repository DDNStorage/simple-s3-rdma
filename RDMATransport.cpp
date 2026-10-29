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

#include "RDMATransport.h"
#include "Common.h"

#ifdef SIMPLE_S3_RDMA_HAVE_CUOBJ
#include "CuObjTransport.h"
#endif
#ifdef SIMPLE_S3_RDMA_HAVE_RC
#include "RcTransport.h"
#endif

std::unique_ptr<RDMATransport> create_rdma_transport([[maybe_unused]] const Config& config) {
    if (config.flavor == RdmaFlavor::AMD) {
#ifdef SIMPLE_S3_RDMA_HAVE_RC
        return std::make_unique<RcTransport>(config.rc_device, config.rc_gid_index);
#else
        spdlog::critical("AMD hipObject RC support was not compiled in (SIMPLE_S3_RDMA_WITH_RC=OFF)");
        return nullptr;
#endif
    }
#ifdef SIMPLE_S3_RDMA_HAVE_CUOBJ
    return std::make_unique<CuObjTransport>();
#else
    spdlog::critical("NVIDIA cuObject RDMA support was not compiled in (SIMPLE_S3_RDMA_WITH_CUOBJ=OFF); "
                     "use --flavor amd or --protocol tcp");
    return nullptr;
#endif
}
