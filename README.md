# Simple S3 RDMA Client

A sample S3 RDMA client in C++ with minimal build dependencies. One binary
speaks two RDMA flavors, selected at runtime with `--flavor`:

- `nvidia` (default): NVIDIA cuObject through libcuobjclient. The RDMA token
  rides in the `x-amz-rdma-token` header of the regular S3 PUT/GET.
- `amd`: AMD hipObject `hipobj-rc-v2`, a two-phase protocol over a
  reliable-connected (RC) queue pair using plain libibverbs. See
  [hipObject RC flavor](#hipobject-rc-flavor).

## Features

- **GPU Memory Support**: Direct GPU memory allocation via CUDA (NVIDIA) or HIP (AMD) for GPU-direct RDMA transfers
- **RDMA Support**: Integrates with NVIDIA's libcuobjclient for GPU-direct transfers
- **Huge Pages**: Automatic allocation using huge pages (2MB/1GB) with transparent huge page fallback
- **AWS Signature V4**: Uses libcurl's built-in AWS SigV4 support (CURLOPT_AWS_SIGV4)
- **Distributed Tracing**: OpenTelemetry integration with AWS X-Ray trace ID propagation
- **Performance Metrics**: Detailed latency tracking with percentile statistics (p50, p90, p95, p99)
- **Throughput Calculation**: Aggregate throughput in B/s, kiB/s, MiB/s, GiB/s
- **Logging**: spdlog-based logging with millisecond-resolution timestamps
- **SSL Control**: Disabled certificate verification for testing environments
- **Auto Bucket Creation**: Automatically creates S3 bucket if it doesn't exist

## Prerequisites

### Required Dependencies

- **CUDA Toolkit** (NVIDIA flavor and GPU memory only)
  - Typically installed at `/usr/local/cuda`
  - Provides `libcufile`, `libcuobjclient` and the `libcuda` stub to link against
  - A build with CUDA needs the NVIDIA driver's `libcuda.so.1` at runtime
- **HIP runtime** (GPU memory on AMD GPUs only)
  - ROCm under `/opt/rocm`, or the distribution packages:
    `sudo apt-get install libamdhip64-dev` (Ubuntu 26.04 ships ROCm 7.1)
  - Provides `libamdhip64` and `libhsa-runtime64`; the amdgpu kernel driver must be loaded at runtime
  - The distribution `libamdhip64` needs g++ 14 or newer to link against
- **rdma-core** (libibverbs, AMD flavor only)
  ```bash
  sudo apt-get install libibverbs-dev
  ```
- **libcurl** >= 7.75.0 (for CURLOPT_AWS_SIGV4 support)
  ```bash
  sudo apt-get install libcurl4-openssl-dev
  ```
- **OpenSSL**
  ```bash
  sudo apt-get install libssl-dev
  ```
- **spdlog** (logging library)
  ```bash
  sudo apt-get install libspdlog-dev
  ```
- **argparse** (command-line argument parsing)
  ```bash
  sudo apt-get install libargparse-dev
  ```
- **OpenTelemetry C++** (for distributed tracing)
  - Installed via vcpkg (automated by build script)
  - Required for trace correlation with AWS X-Ray
- **vcpkg** (C++ package manager)
  - Used to install OpenTelemetry C++
  - Automatically set up by the build script
- **CMake** >= 3.20
  ```bash
  sudo apt-get install cmake
  ```
- **C++17 compiler** (GCC 7+ or Clang 5+)

## Environment Variables

**Required for the NVIDIA flavor only:**
- `CUFILE_ENV_PATH_JSON`: Path to cuFile configuration (required with `--protocol rdma` and `--flavor nvidia`, the default)

**Optional:**
- `AWS_ACCESS_KEY_ID`: AWS Access Key (can be specified via --access-key)
- `AWS_SECRET_ACCESS_KEY`: AWS Secret Key (can be specified via --secret-key)
- `S3_ENDPOINT`: S3 endpoint URL (can be specified via --endpoint)

**Note:** When using `--protocol tcp`, CUFILE_ENV_PATH_JSON is not required.

## Building

### Automated Build (Recommended)

The easiest way to build on Ubuntu is to use the provided build script:

```bash
./scripts/build-ubuntu.sh
```

This script will:
1. Install all required system dependencies (libcurl, OpenSSL, spdlog, argparse, etc.)
2. Set up vcpkg and install OpenTelemetry C++
3. Check for CUDA installation
4. Build the application

The script is idempotent - it's safe to run multiple times.

### Manual Build

If you prefer to build manually:

```bash
# Install system dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    cmake \
    build-essential \
    libcurl4-openssl-dev \
    libssl-dev \
    libspdlog-dev \
    libargparse-dev

# Install vcpkg for OpenTelemetry
export VCPKG_ROOT=/tmp/vcpkg
git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT
$VCPKG_ROOT/bootstrap-vcpkg.sh
$VCPKG_ROOT/vcpkg install 'opentelemetry-cpp[otlp-file]'

# Build
mkdir -p build
cd build
VCPKG_ROOT=$VCPKG_ROOT cmake ..
make
```

### Build Output

The build will produce a single executable: `simple_s3_rdma`

### CMake Options

The build system will automatically:
- Find and link libcurl, OpenSSL, spdlog
- Detect OpenTelemetry if available (optional)
- Detect the CUDA driver API and libcuobjclient under `CUDA_TOOLKIT_ROOT_DIR` (default `/usr/local/cuda`)
- Detect the HIP runtime under `ROCM_PATH` (default `/opt/rocm`) or the system paths

| Option | Default | Meaning |
|---|---|---|
| `SIMPLE_S3_RDMA_GPU` | `CUDA` when `cuda.h` and the libcuda stub are found, else `HIP` when the HIP runtime is found, else `OFF` | GPU runtime for GPU memory (`--gpu`, `--list-gpus`): `CUDA`, `HIP` or `OFF`. |
| `SIMPLE_S3_RDMA_WITH_CUOBJ` | ON when libcuobjclient is found and `SIMPLE_S3_RDMA_GPU` is `CUDA` | Build the NVIDIA cuObject flavor. Needs `SIMPLE_S3_RDMA_GPU=CUDA`. |
| `CUDA_TOOLKIT_ROOT_DIR` | `/usr/local/cuda` | Where to look for the CUDA headers, `cuobjclient.h` and `libcuobjclient`. |
| `ROCM_PATH` | `/opt/rocm` | Where to look for the HIP runtime, before the system paths. |
| `SIMPLE_S3_RDMA_WITH_RC` | ON when libibverbs is found | Build the AMD hipObject RC flavor. |

Setting an option to `ON` on a host where its library cannot be found fails the
configure step instead of silently dropping the feature. An AMD build (HIP GPU
memory, hipObject RC flavor) on a host that also has CUDA installed:

```bash
cmake -DCMAKE_CXX_COMPILER=g++-14 -DSIMPLE_S3_RDMA_GPU=HIP -DSIMPLE_S3_RDMA_WITH_CUOBJ=OFF ..
```

On a host without CUDA, `SIMPLE_S3_RDMA_GPU` already defaults to `HIP` (or
`OFF`) and the cuObject flavor is left out.

### Troubleshooting

**libcurl version too old:**
```bash
curl --version  # Should be >= 7.75.0
```

**CUDA not found:**
```bash
# Set CUDA path if not in default location
cmake -DCUDA_TOOLKIT_ROOT_DIR=/path/to/cuda ..
```

**spdlog not found:**
```bash
# Install spdlog
sudo apt-get install libspdlog-dev
# Or build from source: https://github.com/gabime/spdlog
```

**GCC 15 compiler crash (Ubuntu 26.04 with GCC 15):**

If you encounter an "internal compiler error: Segmentation fault" when building OpenTelemetry C++, this is a known GCC 15 bug. The build script automatically works around this by using GCC 14 when it detects Ubuntu 26.04 with GCC 15. If building manually:

```bash
# Install GCC 14
sudo apt-get install gcc-14 g++-14

# Use GCC 14 for the build
export CC=/usr/bin/gcc-14
export CXX=/usr/bin/g++-14

# Then proceed with vcpkg install and build
./scripts/build-ubuntu.sh
```

## Usage

### Quick Start

**TCP Mode (no special setup required):**
```bash
# Set environment variables
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
export S3_ENDPOINT=https://s3.example.com:8111

# PUT 10 objects of 1GiB each using TCP
./build/simple_s3_rdma put --protocol tcp --obj.size 1GiB --count 10

# GET the same objects back using TCP
./build/simple_s3_rdma get --protocol tcp --obj.size 1GiB --count 10
```

**RDMA Mode (requires CUFILE_ENV_PATH_JSON):**
```bash
# Set environment variables (including CUFILE config for RDMA)
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
export S3_ENDPOINT=https://s3.example.com:8111
export CUFILE_ENV_PATH_JSON=/tmp/cufile.json

# PUT 10 objects of 1GiB each using RDMA
./build/simple_s3_rdma put --protocol rdma --obj.size 1GiB --count 10

# GET the same objects back using RDMA
./build/simple_s3_rdma get --protocol rdma --obj.size 1GiB --count 10
```

### Command Line Syntax

```bash
./simple_s3_rdma <put|get> [options]
```

### Options

- `--access-key <key>`: AWS Access Key ID (default: $AWS_ACCESS_KEY_ID)
- `--secret-key <key>`: AWS Secret Access Key (default: $AWS_SECRET_ACCESS_KEY)
- `--endpoint <url>`: S3 endpoint URL (default: $S3_ENDPOINT)
- `--bucket-name <name>`: S3 bucket name (default: test-bucket)
- `--protocol <tcp|rdma>`: Transfer protocol (default: rdma)
- `--flavor <nvidia|amd>`: RDMA flavor (default: nvidia)
- `--rc-device <name>`: Verbs device for `--flavor amd` (default: first device)
- `--rc-gid-index <n>`: GID index for `--flavor amd` (default: first RoCEv2 IPv4 GID, 0 on InfiniBand)
- `--obj.size <size>`: Object size with units (e.g., 1MiB, 10GiB, 100kiB)
- `--count <n>`: Number of objects to transfer (default: 1)
- `--threads <n>`: Number of concurrent threads (default: 1)
- `--gpu <id>`: GPU device ID to use for memory allocation (enables GPU memory)
- `--list-gpus`: List available GPU devices and exit
- `--verify`: Verify object content against the server's ETag; GET buffers are cleared first (see [Content Verification](#content-verification))
- `--load-balance <ips>`: Comma-separated list of IP addresses for round-robin load balancing (e.g., 192.168.1.1,192.168.1.2)
- `--trace-output <file>`: OTEL trace output file (optional)
- `--trace-sampling <ratio>`: Trace sampling ratio 0.0-1.0 (default: 1.0)
- `--log-output <file>`: Log output file (default: stderr)
- `--log-level <level>`: Log level: TRACE, DEBUG, INFO, WARN, ERROR, case-insensitive (default: INFO). TRACE additionally turns on libcurl's verbose wire logging

### Examples

**PUT Operation (TCP mode):**
```bash
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
export S3_ENDPOINT=https://s3.example.com:8111

./simple_s3_rdma put --protocol tcp --bucket-name my-bucket --obj.size 1GiB --count 10
```

**PUT Operation (RDMA mode):**
```bash
export CUFILE_ENV_PATH_JSON=/path/to/cufile.json
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
export S3_ENDPOINT=https://s3.example.com:8111

./simple_s3_rdma put --protocol rdma --bucket-name my-bucket --obj.size 1GiB --count 10
```

**GET Operation:**
```bash
./simple_s3_rdma get --bucket-name my-bucket --obj.size 1GiB --count 10 --log-level DEBUG
```

**With Custom Logging:**
```bash
./simple_s3_rdma put --obj.size 100MiB --count 5 --log-output /tmp/s3-rdma.log --log-level INFO
```

**With Load Balancing:**
```bash
# Round-robin requests across multiple S3 server IPs
./simple_s3_rdma put --obj.size 1GiB --count 10 --load-balance 192.168.1.10,192.168.1.11,192.168.1.12
```

### GPU Memory Support

The client supports allocating buffers directly in GPU memory, which is essential for GPU-direct RDMA transfers that bypass system memory entirely. The GPU runtime is picked at build time (`SIMPLE_S3_RDMA_GPU`): CUDA for NVIDIA GPUs, HIP for AMD GPUs.

**List Available GPUs:**
```bash
./simple_s3_rdma --list-gpus
```

Output example:
```
Available CUDA devices:

GPU 0: Quadro RTX 4000
  Compute Capability: 7.5
  Total Memory: 7.60376 GiB (8164474880 bytes)
  Free Memory: 7.49249 GiB (8045002752 bytes)
  PCI Bus ID: 2:0
  BAR1 (GPU-direct RDMA window): 256 MiB total, 241 MiB free
```

**Using GPU Memory:**
```bash
# PUT operation using GPU 0 memory
./simple_s3_rdma put --gpu 0 --obj.size 1GiB --count 10

# GET operation using GPU 0 memory
./simple_s3_rdma get --gpu 0 --obj.size 1GiB --count 10
```

**Notes:**
- When `--gpu <id>` is specified, all buffers are allocated on the specified GPU device, using `cuMemAlloc()` (CUDA) or `hipMalloc()` (HIP)
- Without `--gpu`, buffers are allocated in system memory using huge pages (default)
- GPU memory allocation requires the GPU driver: NVIDIA's for CUDA, amdgpu for HIP
- The cuObject flavor registers GPU buffers through cuObject (dma-buf or nvidia-peermem, per `cufile.json`)
- The hipObject RC flavor exports GPU buffers as dma-bufs and registers them with `ibv_reg_dmabuf_mr`, so it needs no peer-memory kernel module. Either runtime works with it
- The GPU device ID can be found using `--list-gpus`
- The NIC reaches GPU memory through a PCIe BAR window, shown by `--list-gpus` and at startup: BAR1 on NVIDIA (read through NVML), visible VRAM on AMD (amdgpu sysfs). Over RDMA, all GPU buffers (threads x object size) must fit in its free space. On NVIDIA a run that cannot fit stops before allocating; on AMD it only warns, since the driver may place the buffers elsewhere. Buffers just under the limit can still fail registration because of mapping overhead
- To enlarge the window, turn on Resizable BAR (and Above 4G Decoding) in the BIOS. The card must support the larger size: `/sys/bus/pci/devices/<bdf>/resource<N>_resize` lists the sizes it accepts as a bitmask (bit n = 2^n MiB). An RX 9070 XT exposes all 16 GiB, while a Quadro RTX 4000 tops out at 256 MiB
- GPU memory is ideal for GPUDirect RDMA workloads that keep data on the GPU

**Performance:**
- GPU memory performance is within 10% of system memory for most workloads
- Larger objects (10+ MiB) show performance parity
- Network/S3 transfer is typically the bottleneck, not memory type
- See `GPU_TESTING_RESULTS.md` for detailed benchmarks

### Content Verification

By default the benchmark reports success on any HTTP 2xx response, and the
"Total bytes transferred" figure is the requested object size summed locally --
neither number reflects what the server actually stored. `--verify` closes that
gap:

```bash
# Confirm the bytes that reach the server match what we intended to send
./simple_s3_rdma put --gpu 0 --obj.size 10MiB --count 5 --verify

# Confirm the bytes that land in GPU memory match what the server sent
./simple_s3_rdma get --gpu 0 --obj.size 10MiB --count 5 --verify
```

For a single-part object S3 sets the ETag to the hex MD5 of the stored bytes, so
the check needs no extra round trip:

- **PUT** hashes the payload on the host before it is copied into device memory,
  then compares that digest against the ETag in the PUT response. A fault
  anywhere in between -- including the GPU staging copies -- shows up as a
  mismatch.
- **GET** copies the received object back out of device memory, hashes it, and
  compares against the ETag on the GET response.

Failures are logged per object and counted in the summary; a verification
mismatch marks the operation failed. Objects with a multipart ETag (`<digest>-<n>`)
are skipped with a warning, since that value is not a digest of the object.

**The ETag depends on how the object was uploaded.** Infinia always gives an
object written over RDMA a UUID rather than an MD5 as its ETag, deliberately, so
that computing a digest does not serialize the transfer. An object written over
TCP gets an MD5 by default, but an Infinia dataset can be configured to use UUID
ETags for every object. A UUID ETag is 32 hex digits, like an MD5, so `--verify`
cannot tell the two apart:

- **RDMA PUTs are not verified.** There is no content digest to compare against:
  `--verify` logs the payload MD5 for each such PUT, so it can be checked against
  a later download, and reports how many it skipped rather than failing them. A
  PUT that the server sends back to TCP is a TCP upload and is verified as one.
- **GETs are verified against the ETag of the stored object**, whether the GET
  itself runs over RDMA or TCP. Upload GET test objects over TCP; GETs of objects
  written over RDMA report a mismatch.
- **On a dataset that uses UUID ETags**, TCP PUTs and all GETs report a mismatch,
  so `--verify` cannot be used there.

This is worth enabling whenever the transfer path changes -- a new staging size,
a different protocol, or a driver update -- and leaving off for throughput runs,
where the GET-side readback and hashing cost real time.


## hipObject RC flavor

`--flavor amd` implements the `hipobj-rc-v2` protocol. Every transfer is three
SigV4-signed POSTs with an empty body, each carrying
`x-amz-rdma-protocol: hipobj-rc-v2`:

1. `POST /.hipobj-rc/prepare` names the operation, target, size, the client's
   initial PSN, a random cookie and an 88-hex-digit token describing the
   client QP, GID and buffer. The server answers with its own token (QPN, GID,
   LID), a session id, its PSN and, for PUT, the address and rkey of its
   staging buffer.
2. The client moves its RC QP from INIT through RTR to RTS against the server
   QP.
3. `POST /.hipobj-rc/ready` hands over the client QPN and buffer. For GET the
   client posts a receive first and the server RDMA-writes the object into the
   client buffer with the cookie as immediate; the response arrives after the
   data. For PUT the client RDMA-writes the buffer into the server's staging
   buffer once the READY request is on the wire, then waits for the response.
   A 409 on a GET READY means the server could not deliver yet and is retried
   a few times on the same session.
4. On any client-side failure after PREPARE the client sends
   `POST /.hipobj-rc/cancel` for the session.

Setup time covers PREPARE and the QP pairing; transfer time covers READY and
the RDMA completion. One RC message carries the whole object, so the object
size is capped by the port's maximum message size.

Example against a soft-RoCE device:

```bash
./simple_s3_rdma put --flavor amd --rc-device rxe0 --obj.size 1MiB --count 10 --verify
./simple_s3_rdma get --flavor amd --rc-device rxe0 --obj.size 1MiB --count 10 --verify
```

With `--gpu <id>` the client buffer is GPU memory, exported as a dma-buf and
registered with `ibv_reg_dmabuf_mr`; the server sees no difference. This needs
a NIC whose driver supports dma-buf MRs, such as mlx5:

```bash
./simple_s3_rdma get --flavor amd --rc-device rocep11s0f0 --gpu 0 --obj.size 16MiB --count 10 --verify
```

## Object Naming

Objects are named using the pattern: `test_{thread}_{size}_{num}.bin`

For example:
- `test_0_1073741824_0.bin` (first 1GiB object)
- `test_0_1073741824_1.bin` (second 1GiB object)

## Performance Metrics

The client tracks and reports:

1. **Latency Statistics** (per operation):
   - Setup time (request preparation and signing)
   - Send time (data transmission)
   - Receive time (response processing)
   - Total time (end-to-end)

2. **Percentiles**: p50, p90, p95, p99 for all latency metrics

3. **Throughput**: Aggregate throughput across all operations

## Current Implementation Status

### ✅ Implemented
- **CLI parsing** using argparse library with automatic help generation
- **Huge page memory allocation** with fallback (2MB → 1GB → transparent)
- **AWS Signature V4** using libcurl's built-in CURLOPT_AWS_SIGV4
- **HTTP client** with SSL verification disabled for self-signed certificates
- **Latency tracking** with setup, transfer, and total time metrics
- **Percentile statistics** (p50, p90, p95, p99)
- **Throughput calculation** in B/s, kiB/s, MiB/s, GiB/s
- **spdlog logging** with millisecond-resolution timestamps
- **PUT operations** using CURLOPT_UPLOAD and CURLOPT_READFUNCTION
- **GET operations** using CURLOPT_WRITEFUNCTION
- **Automatic bucket creation** if bucket doesn't exist
- **Random data generation** from /dev/urandom for PUT operations

### ⏳ TODO
- RDMA token generation using cuObjClient
- RDMA-enabled PUT/GET with x-amz-rdma-token headers
- RDMA response header handling (x-amz-rdma-reply, x-amz-rdma-bytes-transferred)
- OpenTelemetry tracing integration
- Multi-threading support for concurrent operations

### 📊 Performance (Current HTTP-only Implementation)

Tested on local network with MinIO S3 server:
- **PUT**: ~160 MiB/s, ~61ms latency (10MiB objects)
- **GET**: ~148 MiB/s, ~67ms latency (10MiB objects)

## Architecture

### Design Philosophy

This implementation follows the proven pattern from `aws-c-s3/benchmarks/bench_curl_s3.c`:

1. **libcurl's AWS SigV4**: Uses `CURLOPT_AWS_SIGV4` instead of manual signature generation
2. **Callback-based I/O**:
   - PUT: `CURLOPT_READFUNCTION` to stream data from memory
   - GET: `CURLOPT_WRITEFUNCTION` to receive data into memory
3. **Connection reuse**: Single CURL handle per operation for HTTP keep-alive
4. **Minimal headers**: Only essential headers (Expect:, x-amz-content-sha256)
5. **Thread-safe**: Uses `CURLOPT_NOSIGNAL` for safe multi-threading (future)

## License

Licensed under the Apache License, Version 2.0. See the [LICENSE](LICENSE) file for details.

