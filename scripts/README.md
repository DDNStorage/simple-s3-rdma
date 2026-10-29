# Build Scripts

This directory contains build, setup, and maintenance scripts for the Simple S3 RDMA Client.

## build-ubuntu.sh

Automated build script for Ubuntu/Debian systems.

### What it does

1. **Installs system dependencies**:
   - build-essential, cmake, git
   - libcurl (>= 7.75.0), OpenSSL
   - spdlog, argparse

2. **Sets up vcpkg**:
   - Clones vcpkg to `/tmp/vcpkg` (or `$VCPKG_ROOT` if set)
   - Bootstraps vcpkg

3. **Installs OpenTelemetry C++**:
   - Uses vcpkg to install `opentelemetry-cpp[otlp-file]`

4. **Checks for CUDA**:
   - Verifies CUDA installation at `/usr/local/cuda`
   - Warns if not found (RDMA features require CUDA)

5. **Builds the application**:
   - Creates build directory
   - Runs cmake with vcpkg toolchain
   - Compiles with make using all available cores

### Usage

```bash
# From project root
./scripts/build-ubuntu.sh

# Or with custom vcpkg location
VCPKG_ROOT=/opt/vcpkg ./scripts/build-ubuntu.sh
```

### Requirements

- Ubuntu 20.04+ or Debian 11+
- sudo access (for installing packages)
- Internet connection (for downloading dependencies)

### Environment Variables

- `VCPKG_ROOT`: Custom vcpkg installation directory (default: `/tmp/vcpkg`)

### Exit Codes

- `0`: Build successful
- `1`: Build failed or error occurred

### Notes

- The script is **idempotent** - safe to run multiple times
- Already installed dependencies will be skipped
- vcpkg packages are cached and won't be reinstalled
- The script will **not** run as root (safety check)

## clean.sh

Removes all build artifacts and generated files to provide a clean slate.

### What it does

1. **Removes build directories**:
   - `build/`
   - `cmake-build-debug/`
   - `build-debug/`
   - `cmake-build-release/`
   - `out/` (Visual Studio)

2. **Removes CMake cache files**:
   - `CMakeCache.txt`
   - `CMakeFiles/`
   - `cmake_install.cmake`
   - Generated `Makefile`
   - `compile_commands.json`

3. **Removes compiled artifacts**:
   - Object files (`*.o`)
   - Static libraries (`*.a`)
   - Shared libraries (`*.so`, `*.so.*`)

4. **Removes log and temporary files**:
   - `cufile.log`
   - `*.log` files
   - Editor backup files (`*~`, `*.swp`, `*.swo`)
   - `.DS_Store` (macOS)

5. **Optional: Clean vcpkg cache** (with `--all` flag):
   - vcpkg buildtrees
   - vcpkg packages
   - vcpkg downloads

### Usage

```bash
# Clean build artifacts only (fast)
./scripts/clean.sh

# Clean everything including vcpkg cache (slower next build)
./scripts/clean.sh --all
# or
./scripts/clean.sh -a
```

### When to use

- **After build errors**: Clean state can help resolve CMake configuration issues
- **Before fresh build**: Ensure no stale artifacts interfere
- **Disk space cleanup**: Remove large build directories and caches
- **Switching branches**: Clean build when changing between major branches

### Notes

- Safe to run - only removes generated/temporary files
- Source code is never touched
- vcpkg installation itself is preserved (only build cache is cleaned with `--all`)
- After cleaning, rebuild with `./scripts/build-ubuntu.sh`

## detect-cuda-arch.sh

Diagnostic script to detect and display CUDA architecture information.

### What it does

1. **Detects system architecture**: `x86_64`, `aarch64`, `ppc64le`
2. **Lists available CUDA targets**: Modern vs. traditional layout
3. **Checks for required headers**: `cuobjclient.h`, `cufile.h`, `cuda.h`
4. **Provides recommendations**: Which target CMake will use

### Usage

```bash
./scripts/detect-cuda-arch.sh
```

### Example output

```
=== CUDA Architecture Detection ===

System Architecture: x86_64
Expected CUDA target: x86_64-linux

CUDA Root: /usr/local/cuda
CUDA Real Path: /usr/local/cuda

Modern CUDA layout detected (targets directory exists)

Available CUDA targets:
  ✓ x86_64-linux
      Headers: 1683 files in /usr/local/cuda/targets/x86_64-linux/include
      Libraries: 247 files in /usr/local/cuda/targets/x86_64-linux/lib
      ✓ cuobjclient.h found
      ✓ cufile.h found
      ✓ cuda.h found

=== Recommendation ===
This is an x86_64 system.
✓ x86_64 target found
  CMake will use: /usr/local/cuda/targets/x86_64-linux
```

### When to use

- **Before building**: Verify CUDA is properly installed
- **Debugging build issues**: Check if headers are present
- **Multi-architecture systems**: Verify which target will be used
- **Documentation**: Understand your CUDA installation layout

## test-arch-detection.sh

Test script that simulates CMake's architecture detection logic.

### What it does

Runs test cases to demonstrate how the build system selects CUDA targets on different platforms:
- x86_64 systems
- DGX systems (ARM64 with sbsa-linux)
- Jetson systems (ARM64 with aarch64-linux)
- POWER systems (ppc64le)

### Usage

```bash
./scripts/test-arch-detection.sh
```

### When to use

- **Understanding the build system**: See how architecture detection works
- **Verifying logic**: Ensure correct target selection for your platform
- **Before cross-compilation**: Understand what will happen on target system

