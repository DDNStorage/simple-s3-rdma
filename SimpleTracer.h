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

#ifndef SIMPLE_TRACER_H
#define SIMPLE_TRACER_H

#include <array>
#include <string>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/trace/span.h"

namespace trace_api = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// Tracer wrapper for OpenTelemetry
class SimpleTracer {
private:
    nostd::shared_ptr<trace_api::Tracer> tracer_;
    bool enabled_;

public:
    SimpleTracer(bool enabled, [[maybe_unused]] double sampling_rate)
        : enabled_(enabled)
    {
        if (enabled_) {
            tracer_ = trace_api::Provider::GetTracerProvider()->GetTracer("simple-s3-rdma");
        }
    }

    [[nodiscard]] bool should_sample() const {
        return enabled_;  // OTEL handles sampling
    }

    nostd::shared_ptr<trace_api::Span> start_span(const std::string& name) {
        if (!enabled_ || !tracer_) {
            return nostd::shared_ptr<trace_api::Span>(nullptr);
        }
        return tracer_->StartSpan(name);
    }

    nostd::shared_ptr<trace_api::Span> start_span(const std::string& name,
                                                   const nostd::shared_ptr<trace_api::Span>& parent) {
        if (!enabled_ || !tracer_) {
            return nostd::shared_ptr<trace_api::Span>(nullptr);
        }

        if (parent && parent->GetContext().IsValid()) {
            trace_api::StartSpanOptions options;
            options.parent = parent->GetContext();
            return tracer_->StartSpan(name, options);
        }

        return tracer_->StartSpan(name);
    }

    static void end_span(const nostd::shared_ptr<trace_api::Span>& span) {
        if (span) {
            span->End();
        }
    }

    static void set_span_attribute(const nostd::shared_ptr<trace_api::Span>& span, const std::string& key, const std::string& value) {
        if (span) {
            span->SetAttribute(key, value);
        }
    }

    static void set_span_attribute(const nostd::shared_ptr<trace_api::Span>& span, const std::string& key, int64_t value) {
        if (span) {
            span->SetAttribute(key, value);
        }
    }

    static void set_span_attribute(const nostd::shared_ptr<trace_api::Span>& span, const std::string& key, double value) {
        if (span) {
            span->SetAttribute(key, value);
        }
    }

    static void set_span_status(const nostd::shared_ptr<trace_api::Span>& span, bool success) {
        if (span) {
            span->SetStatus(success ? trace_api::StatusCode::kOk : trace_api::StatusCode::kError);
        }
    }

    // Get AWS X-Ray formatted trace ID header value
    static std::string get_xray_trace_id(const nostd::shared_ptr<trace_api::Span>& span) {
        if (!span) {
            return "";
        }

        auto span_context = span->GetContext();
        if (!span_context.IsValid()) {
            return "";
        }

        // Get trace ID and span ID as hex strings
        std::array<char, 32> trace_id_hex{};
        std::array<char, 16> span_id_hex{};
        span_context.trace_id().ToLowerBase16(nostd::span<char, 32>(trace_id_hex.data(), 32));
        span_context.span_id().ToLowerBase16(nostd::span<char, 16>(span_id_hex.data(), 16));

        // AWS X-Ray format: Root=1-{first 8 chars}-{remaining 24 chars};Parent={span_id};Sampled=1
        std::string result = "Root=1-";
        result += std::string(trace_id_hex.data(), 8);
        result += "-";
        result += std::string(trace_id_hex.data() + 8, 24);
        result += ";Parent=";
        result += std::string(span_id_hex.data(), 16);
        result += ";Sampled=1";

        return result;
    }
};

#endif // SIMPLE_TRACER_H

