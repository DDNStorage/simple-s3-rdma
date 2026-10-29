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

# Script to detect CUDA target architecture
# Useful for verifying the architecture on different platforms (x86_64, DGX/ARM64, POWER, etc.)

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== CUDA Architecture Detection ===${NC}"
echo ""

# Detect system architecture
SYSTEM_ARCH=$(uname -m)
echo -e "${GREEN}System Architecture:${NC} ${SYSTEM_ARCH}"

# Map common architecture names
case "${SYSTEM_ARCH}" in
    x86_64|amd64|AMD64)
        EXPECTED_CUDA_ARCH="x86_64-linux"
        ;;
    aarch64|arm64|ARM64)
        EXPECTED_CUDA_ARCH="sbsa-linux (server) or aarch64-linux (embedded)"
        ;;
    ppc64le)
        EXPECTED_CUDA_ARCH="ppc64le-linux"
        ;;
    *)
        EXPECTED_CUDA_ARCH="unknown"
        ;;
esac
echo -e "${GREEN}Expected CUDA target:${NC} ${EXPECTED_CUDA_ARCH}"
echo ""

# Check CUDA installation
CUDA_ROOT="/usr/local/cuda"
if [ ! -d "${CUDA_ROOT}" ]; then
    echo -e "${RED}ERROR: CUDA not found at ${CUDA_ROOT}${NC}"
    exit 1
fi

echo -e "${GREEN}CUDA Root:${NC} ${CUDA_ROOT}"
CUDA_REAL_PATH=$(readlink -f "${CUDA_ROOT}")
echo -e "${GREEN}CUDA Real Path:${NC} ${CUDA_REAL_PATH}"
echo ""

# Check for targets directory (modern CUDA layout)
if [ -d "${CUDA_ROOT}/targets" ]; then
    echo -e "${YELLOW}Modern CUDA layout detected (targets directory exists)${NC}"
    echo ""
    echo "Available CUDA targets:"
    for target_dir in "${CUDA_ROOT}"/targets/*-linux; do
        if [ -d "${target_dir}" ]; then
            target_name=$(basename "${target_dir}")
            echo -e "  ${GREEN}✓${NC} ${target_name}"
            
            # Check for include and lib directories
            if [ -d "${target_dir}/include" ]; then
                num_headers=$(find "${target_dir}/include" -name "*.h" | wc -l)
                echo -e "      Headers: ${num_headers} files in ${target_dir}/include"
            fi
            if [ -d "${target_dir}/lib" ]; then
                num_libs=$(find "${target_dir}/lib" -name "*.so*" -o -name "*.a" | wc -l)
                echo -e "      Libraries: ${num_libs} files in ${target_dir}/lib"
            fi
            
            # Check for specific headers we need
            if [ -f "${target_dir}/include/cuobjclient.h" ]; then
                echo -e "      ${GREEN}✓ cuobjclient.h found${NC}"
            else
                echo -e "      ${YELLOW}✗ cuobjclient.h not found${NC}"
            fi
            if [ -f "${target_dir}/include/cufile.h" ]; then
                echo -e "      ${GREEN}✓ cufile.h found${NC}"
            else
                echo -e "      ${YELLOW}✗ cufile.h not found${NC}"
            fi
            if [ -f "${target_dir}/include/cuda.h" ]; then
                echo -e "      ${GREEN}✓ cuda.h found${NC}"
            else
                echo -e "      ${YELLOW}✗ cuda.h not found${NC}"
            fi
            echo ""
        fi
    done
else
    echo -e "${YELLOW}Traditional CUDA layout detected (no targets directory)${NC}"
    echo ""
    
    # Check traditional locations
    if [ -d "${CUDA_ROOT}/include" ]; then
        num_headers=$(find "${CUDA_ROOT}/include" -name "*.h" | wc -l)
        echo -e "${GREEN}Include directory:${NC} ${CUDA_ROOT}/include (${num_headers} headers)"
    fi
    
    if [ -d "${CUDA_ROOT}/lib64" ]; then
        num_libs=$(find "${CUDA_ROOT}/lib64" -name "*.so*" -o -name "*.a" | wc -l)
        echo -e "${GREEN}Library directory:${NC} ${CUDA_ROOT}/lib64 (${num_libs} libraries)"
    elif [ -d "${CUDA_ROOT}/lib" ]; then
        num_libs=$(find "${CUDA_ROOT}/lib" -name "*.so*" -o -name "*.a" | wc -l)
        echo -e "${GREEN}Library directory:${NC} ${CUDA_ROOT}/lib (${num_libs} libraries)"
    fi
fi

echo ""
echo -e "${BLUE}=== Recommendation ===${NC}"
if [ "${SYSTEM_ARCH}" = "aarch64" ] || [ "${SYSTEM_ARCH}" = "arm64" ]; then
    echo -e "${YELLOW}This is an ARM64 system.${NC}"
    if [ -d "${CUDA_ROOT}/targets/sbsa-linux" ]; then
        echo -e "${GREEN}✓ NVIDIA Server SBSA target found - this is likely a DGX or server platform${NC}"
        echo -e "  CMake will use: ${CUDA_ROOT}/targets/sbsa-linux"
    elif [ -d "${CUDA_ROOT}/targets/aarch64-linux" ]; then
        echo -e "${YELLOW}✓ ARM64 aarch64 target found - this is likely an embedded/Jetson platform${NC}"
        echo -e "  CMake will use: ${CUDA_ROOT}/targets/aarch64-linux"
    else
        echo -e "${RED}✗ No ARM64 CUDA target found${NC}"
    fi
elif [ "${SYSTEM_ARCH}" = "x86_64" ]; then
    echo -e "${GREEN}This is an x86_64 system.${NC}"
    if [ -d "${CUDA_ROOT}/targets/x86_64-linux" ]; then
        echo -e "${GREEN}✓ x86_64 target found${NC}"
        echo -e "  CMake will use: ${CUDA_ROOT}/targets/x86_64-linux"
    else
        echo -e "${YELLOW}No x86_64 target directory, likely using traditional layout${NC}"
    fi
fi

echo ""
echo -e "${BLUE}The updated CMakeLists.txt will automatically detect and use the correct architecture.${NC}"
