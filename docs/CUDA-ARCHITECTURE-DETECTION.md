# CUDA Architecture Detection

## Overview

Modern CUDA installations (13.0+) use architecture-specific directory layouts. The build system now automatically detects and uses the correct architecture.

## Supported Architectures

| Platform | System Arch (`uname -m`) | CUDA Target Directory | Common Systems |
|----------|--------------------------|----------------------|----------------|
| x86_64   | `x86_64`, `amd64`       | `targets/x86_64-linux` | Most servers, desktops |
| ARM64 Server | `aarch64`, `arm64` | `targets/sbsa-linux` | **NVIDIA DGX**, Grace Hopper |
| ARM64 Embedded | `aarch64`, `arm64` | `targets/aarch64-linux` | Jetson AGX, Jetson Orin |
| IBM POWER | `ppc64le` | `targets/ppc64le-linux` | IBM Power systems |

## NVIDIA DGX Systems

**Important:** NVIDIA DGX systems use ARM64 processors but the CUDA target is `sbsa-linux`, **NOT** `aarch64-linux`.

- **SBSA** = Server Base System Architecture (ARM64 servers)
- **aarch64** = ARM64 embedded/consumer devices

The CMakeLists.txt is configured to prefer `sbsa-linux` over `aarch64-linux` when both are present on an ARM64 system.

## How Detection Works

The CMake configuration uses the following strategy:

1. **Auto-detection**: If only one `targets/*-linux` directory exists, use it
2. **Multiple targets**: Use `CMAKE_SYSTEM_PROCESSOR` to select:
   - `x86_64|AMD64|amd64` → `x86_64-linux`
   - `aarch64|arm64` → `sbsa-linux` (preferred) or `aarch64-linux`
   - `ppc64le` → `ppc64le-linux`
3. **Fallback**: Traditional CUDA layout (older versions without `targets/`)

## Quick Start

To find out what architecture will be used on your system:

```bash
# Run the detection script
./scripts/detect-cuda-arch.sh
```

This will show you:
- Your system architecture (`uname -m`)
- Available CUDA target directories
- Which headers are present
- What CMake will use

## Verification

### On Your Current System

Run the detection script to see what CUDA architecture is available:

```bash
./scripts/detect-cuda-arch.sh
```

Example output on x86_64:
```
System Architecture: x86_64
Expected CUDA target: x86_64-linux

Available CUDA targets:
  ✓ x86_64-linux
      Headers: 1683 files
      ✓ cuobjclient.h found
      ✓ cufile.h found
      ✓ cuda.h found
```

### On NVIDIA DGX

Expected output when you run `detect-cuda-arch.sh`:
```
System Architecture: aarch64
Expected CUDA target: sbsa-linux (server) or aarch64-linux (embedded)

Available CUDA targets:
  ✓ sbsa-linux
      Headers: 1600+ files
      ✓ cuobjclient.h found
      ✓ cufile.h found
      ✓ cuda.h found
```

### Manual Verification

You can also manually check on any system:

```bash
# Check system architecture
uname -m

# Check CUDA target directories
ls -la /usr/local/cuda/targets/

# Check if headers exist for a specific target
ls /usr/local/cuda/targets/sbsa-linux/include/ | grep -E "cuda.h|cufile.h|cuobjclient.h"
```

## CMake Build Messages

During `cmake` configuration, you'll see messages indicating which architecture was detected:

```
-- Auto-detected CUDA target architecture: sbsa-linux
-- Using modern CUDA layout: /usr/local/cuda/targets/sbsa-linux
-- CUDA Include dir: /usr/local/cuda/targets/sbsa-linux/include
-- CUDA Library dir: /usr/local/cuda/targets/sbsa-linux/lib
```

## Directory Structure Examples

### Modern CUDA Layout (13.0+)
```
/usr/local/cuda/
├── targets/
│   ├── sbsa-linux/          # ARM64 servers (DGX)
│   │   ├── include/
│   │   │   ├── cuda.h
│   │   │   ├── cufile.h
│   │   │   └── cuobjclient.h
│   │   └── lib/
│   │       ├── libcudart.so
│   │       ├── libcufile.so
│   │       └── libcuobjclient.so
│   ├── x86_64-linux/        # x86_64 systems
│   └── aarch64-linux/       # ARM64 embedded (Jetson)
└── ...
```

### Traditional CUDA Layout (older versions)
```
/usr/local/cuda/
├── include/
│   ├── cuda.h
│   └── ...
├── lib64/                   # or lib/ on non-x86_64
│   ├── libcudart.so
│   └── ...
└── ...
```

## Troubleshooting

### Wrong Architecture Detected

If CMake picks the wrong architecture, you can override it:

```bash
# Force a specific CUDA include/lib directory
cmake -DCUDA_INCLUDE_DIR=/usr/local/cuda/targets/sbsa-linux/include \
      -DCUDA_LIB_DIR=/usr/local/cuda/targets/sbsa-linux/lib \
      ..
```

### Headers Not Found

Make sure the development packages are installed:

```bash
# For CUDA 13.3
sudo apt-get install cuda-cudart-dev-13-3 cuda-driver-dev-13-3 libcufile-dev-13-3
```

### Multiple Target Directories

If multiple architecture directories exist and auto-detection fails, check:

```bash
ls -la /usr/local/cuda/targets/
```

The build system will select based on your system's `uname -m` output.
