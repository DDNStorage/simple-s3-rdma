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

/*
 * Simple S3 RDMA Client
 * Based on bench_curl_s3.c architecture
 */

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <argparse/argparse.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// System includes
#include <sys/mman.h>
#include <cerrno>

// OpenTelemetry includes
#include "opentelemetry/exporters/otlp/otlp_file_exporter_factory.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/trace/provider.h"

// Local includes
#include "Common.h"
#include "SimpleTracer.h"
#include "RDMATransport.h"
#include "BenchmarkThread.h"
#include "GPUMemoryManager.h"

#if !CURL_AT_LEAST_VERSION(7, 75, 0)
#    error "libcurl >= 7.75.0 required for CURLOPT_AWS_SIGV4"
#endif

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace otlp = opentelemetry::exporter::otlp;

#include "opentelemetry/exporters/otlp/otlp_file_exporter_factory.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/nostd/shared_ptr.h"

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;
namespace nostd = opentelemetry::nostd;



// Huge page allocator
void* allocate_huge_pages(size_t size) {
    // Try 1GB huge pages first (better for large allocations)
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT),
                     -1, 0);

    if (ptr != MAP_FAILED) {
        spdlog::info("Allocated {} bytes using 1GB huge pages", size);
        return ptr;
    }

    spdlog::warn("Failed to allocate 1GB huge pages: {}", strerror(errno));

    // Try 2MB huge pages
    ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT),
               -1, 0);

    if (ptr != MAP_FAILED) {
        spdlog::info("Allocated {} bytes using 2MB huge pages", size);
        return ptr;
    }

    spdlog::warn("Failed to allocate 2MB huge pages: {}", strerror(errno));
    
    // Fall back to transparent huge pages
    ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == MAP_FAILED) {
        spdlog::critical("Failed to allocate memory: {}", strerror(errno));
        return nullptr;
    }
    
    madvise(ptr, size, MADV_HUGEPAGE);
    spdlog::info("Allocated {} bytes using transparent huge pages", size);
    return ptr;
}

// Parse size with units
size_t parse_size(const char *s) {
    char *end = nullptr;
    long long n = strtoll(s, &end, 10);
    if (n <= 0) {
        return 0;
    }

    switch (*end) {
        case 'K': case 'k': return static_cast<size_t>(n) * 1024;
        case 'M': case 'm': return static_cast<size_t>(n) * 1024 * 1024;
        case 'G': case 'g': return static_cast<size_t>(n) * 1024 * 1024 * 1024;
        case '\0': return static_cast<size_t>(n);
        default:
            if (strncmp(end, "kiB", 3) == 0 || strncmp(end, "KiB", 3) == 0) {
                return static_cast<size_t>(n) * 1024;
            }
            if (strncmp(end, "MiB", 3) == 0) {
                return static_cast<size_t>(n) * 1024 * 1024;
            }
            if (strncmp(end, "GiB", 3) == 0) {
                return static_cast<size_t>(n) * 1024 * 1024 * 1024;
            }
            return 0;
    }
}

// Initialize logger
void init_logger(const std::optional<std::string>& output_file, const std::string& level_str) {
    try {
        std::shared_ptr<spdlog::logger> logger;

        if (!output_file.has_value()) {
            logger = spdlog::stderr_color_mt("s3_rdma");
        } else {
            logger = spdlog::basic_logger_mt("s3_rdma", output_file.value());
        }

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");

        // Match the level name case-insensitively: "debug" is as reasonable a thing
        // to type as "DEBUG", and silently falling through to INFO made a mistyped
        // level look like a level that simply produced no output.
        std::string level_upper = level_str;
        std::transform(level_upper.begin(), level_upper.end(), level_upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        bool level_recognized = true;
        if (level_upper == "TRACE") {
            logger->set_level(spdlog::level::trace);
        } else if (level_upper == "DEBUG") {
            logger->set_level(spdlog::level::debug);
        } else if (level_upper == "INFO") {
            logger->set_level(spdlog::level::info);
        } else if (level_upper == "WARN") {
            logger->set_level(spdlog::level::warn);
        } else if (level_upper == "ERROR") {
            logger->set_level(spdlog::level::err);
        } else {
            logger->set_level(spdlog::level::info);
            level_recognized = false;
        }

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::trace);

        if (!level_recognized) {
            spdlog::warn("Unrecognized --log-level '{}', using INFO. "
                         "Valid levels: TRACE, DEBUG, INFO, WARN, ERROR",
                         level_str);
        }

    } catch (const spdlog::spdlog_ex& ex) {
        fprintf(stderr, "Log initialization failed: %s\n", ex.what());
    }
}

// Parse command line arguments using argparse
bool parse_args(int argc, char** argv, Config& config) {
    argparse::ArgumentParser program("simple_s3_rdma", "1.0");

    program.add_description("Sample S3 RDMA client with NVIDIA cuObject and AMD hipObject support");

    // Positional argument for operation
    program.add_argument("operation")
        .help("Operation to perform: put or get");

    // Protocol selection
    program.add_argument("--protocol")
        .help("Transfer protocol: tcp or rdma")
        .default_value(std::string("rdma"));

    program.add_argument("--flavor")
        .help("RDMA flavor: nvidia (cuObject) or amd (hipObject hipobj-rc-v2)")
        .default_value(std::string("nvidia"));

    program.add_argument("--rc-device")
        .help("Verbs device for --flavor amd (default: first device)");

    program.add_argument("--rc-gid-index")
        .help("GID index for --flavor amd (default: first RoCEv2 IPv4 GID, 0 on InfiniBand)")
        .scan<'i', int>();

    // AWS credentials
    const char* env_access_key = std::getenv("AWS_ACCESS_KEY_ID");
    const char* env_secret_key = std::getenv("AWS_SECRET_ACCESS_KEY");
    const char* env_endpoint = std::getenv("S3_ENDPOINT");

    program.add_argument("--access-key")
        .help("AWS Access Key ID")
        .default_value(std::string(env_access_key != nullptr ? env_access_key : ""));

    program.add_argument("--secret-key")
        .help("AWS Secret Access Key")
        .default_value(std::string(env_secret_key != nullptr ? env_secret_key : ""));

    program.add_argument("--endpoint")
        .help("S3 endpoint URL")
        .default_value(std::string(env_endpoint != nullptr ? env_endpoint : ""));

    // S3 configuration
    program.add_argument("--bucket-name")
        .help("S3 bucket name")
        .default_value(std::string("test-bucket"));

    program.add_argument("--region")
        .help("AWS region")
        .default_value(std::string("us-east-1"));

    // Object configuration
    program.add_argument("--obj.size")
        .help("Object size (e.g., 1MiB, 10GiB, 100kiB)")
        .required();

    program.add_argument("--count")
        .help("Number of objects to transfer")
        .default_value(1)
        .scan<'i', int>();

    program.add_argument("--threads")
        .help("Number of concurrent threads")
        .default_value(1)
        .scan<'i', int>();

    program.add_argument("--stagger-start")
        .help("Stagger thread start by N microseconds")
        .scan<'i', int>();

    // Statistics configuration
    program.add_argument("--sample-interval")
        .help("Sample interval in seconds for statistics aggregation (default: 10)")
        .default_value(10)
        .scan<'i', int>();

    // Load balancing configuration
    program.add_argument("--load-balance")
        .help("Comma-separated list of IP addresses for round-robin load balancing (e.g., 192.168.1.1,192.168.1.2)");

    // Logging configuration
    program.add_argument("--log-output")
        .help("Log output file (default: stderr)");

    program.add_argument("--log-level")
        .help("Log level: TRACE, DEBUG, INFO, WARN, ERROR (case-insensitive)")
        .default_value(std::string("INFO"));

    // Tracing configuration
    program.add_argument("--trace-output")
        .help("OTEL trace output file");

    program.add_argument("--trace-sampling")
        .help("Trace sampling ratio (0.0-1.0)")
        .default_value(1.0)
        .scan<'g', double>();

    // GPU configuration
    program.add_argument("--gpu")
        .help("GPU device ID to use for memory allocation (enables GPU memory)")
        .scan<'i', int>();

    program.add_argument("--list-gpus")
        .help("List available GPU devices and exit")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--verify")
        .help("Verify object content against the server's ETag (hex MD5). Catches "
              "corruption anywhere in the path, including the GPU staging copies")
        .default_value(false)
        .implicit_value(true);

    // Parse arguments
    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << '\n';
        std::cerr << program;
        return false;
    }

    // Extract values
    config.operation = program.get<std::string>("operation");
    if (config.operation != "put" && config.operation != "get") {
        std::cerr << "Error: Operation must be 'put' or 'get'\n";
        return false;
    }

    std::string protocol_str = program.get<std::string>("--protocol");
    if (protocol_str == "tcp") {
        config.protocol = Protocol::TCP;
    } else if (protocol_str == "rdma") {
        config.protocol = Protocol::RDMA;
    } else {
        std::cerr << "Error: Protocol must be 'tcp' or 'rdma'\n";
        return false;
    }

    std::string flavor_str = program.get<std::string>("--flavor");
    if (flavor_str == "nvidia") {
        config.flavor = RdmaFlavor::NVIDIA;
    } else if (flavor_str == "amd") {
        config.flavor = RdmaFlavor::AMD;
    } else {
        std::cerr << "Error: Flavor must be 'nvidia' or 'amd'\n";
        return false;
    }
    if (program.is_used("--rc-device")) {
        config.rc_device = program.get<std::string>("--rc-device");
    }
    if (program.is_used("--rc-gid-index")) {
        config.rc_gid_index = program.get<int>("--rc-gid-index");
    }
    config.access_key = program.get<std::string>("--access-key");
    config.secret_key = program.get<std::string>("--secret-key");
    config.endpoint = program.get<std::string>("--endpoint");
    config.bucket_name = program.get<std::string>("--bucket-name");
    config.region = program.get<std::string>("--region");
    config.count = program.get<int>("--count");
    config.threads = program.get<int>("--threads");

    // Optional arguments
    if (program.is_used("--stagger-start")) {
        config.stagger_start = std::chrono::microseconds(program.get<int>("--stagger-start"));
    }

    config.sample_interval = std::chrono::seconds(program.get<int>("--sample-interval"));

    if (program.is_used("--log-output")) {
        config.log_output = program.get<std::string>("--log-output");
    }
    config.log_level = program.get<std::string>("--log-level");

    if (program.is_used("--trace-output")) {
        config.trace_output = program.get<std::string>("--trace-output");
    }
    config.trace_sampling = program.get<double>("--trace-sampling");

    // GPU configuration
    if (program.is_used("--gpu")) {
        config.gpu_id = program.get<int>("--gpu");
        config.memory_type = MemoryType::GPU;
    }

    config.verify = program.get<bool>("--verify");

    // Parse load-balance IPs
    if (program.is_used("--load-balance")) {
        std::string lb_str = program.get<std::string>("--load-balance");
        size_t start = 0;
        size_t end = 0;
        while ((end = lb_str.find(',', start)) != std::string::npos) {
            std::string ip = lb_str.substr(start, end - start);
            // Trim whitespace
            ip.erase(0, ip.find_first_not_of(" \t"));
            ip.erase(ip.find_last_not_of(" \t") + 1);
            if (!ip.empty()) {
                config.load_balance_ips.push_back(ip);
            }
            start = end + 1;
        }
        // Add the last IP
        std::string ip = lb_str.substr(start);
        ip.erase(0, ip.find_first_not_of(" \t"));
        ip.erase(ip.find_last_not_of(" \t") + 1);
        if (!ip.empty()) {
            config.load_balance_ips.push_back(ip);
        }

        if (config.load_balance_ips.empty()) {
            std::cerr << "Error: --load-balance specified but no valid IP addresses found\n";
            return false;
        }
    }

    // Parse object size
    auto size_str = program.get<std::string>("--obj.size");
    config.obj_size = parse_size(size_str.c_str());
    if (config.obj_size == 0) {
        std::cerr << "Error: Invalid object size: " << size_str << '\n';
        return false;
    }

    // Validate required parameters
    if (config.access_key.empty() || config.secret_key.empty() || config.endpoint.empty()) {
        std::cerr << "Error: Missing required parameters (access-key, secret-key, endpoint)\n";
        std::cerr << "These can be provided via command line or environment variables:\n";
        std::cerr << "  AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, S3_ENDPOINT\n";
        return false;
    }

    return true;
}

// Check if bucket exists and create if needed
bool ensure_bucket_exists(const Config& config, const std::string& sigv4, const std::string& userpwd) {
    std::string url = config.endpoint + "/" + config.bucket_name;

    spdlog::info("Checking if bucket '{}' exists", config.bucket_name);

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        spdlog::error("Failed to initialize CURL for bucket check");
        return false;
    }

    // Configure load balancing if enabled
    struct curl_slist* resolve_list = nullptr;
    if (!config.load_balance_ips.empty()) {
        // Extract hostname and port from endpoint
        std::string host;
        size_t proto_end = config.endpoint.find("://");
        if (proto_end != std::string::npos) {
            host = config.endpoint.substr(proto_end + 3);
        } else {
            host = config.endpoint;
        }

        std::string hostname = host;
        std::string port;
        size_t colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            hostname = host.substr(0, colon_pos);
            port = host.substr(colon_pos + 1);
        } else {
            port = (config.endpoint.find("https://") == 0) ? "443" : "80";
        }

        // Use first IP for bucket operations
        const std::string& selected_ip = config.load_balance_ips[0];
        std::string resolve_entry = hostname + ":" + port + ":" + selected_ip;
        resolve_list = curl_slist_append(resolve_list, resolve_entry.c_str());
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
        spdlog::debug("Load balancing (bucket check): {} -> {}", hostname, selected_ip);
    }

    // HEAD request to check if bucket exists
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, sigv4.c_str());
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD request
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (resolve_list != nullptr) {
        curl_slist_free_all(resolve_list);
        resolve_list = nullptr;
    }

    if (res == CURLE_OK && http_code == 200) {
        spdlog::info("Bucket '{}' exists", config.bucket_name);
        return true;
    }

    if (http_code != 404) {
        spdlog::error("Unexpected response when checking bucket: http={}", http_code);
        return false;
    }

    // Bucket doesn't exist, create it
    spdlog::info("Bucket '{}' does not exist, creating it", config.bucket_name);

    curl = curl_easy_init();
    if (curl == nullptr) {
        spdlog::error("Failed to initialize CURL for bucket creation");
        return false;
    }

    // Configure load balancing if enabled
    if (!config.load_balance_ips.empty()) {
        // Extract hostname and port from endpoint
        std::string host;
        size_t proto_end = config.endpoint.find("://");
        if (proto_end != std::string::npos) {
            host = config.endpoint.substr(proto_end + 3);
        } else {
            host = config.endpoint;
        }

        std::string hostname = host;
        std::string port;
        size_t colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            hostname = host.substr(0, colon_pos);
            port = host.substr(colon_pos + 1);
        } else {
            port = (config.endpoint.find("https://") == 0) ? "443" : "80";
        }

        // Use first IP for bucket operations
        const std::string& selected_ip = config.load_balance_ips[0];
        std::string resolve_entry = hostname + ":" + port + ":" + selected_ip;
        resolve_list = curl_slist_append(resolve_list, resolve_entry.c_str());
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
        spdlog::debug("Load balancing (bucket create): {} -> {}", hostname, selected_ip);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, sigv4.c_str());
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (resolve_list != nullptr) {
        curl_slist_free_all(resolve_list);
    }

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        spdlog::error("Failed to create bucket: curl={} http={}", curl_easy_strerror(res), http_code);
        return false;
    }

    spdlog::info("Bucket '{}' created successfully", config.bucket_name);
    return true;
}

int main(int argc, char** argv) {
    // Check for --list-gpus early (before full arg parsing)
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--list-gpus") {
            auto devices_result = GPUMemoryManager::enumerate_devices();
            if (!devices_result) {
                std::cerr << "Failed to enumerate GPU devices: "
                         << gpu_error_to_string(devices_result.error()) << "\n";
                return 1;
            }

            auto& devices = devices_result.value();
            if (devices.empty()) {
                std::cout << "No " << GPUMemoryManager::backend_name() << " devices found\n";
                return 0;
            }

            std::cout << "Available " << GPUMemoryManager::backend_name() << " devices:\n";
            for (const auto& dev : devices) {
                std::cout << "\nGPU " << dev.device_id << ": " << dev.name << "\n";
                std::cout << "  Compute Capability: " << dev.compute_capability_major
                          << "." << dev.compute_capability_minor << "\n";
                std::cout << "  Total Memory: " << (dev.total_memory / (1024.0 * 1024.0 * 1024.0))
                          << " GiB (" << dev.total_memory << " bytes)\n";
                std::cout << "  Free Memory: " << (dev.free_memory / (1024.0 * 1024.0 * 1024.0))
                          << " GiB (" << dev.free_memory << " bytes)\n";
                std::cout << "  PCI Bus ID: " << std::hex << dev.pci_bus_id << ":"
                          << dev.pci_device_id << std::dec << "\n";
                if (dev.bar) {
                    std::cout << "  " << dev.bar->name << " (GPU-direct RDMA window): "
                              << dev.bar->total / (1024 * 1024) << " MiB total, "
                              << dev.bar->free / (1024 * 1024) << " MiB free\n";
                }
            }
            return 0;
        }
    }

    Config config;

    if (!parse_args(argc, argv, config)) {
        return 1;
    }

    init_logger(config.log_output, config.log_level);

    // Save the main logger so we can restore it after threads complete
    auto main_logger = spdlog::default_logger();

    spdlog::info("S3 RDMA Client starting");
    spdlog::info("Operation: {}", config.operation);
    spdlog::info("Protocol: {}", config.protocol == Protocol::RDMA ? "rdma" : "tcp");
    if (config.protocol == Protocol::RDMA) {
        spdlog::info("RDMA flavor: {}", config.flavor == RdmaFlavor::AMD ? "amd (hipobj-rc-v2)" : "nvidia (cuObject)");
    }
    spdlog::info("Endpoint: {}", config.endpoint);
    spdlog::info("Bucket: {}", config.bucket_name);
    spdlog::info("Object size: {} bytes", config.obj_size);
    spdlog::info("Count: {}", config.count);
    spdlog::info("Threads: {}", config.threads);

    // Validate and log GPU configuration
    if (config.memory_type == MemoryType::GPU) {
        if (!config.gpu_id.has_value()) {
            spdlog::critical("GPU memory type selected but no GPU ID specified");
            return 1;
        }

        auto is_valid_result = GPUMemoryManager::is_valid_device(config.gpu_id.value());
        if (!is_valid_result) {
            spdlog::critical("Failed to validate GPU device: {}", gpu_error_to_string(is_valid_result.error()));
            return 1;
        }
        if (!is_valid_result.value()) {
            spdlog::critical("Invalid GPU device ID: {}", config.gpu_id.value());
            return 1;
        }

        auto devices_result = GPUMemoryManager::enumerate_devices();
        if (!devices_result) {
            spdlog::critical("Failed to enumerate GPU devices: {}", gpu_error_to_string(devices_result.error()));
            return 1;
        }

        for (const auto& dev : devices_result.value()) {
            if (dev.device_id == config.gpu_id.value()) {
                spdlog::info("Memory type: GPU (device {})", config.gpu_id.value());
                GPUMemoryManager::print_device_info(dev);

                // Every thread's buffer is registered for RDMA at once, so they must
                // all fit in the BAR window the NIC reaches GPU memory through. This
                // catches only what certainly fails: mapping overhead means buffers
                // just under the free space can still fail registration (240 MiB of
                // 242 MiB free on a Quadro RTX 4000), which then logs its own hint.
                const size_t needed = static_cast<size_t>(config.threads) * config.obj_size;
                if (config.protocol == Protocol::RDMA && dev.bar && needed > dev.bar->free) {
                    constexpr double MiB = 1024.0 * 1024.0;
                    if (dev.bar->hard_limit) {
                        spdlog::critical("{} threads x {} bytes = {:.0f} MiB of GPU buffers do not fit in the "
                                         "{:.0f} MiB of free {} space the NIC maps them through; use fewer "
                                         "threads or a smaller --obj.size",
                                         config.threads, config.obj_size, needed / MiB, dev.bar->free / MiB,
                                         dev.bar->name);
                        return 1;
                    }
                    spdlog::warn("{} threads x {} bytes = {:.0f} MiB of GPU buffers exceed the {:.0f} MiB of "
                                 "free {}; registration may fail, or the driver may stage through system memory",
                                 config.threads, config.obj_size, needed / MiB, dev.bar->free / MiB,
                                 dev.bar->name);
                }
                break;
            }
        }
    } else {
        spdlog::info("Memory type: System (huge pages)");
    }
    config.stagger_start.and_then([](auto duration) -> std::optional<std::chrono::microseconds> {
        spdlog::info("Stagger start: {} μs", duration.count());
        return std::nullopt;
    });
    if (!config.load_balance_ips.empty()) {
        std::string ips_str;
        for (size_t i = 0; i < config.load_balance_ips.size(); ++i) {
            if (i > 0) ips_str += ", ";
            ips_str += config.load_balance_ips[i];
        }
        spdlog::info("Load balancing enabled: {} IPs [{}]", config.load_balance_ips.size(), ips_str);
    }

    // Check CUFILE_ENV_PATH_JSON (RDMA mode with the cuObject flavor only)
#ifdef SIMPLE_S3_RDMA_HAVE_CUOBJ
    if (config.protocol == Protocol::RDMA && config.flavor == RdmaFlavor::NVIDIA) {
        const char* cufile_env = std::getenv("CUFILE_ENV_PATH_JSON");
        if (cufile_env == nullptr) {
            spdlog::critical("CUFILE_ENV_PATH_JSON environment variable is not set");
            return 1;
        }
        spdlog::info("CUFILE_ENV_PATH_JSON: {}", cufile_env);
    }
#endif

    // Initialize OpenTelemetry
    bool tracing_enabled = config.trace_output.has_value();

    if (tracing_enabled) {
        // Create OTLP file exporter
        otlp::OtlpFileClientFileSystemOptions fs_opts;
        fs_opts.file_pattern = config.trace_output.value();

        otlp::OtlpFileExporterOptions exporter_opts;
        exporter_opts.console_debug = false;
        exporter_opts.backend_options = fs_opts;

        auto exporter = otlp::OtlpFileExporterFactory::Create(exporter_opts);
        auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

        // Create resource with service name
        auto resource_attributes = resource::ResourceAttributes{
            {"service.name", "simple-s3-rdma"},
            {"service.version", "1.0"}
        };
        auto resource = resource::Resource::Create(resource_attributes);

        // Create tracer provider
        auto provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
        trace_api::Provider::SetTracerProvider(nostd::shared_ptr<trace_api::TracerProvider>(provider.release()));

        spdlog::info("OpenTelemetry tracing enabled: output={}, sampling={}", config.trace_output.value(), config.trace_sampling);
    }

    SimpleTracer tracer(tracing_enabled, config.trace_sampling);

    // Initialize RDMA if requested
    std::unique_ptr<RDMATransport> rdma_transport;
    RDMATransport* rdma_ptr = nullptr;
    bool use_rdma = (config.protocol == Protocol::RDMA);
    if (use_rdma) {
        rdma_transport = create_rdma_transport(config);
        if (!rdma_transport) {
            return 1;
        }
        spdlog::info("Initializing RDMA support ({})...", rdma_transport->name());
        if (!rdma_transport->initialize()) {
            spdlog::warn("RDMA initialization failed, falling back to TCP");
            use_rdma = false;
        } else {
            rdma_ptr = rdma_transport.get();
        }
    }

    // Initialize curl
    curl_global_init(CURL_GLOBAL_ALL);

    // Build sigv4 string
    std::string sigv4 = "aws:amz:" + config.region + ":s3";
    std::string userpwd = config.access_key + ":" + config.secret_key;

    // Ensure bucket exists
    if (!ensure_bucket_exists(config, sigv4, userpwd)) {
        spdlog::critical("Failed to ensure bucket exists");
        curl_global_cleanup();
        return 1;
    }

    // Allocate and register buffers serially (cuObjClient is not thread-safe)
    spdlog::info("Allocating and registering {} buffer(s)", config.threads);

    // Store buffers - either GPU or system memory
    std::vector<GPUMemory> gpu_buffers;
    std::vector<uint8_t*> system_buffers;

    for (int i = 0; i < config.threads; ++i) {
        // Allocate buffer based on memory type
        uint8_t* buffer_ptr = nullptr;

        if (config.memory_type == MemoryType::GPU) {
            auto alloc_result = GPUMemoryManager::allocate_gpu_memory(
                config.gpu_id.value(), config.obj_size);
            if (!alloc_result) {
                spdlog::critical("Failed to allocate GPU buffer for thread {}: {}",
                               i, gpu_error_to_string(alloc_result.error()));
                // GPUMemory RAII will auto-cleanup gpu_buffers on return
                curl_global_cleanup();
                return 1;
            }

            buffer_ptr = static_cast<uint8_t*>(alloc_result.value().get());

            // Register buffer for RDMA if enabled
            if (rdma_ptr != nullptr) {
                if (!rdma_ptr->register_buffer(buffer_ptr, config.obj_size, config.gpu_id.value())) {
                    spdlog::critical("Failed to register GPU buffer for thread {}", i);
                    // GPUMemory RAII will auto-cleanup gpu_buffers on return
                    curl_global_cleanup();
                    return 1;
                }
            }

            // Move the GPUMemory into our vector (transfers ownership)
            gpu_buffers.push_back(std::move(alloc_result.value()));

        } else {
            buffer_ptr = static_cast<uint8_t*>(allocate_huge_pages(config.obj_size));
            if (buffer_ptr == nullptr) {
                spdlog::critical("Failed to allocate system buffer for thread {}", i);
                // Clean up previously allocated system buffers
                for (auto* buf : system_buffers) {
                    munmap(buf, config.obj_size);
                }
                curl_global_cleanup();
                return 1;
            }

            // Register buffer for RDMA if enabled
            if (rdma_ptr != nullptr) {
                if (!rdma_ptr->register_buffer(buffer_ptr, config.obj_size, -1)) {
                    spdlog::critical("Failed to register system buffer for thread {}", i);
                    munmap(buffer_ptr, config.obj_size);
                    // Clean up previously allocated system buffers
                    for (auto* buf : system_buffers) {
                        munmap(buf, config.obj_size);
                    }
                    curl_global_cleanup();
                    return 1;
                }
            }

            system_buffers.push_back(buffer_ptr);
        }
    }

    // Create benchmark threads
    spdlog::info("Creating {} benchmark thread(s)", config.threads);
    std::vector<std::unique_ptr<BenchmarkThread>> threads;
    std::vector<std::thread> thread_handles;

    for (int i = 0; i < config.threads; ++i) {
        auto thread = std::make_unique<BenchmarkThread>(i, config, tracer, rdma_ptr);

        // Set buffer based on memory type
        if (config.memory_type == MemoryType::GPU) {
            thread->set_buffer(static_cast<uint8_t*>(gpu_buffers[i].get()));
        } else {
            thread->set_buffer(system_buffers[i]);
        }

        threads.push_back(std::move(thread));
    }

    // Initialize all threads
    spdlog::info("Initializing threads...");
    std::atomic<int> init_success_count{0};
    std::atomic<int> init_complete_count{0};
    std::vector<std::thread> init_threads;

    for (int i = 0; i < config.threads; ++i) {
        init_threads.emplace_back([&, i]() {
            if (threads[i]->init()) {
                init_success_count++;
            }
            init_complete_count++;
        });
    }

    // Wait for all init threads to complete
    for (auto& t : init_threads) {
        t.join();
    }

    // Check if all threads initialized successfully
    if (init_success_count != config.threads) {
        spdlog::critical("Thread initialization failed: {}/{} threads initialized successfully",
                        init_success_count.load(), config.threads);
        curl_global_cleanup();
        return 1;
    }

    spdlog::info("All {} threads initialized successfully", config.threads);

    // Start benchmark
    auto overall_start = std::chrono::high_resolution_clock::now();

    // Start overall trace span
    nostd::shared_ptr<trace_api::Span> overall_span = tracer.start_span("s3_operations");
    SimpleTracer::set_span_attribute(overall_span, "operation", config.operation);
    SimpleTracer::set_span_attribute(overall_span, "bucket", config.bucket_name);
    SimpleTracer::set_span_attribute(overall_span, "object_size", static_cast<int64_t>(config.obj_size));
    SimpleTracer::set_span_attribute(overall_span, "count", static_cast<int64_t>(config.count));
    SimpleTracer::set_span_attribute(overall_span, "threads", static_cast<int64_t>(config.threads));

    // Launch benchmark threads with optional stagger
    spdlog::info("Starting {} benchmark thread(s)", config.threads);
    for (int i = 0; i < config.threads; ++i) {
        // Capture span by value (nostd::shared_ptr should be copyable)
        auto span_copy = overall_span;
        thread_handles.emplace_back([&threads, i, span_copy]() {
            threads[i]->run(span_copy);
        });

        // Stagger thread start if requested (using lambda with optional)
        if (i < config.threads - 1) {
            config.stagger_start.and_then([](auto duration) -> std::optional<std::chrono::microseconds> {
                std::this_thread::sleep_for(duration);
                return std::nullopt;
            });
        }
    }

    // Wait for all threads to complete
    spdlog::info("Waiting for threads to complete...");
    for (auto& t : thread_handles) {
        t.join();
    }

    // Restore main logger
    spdlog::set_default_logger(main_logger);

    spdlog::info("All threads completed");

    // Collect aggregate statistics from all threads
    std::vector<LatencyStats> all_stats;
    size_t total_bytes = 0;
    size_t failed_count = 0;
    size_t verify_failed_count = 0;
    size_t verify_skipped_count = 0;

    for (const auto& thread : threads) {
        const auto& thread_stats = thread->get_stats();
        all_stats.insert(all_stats.end(), thread_stats.begin(), thread_stats.end());
        total_bytes += thread->get_total_bytes();
        failed_count += thread->get_failed_count();
        verify_failed_count += thread->get_verify_failed_count();
        verify_skipped_count += thread->get_verify_skipped_count();
    }

    // End overall span
    int total_operations = config.count * config.threads;
    SimpleTracer::set_span_attribute(overall_span, "total_operations", static_cast<int64_t>(total_operations));
    SimpleTracer::set_span_attribute(overall_span, "successful_operations", static_cast<int64_t>(all_stats.size()));
    SimpleTracer::set_span_attribute(overall_span, "failed_operations", static_cast<int64_t>(failed_count));
    SimpleTracer::set_span_attribute(overall_span, "total_bytes", static_cast<int64_t>(total_bytes));
    SimpleTracer::set_span_status(overall_span, failed_count == 0);
    SimpleTracer::end_span(overall_span);

    auto overall_end = std::chrono::high_resolution_clock::now();
    double total_time_sec = std::chrono::duration<double>(overall_end - overall_start).count();

    // Print statistics
    spdlog::info("=== Summary ===");
    spdlog::info("Total operations: {}", total_operations);
    spdlog::info("Successful operations: {}", all_stats.size());
    spdlog::info("Failed operations: {}", failed_count);
    if (config.verify) {
        const size_t verified = all_stats.size() - std::min(verify_skipped_count, all_stats.size());
        if (verify_failed_count > 0) {
            spdlog::error("Content verification FAILED on {} operation(s)", verify_failed_count);
        } else if (verified > 0) {
            spdlog::info("Content verification: all {} operation(s) matched the server ETag", verified);
        } else if (all_stats.empty()) {
            spdlog::warn("Content verification: no operation completed, nothing was verified");
        }
        if (verify_skipped_count > 0) {
            spdlog::info("Content verification: skipped for {} RDMA PUT(s) "
                         "(the ETag is assigned by the server, not a content digest)", verify_skipped_count);
        }
    }

    // Format total bytes in human-readable base-2 units
    std::string bytes_formatted;
    if (total_bytes >= 1024ULL * 1024 * 1024) {
        bytes_formatted = fmt::format("{:.2f} GiB", total_bytes / (1024.0 * 1024 * 1024));
    } else if (total_bytes >= 1024ULL * 1024) {
        bytes_formatted = fmt::format("{:.2f} MiB", total_bytes / (1024.0 * 1024));
    } else if (total_bytes >= 1024ULL) {
        bytes_formatted = fmt::format("{:.2f} KiB", total_bytes / 1024.0);
    } else {
        bytes_formatted = fmt::format("{} B", total_bytes);
    }

    spdlog::info("Total bytes transferred: {} ({} bytes)", bytes_formatted, total_bytes);
    spdlog::info("Total time: {:.2f} seconds", total_time_sec);

    if (total_bytes > 0) {
        // Base-2 bytes (binary)
        double throughput_bps = total_bytes / total_time_sec;
        double throughput_kib = throughput_bps / 1024.0;
        double throughput_mib = throughput_kib / 1024.0;
        double throughput_gib = throughput_mib / 1024.0;

        // Gigabits per second (decimal, networking standard)
        double throughput_bits = (total_bytes * 8.0) / total_time_sec;
        double throughput_gbps = throughput_bits / 1000000000.0;

        spdlog::info("Throughput: {:.2f} MiB/s, {:.2f} GiB/s, {:.2f} Gbps",
                    throughput_mib, throughput_gib, throughput_gbps);
    }

    // Compute percentiles
    if (!all_stats.empty()) {
        std::vector<double> setup_times_ms;
        std::vector<double> transfer_times_ms;
        std::vector<double> total_times_ms;
        for (const auto& s : all_stats) {
            setup_times_ms.push_back(s.setup_time.count() / 1000.0);
            transfer_times_ms.push_back(s.transfer_time.count() / 1000.0);
            total_times_ms.push_back(s.total_time.count() / 1000.0);
        }

        auto percentile = [](std::vector<double>& values, double p) {
            std::sort(values.begin(), values.end());
            auto idx = static_cast<size_t>(p * static_cast<double>(values.size()));
            if (idx >= values.size()) {
                idx = values.size() - 1;
            }
            return values[idx];
        };

        spdlog::info("=== Latency Statistics ===");
        spdlog::info("Setup Time (ms)    - p50: {:.2f}, p90: {:.2f}, p95: {:.2f}, p99: {:.2f}",
                    percentile(setup_times_ms, 0.50), percentile(setup_times_ms, 0.90),
                    percentile(setup_times_ms, 0.95), percentile(setup_times_ms, 0.99));
        spdlog::info("Transfer Time (ms) - p50: {:.2f}, p90: {:.2f}, p95: {:.2f}, p99: {:.2f}",
                    percentile(transfer_times_ms, 0.50), percentile(transfer_times_ms, 0.90),
                    percentile(transfer_times_ms, 0.95), percentile(transfer_times_ms, 0.99));
        spdlog::info("Total Time (ms)    - p50: {:.2f}, p90: {:.2f}, p95: {:.2f}, p99: {:.2f}",
                    percentile(total_times_ms, 0.50), percentile(total_times_ms, 0.90),
                    percentile(total_times_ms, 0.95), percentile(total_times_ms, 0.99));
    }

    // Compute sample interval statistics
    if (!all_stats.empty() && all_stats.size() > 1) {
        // Sort stats by completion time
        std::vector<LatencyStats> sorted_stats = all_stats;
        std::sort(sorted_stats.begin(), sorted_stats.end(),
                  [](const LatencyStats& a, const LatencyStats& b) {
                      return a.completion_time < b.completion_time;
                  });

        // Find the time range
        auto first_completion = sorted_stats.front().completion_time;
        auto last_completion = sorted_stats.back().completion_time;

        // Calculate number of intervals
        auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(
            last_completion - first_completion);

        if (total_duration.count() > 0) {
            // Group operations by sample interval
            struct IntervalStats {
                size_t bytes = 0;
                size_t count = 0;
                double duration_sec = 0.0;
            };

            std::vector<IntervalStats> intervals;
            size_t current_interval = 0;
            IntervalStats current_stats;
            auto interval_start = first_completion;

            for (const auto& stat : sorted_stats) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    stat.completion_time - first_completion);
                size_t interval_idx = elapsed.count() / config.sample_interval.count();

                // If we've moved to a new interval, save the current one
                while (current_interval < interval_idx) {
                    if (current_stats.count > 0) {
                        auto interval_end = interval_start + config.sample_interval;
                        current_stats.duration_sec = std::chrono::duration<double>(
                            interval_end - interval_start).count();
                        intervals.push_back(current_stats);
                    }
                    current_stats = IntervalStats{};
                    current_interval++;
                    interval_start += config.sample_interval;
                }

                // Add to current interval
                current_stats.bytes += config.obj_size;
                current_stats.count++;
            }

            // Add the last interval
            if (current_stats.count > 0) {
                auto interval_end = last_completion;
                current_stats.duration_sec = std::chrono::duration<double>(
                    interval_end - interval_start).count();
                intervals.push_back(current_stats);
            }

            // Calculate throughput for each interval and find min/max/median intervals
            if (!intervals.empty()) {
                struct IntervalWithThroughput {
                    size_t index;
                    double throughput_mib;
                    size_t bytes;
                    size_t count;
                    double duration_sec;
                };

                std::vector<IntervalWithThroughput> interval_throughputs;
                for (size_t i = 0; i < intervals.size(); ++i) {
                    const auto& interval = intervals[i];
                    if (interval.duration_sec > 0) {
                        double throughput_mib = (interval.bytes / interval.duration_sec) / (1024.0 * 1024.0);
                        interval_throughputs.push_back({i, throughput_mib, interval.bytes, interval.count, interval.duration_sec});
                    }
                }

                if (!interval_throughputs.empty()) {
                    // Find min and max intervals
                    auto min_it = std::min_element(interval_throughputs.begin(), interval_throughputs.end(),
                        [](const auto& a, const auto& b) { return a.throughput_mib < b.throughput_mib; });
                    auto max_it = std::max_element(interval_throughputs.begin(), interval_throughputs.end(),
                        [](const auto& a, const auto& b) { return a.throughput_mib < b.throughput_mib; });

                    // Find median interval
                    std::vector<IntervalWithThroughput> sorted_intervals = interval_throughputs;
                    std::sort(sorted_intervals.begin(), sorted_intervals.end(),
                        [](const auto& a, const auto& b) { return a.throughput_mib < b.throughput_mib; });

                    size_t median_idx = sorted_intervals.size() / 2;
                    if (sorted_intervals.size() % 2 == 0 && median_idx > 0) {
                        median_idx--; // For even count, use the lower middle value
                    }
                    const auto& median_interval = sorted_intervals[median_idx];

                    spdlog::info("=== Sample Interval Statistics ({}s intervals) ===", config.sample_interval.count());
                    spdlog::info("Number of intervals: {}", interval_throughputs.size());

                    // Convert MiB/s to Gbps for each interval
                    auto to_gbps = [](double mib_per_sec) {
                        return (mib_per_sec * 1024.0 * 1024.0 * 8.0) / 1000000000.0;
                    };

                    spdlog::info("Min Throughput Interval: {:.2f} MiB/s, {:.2f} Gbps ({} ops, {:.2f} s)",
                                min_it->throughput_mib, to_gbps(min_it->throughput_mib),
                                min_it->count, min_it->duration_sec);
                    spdlog::info("Max Throughput Interval: {:.2f} MiB/s, {:.2f} Gbps ({} ops, {:.2f} s)",
                                max_it->throughput_mib, to_gbps(max_it->throughput_mib),
                                max_it->count, max_it->duration_sec);
                    spdlog::info("Median Throughput Interval: {:.2f} MiB/s, {:.2f} Gbps ({} ops, {:.2f} s)",
                                median_interval.throughput_mib, to_gbps(median_interval.throughput_mib),
                                median_interval.count, median_interval.duration_sec);
                }
            }
        }
    }

    // Cleanup (OTEL exporter handles trace export automatically)
    // Deregister before freeing, or cuFile sees registered memory disappear
    if (rdma_transport) {
        rdma_transport->deregister_all();
    }

    // Clean up buffers
    if (config.memory_type == MemoryType::GPU) {
        // GPUMemory RAII handles cleanup automatically
        gpu_buffers.clear();
    } else {
        for (auto* buffer : system_buffers) {
            munmap(buffer, config.obj_size);
        }
        system_buffers.clear();
    }

    curl_global_cleanup();

    spdlog::info("S3 RDMA Client finished");

    return (failed_count > 0) ? 1 : 0;
}


