#!/bin/bash
# Copyright 2026 DataDirect Networks, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Show help if requested
if [ "$1" == "--help" ] || [ "$1" == "-h" ]; then
    echo "Simple S3 RDMA Client - Ubuntu Build Script"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "This script automates the build process for the simple-s3-rdma application on Ubuntu."
    echo ""
    echo "The script performs the following steps:"
    echo "  1. Installs system dependencies (libcurl, OpenSSL, spdlog, argparse, etc.)"
    echo "  2. Sets up vcpkg and installs OpenTelemetry C++"
    echo "  3. Checks for CUDA installation and GDS libraries"
    echo "  4. Builds the application using CMake and Make"
    echo ""
    echo "Options:"
    echo "  -h, --help     Show this help message and exit"
    echo ""
    echo "Requirements:"
    echo "  - Ubuntu 20.04 or later"
    echo "  - Internet connection (for downloading dependencies)"
    echo "  - CUDA toolkit installed (for the NVIDIA cuObject RDMA flavor)"
    echo "  - Approximately 2GB of disk space for vcpkg and dependencies"
    echo ""
    echo "Notes:"
    echo "  - The script will use 'sudo' for installing system packages"
    echo "  - vcpkg will be installed to /tmp/vcpkg (can be changed via VCPKG_ROOT env var)"
    echo "  - On Ubuntu 26.04 with GCC 15, GCC 14 is automatically used to avoid compiler bugs"
    echo "  - The script is idempotent - safe to run multiple times"
    echo ""
    echo "Examples:"
    echo "  $0                    # Build with default settings"
    echo "  VCPKG_ROOT=~/vcpkg $0 # Use custom vcpkg location"
    echo ""
    exit 0
fi

echo -e "${GREEN}=== Simple S3 RDMA Client - Ubuntu Build Script ===${NC}"
echo ""

# Check if running as root
if [ "$EUID" -eq 0 ]; then
    echo -e "${RED}ERROR: Do not run this script as root (use sudo only for apt commands)${NC}"
    exit 1
fi

# Detect Ubuntu version
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo -e "${GREEN}Detected OS: $NAME $VERSION${NC}"
else
    echo -e "${YELLOW}Warning: Could not detect OS version${NC}"
fi

# Workaround for GCC 15 compiler bug on Ubuntu 26.04
# GCC 15 has a known issue causing segfaults when compiling opentelemetry-cpp
# See: https://gcc.gnu.org/bugzilla/ (internal compiler error with complex template instantiations)
# Check if we're on Ubuntu 26.04 and if the default GCC is version 15
if [ -n "$VERSION_ID" ] && [ "$VERSION_ID" == "26.04" ]; then
    # Check if default GCC is version 15 (extract major version number)
    GCC_VERSION=$(gcc --version 2>/dev/null | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1 | cut -d. -f1)
    if [ "$GCC_VERSION" == "15" ]; then
        echo -e "${YELLOW}Detected Ubuntu 26.04 with GCC 15 - using GCC 14 to avoid compiler bugs${NC}"
        export CC=/usr/bin/gcc-14
        export CXX=/usr/bin/g++-14

        # Check if GCC 14 is installed, if not, install it
        if ! command -v g++-14 &> /dev/null; then
            echo "Installing gcc-14 and g++-14..."
            sudo apt-get update
            sudo apt-get install -y gcc-14 g++-14
        fi
        echo -e "${GREEN}Using GCC 14: $(g++-14 --version | head -1)${NC}"
    fi
fi

# Step 1: Install system dependencies
echo ""
echo -e "${GREEN}Step 1: Installing system dependencies (including static libraries)...${NC}"
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    pkg-config \
    libcurl4-openssl-dev \
    libssl-dev \
    libspdlog-dev \
    libargparse-dev \
    zlib1g-dev \
    libnghttp2-dev \
    libkrb5-dev \
    libibverbs-dev

# Check libcurl version
echo ""
echo -e "${GREEN}Checking libcurl version (need >= 7.75.0)...${NC}"
CURL_VERSION=$(curl-config --version | awk '{print $2}')
echo "Found libcurl version: $CURL_VERSION"

# Step 2: Install/setup vcpkg for OpenTelemetry
echo ""
echo -e "${GREEN}Step 2: Setting up vcpkg for OpenTelemetry...${NC}"

VCPKG_ROOT="${VCPKG_ROOT:-/tmp/vcpkg}"
export VCPKG_ROOT

if [ ! -d "$VCPKG_ROOT" ]; then
    echo "Installing vcpkg to $VCPKG_ROOT..."
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    "$VCPKG_ROOT/bootstrap-vcpkg.sh"
else
    echo "vcpkg already installed at $VCPKG_ROOT"
fi

# Step 3: Install OpenTelemetry via vcpkg
echo ""
echo -e "${GREEN}Step 3: Installing OpenTelemetry C++ via vcpkg...${NC}"
if ! "$VCPKG_ROOT/vcpkg" list | grep -q "opentelemetry-cpp"; then
    echo "Installing opentelemetry-cpp[otlp-file]..."
    "$VCPKG_ROOT/vcpkg" install 'opentelemetry-cpp[otlp-file]'
else
    echo "opentelemetry-cpp already installed"
fi

# Step 4: Check for CUDA and install GDS libraries if needed
echo ""
echo -e "${GREEN}Step 4: Checking for CUDA/GDS installation...${NC}"
if [ -d "/usr/local/cuda" ]; then
    # Try multiple methods to detect CUDA version
    CUDA_VERSION=""

    # Method 1: Check version.txt (older CUDA installations)
    if [ -f "/usr/local/cuda/version.txt" ]; then
        CUDA_VERSION=$(cat /usr/local/cuda/version.txt)
    # Method 2: Check version.json (newer CUDA installations)
    elif [ -f "/usr/local/cuda/version.json" ]; then
        CUDA_VERSION=$(grep -o '"cuda_[^"]*"' /usr/local/cuda/version.json | head -1 | cut -d'"' -f2 | sed 's/cuda_//')
    # Method 3: Try nvcc if available
    elif command -v nvcc &> /dev/null; then
        CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $5}' | cut -d',' -f1)
    # Method 4: Check installed CUDA packages (Debian/Ubuntu)
    elif dpkg -l 2>/dev/null | grep -q "cuda-toolkit-config-common"; then
        CUDA_VERSION=$(dpkg -l | grep "^ii.*cuda-toolkit-[0-9].*-config-common" | awk '{print $2}' | grep -o '[0-9]\+-[0-9]\+' | head -1 | sed 's/-/./')
    else
        CUDA_VERSION="unknown"
    fi

    echo -e "${GREEN}Found CUDA at /usr/local/cuda (version: $CUDA_VERSION)${NC}"

    # Determine the correct libcuobjclient-dev package based on CUDA version
    # Extract major.minor version (e.g., "13.1" from "CUDA Version 13.1.105")
    CUDA_MAJOR_MINOR=""
    if [[ "$CUDA_VERSION" =~ ([0-9]+)\.([0-9]+) ]]; then
        CUDA_MAJOR="${BASH_REMATCH[1]}"
        CUDA_MINOR="${BASH_REMATCH[2]}"
        CUDA_MAJOR_MINOR="${CUDA_MAJOR}-${CUDA_MINOR}"
    fi

    # Check if any libcuobjclient-dev package is already installed
    INSTALLED_CUOBJ=$(dpkg -l 2>/dev/null | grep "^ii.*libcuobjclient-dev-[0-9]" | awk '{print $2}' | head -1)

    if [ -n "$INSTALLED_CUOBJ" ]; then
        echo -e "${GREEN}CuObjClient development libraries already installed: $INSTALLED_CUOBJ${NC}"
    else
        # Try to install the version-specific package if we detected the version
        if [ -n "$CUDA_MAJOR_MINOR" ]; then
            CUOBJ_PACKAGE="libcuobjclient-dev-${CUDA_MAJOR_MINOR}"
            echo -e "${YELLOW}Attempting to install CuObjClient for CUDA ${CUDA_MAJOR}.${CUDA_MINOR}: ${CUOBJ_PACKAGE}...${NC}"

            # Check if the package exists in apt
            if apt-cache show "$CUOBJ_PACKAGE" &> /dev/null; then
                sudo apt-get install -y "$CUOBJ_PACKAGE"
            else
                echo -e "${YELLOW}Package $CUOBJ_PACKAGE not found in apt.${NC}"
                echo -e "${YELLOW}Available libcuobjclient-dev packages:${NC}"
                apt-cache search libcuobjclient-dev | grep "^libcuobjclient-dev"
                echo -e "${YELLOW}Please install manually: sudo apt-get install libcuobjclient-dev-XX-X${NC}"
            fi
        else
            echo -e "${YELLOW}Could not determine CUDA version.${NC}"
            echo -e "${YELLOW}Available libcuobjclient-dev packages:${NC}"
            apt-cache search libcuobjclient-dev | grep "^libcuobjclient-dev"
            echo -e "${YELLOW}Please install manually: sudo apt-get install libcuobjclient-dev-XX-X${NC}"
        fi
    fi
else
    echo -e "${YELLOW}Warning: CUDA not found at /usr/local/cuda${NC}"
    echo -e "${YELLOW}The NVIDIA cuObject flavor will not be built without CUDA toolkit (--flavor amd still works)${NC}"
    echo -e "${YELLOW}Install CUDA from: https://developer.nvidia.com/cuda-downloads${NC}"
fi

# Step 5: Build the application
echo ""
echo -e "${GREEN}Step 5: Building simple-s3-rdma...${NC}"

# Get the script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

cd "$PROJECT_ROOT"

# Clean up any stale CMake cache files in the project root
# (these can interfere with the build if cmake was previously run from the wrong directory)
if [ -f "CMakeCache.txt" ] || [ -d "CMakeFiles" ]; then
    echo "Cleaning up stale CMake cache files in project root..."
    rm -rf CMakeCache.txt CMakeFiles
fi

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Running cmake..."
cmake -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ..

# Build
echo "Building..."
make -j$(nproc)

# Step 6: Verify build
echo ""
echo -e "${GREEN}Step 6: Verifying build...${NC}"
if [ -f "./simple_s3_rdma" ]; then
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo ""
    echo "Binary location: $PROJECT_ROOT/build/simple_s3_rdma"
    echo ""
    echo "Run with: ./build/simple_s3_rdma --help"
else
    echo -e "${RED}✗ Build failed - binary not found${NC}"
    exit 1
fi

# Step 7: Display usage information
echo ""
echo -e "${GREEN}=== Build Complete ===${NC}"
echo ""
echo "Next steps:"
echo "  1. Set required environment variables:"
echo "     export AWS_ACCESS_KEY_ID=your_access_key"
echo "     export AWS_SECRET_ACCESS_KEY=your_secret_key"
echo "     export S3_ENDPOINT=https://your-s3-endpoint"
echo ""
echo "  2. Run the application in TCP mode (no CUFILE setup needed):"
echo "     cd $PROJECT_ROOT"
echo "     ./build/simple_s3_rdma put --protocol tcp --obj.size 1MiB --count 10"
echo ""
echo "  3. For RDMA mode, also set:"
echo "     export CUFILE_ENV_PATH_JSON=/path/to/cufile.json"
echo "     ./build/simple_s3_rdma put --protocol rdma --obj.size 1MiB --count 10"
echo ""
echo "  4. For help:"
echo "     ./build/simple_s3_rdma --help"
echo ""

