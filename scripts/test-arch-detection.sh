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

# Test script to demonstrate architecture detection logic
# This simulates what CMake will do on different platforms

set -e

echo "=== Testing CUDA Architecture Detection Logic ==="
echo ""

# Test function
test_arch_detection() {
    local system_arch=$1
    local available_targets=$2
    local expected_result=$3
    
    echo "Test: System Architecture = ${system_arch}"
    echo "      Available CUDA targets = ${available_targets}"
    
    # Simulate CMake logic
    local selected_arch=""
    
    case "${system_arch}" in
        x86_64|AMD64|amd64)
            if echo "${available_targets}" | grep -q "x86_64-linux"; then
                selected_arch="x86_64-linux"
            fi
            ;;
        aarch64|arm64|ARM64)
            # Prefer sbsa-linux over aarch64-linux
            if echo "${available_targets}" | grep -q "sbsa-linux"; then
                selected_arch="sbsa-linux"
            elif echo "${available_targets}" | grep -q "aarch64-linux"; then
                selected_arch="aarch64-linux"
            fi
            ;;
        ppc64le)
            if echo "${available_targets}" | grep -q "ppc64le-linux"; then
                selected_arch="ppc64le-linux"
            fi
            ;;
    esac
    
    echo "      Selected target = ${selected_arch}"
    
    if [ "${selected_arch}" = "${expected_result}" ]; then
        echo "      ✓ PASS"
    else
        echo "      ✗ FAIL (expected: ${expected_result})"
    fi
    echo ""
}

# Test cases
echo "--- Test Cases ---"
echo ""

# x86_64 system with x86_64-linux target
test_arch_detection "x86_64" "x86_64-linux" "x86_64-linux"

# DGX system (ARM64) with sbsa-linux target
test_arch_detection "aarch64" "sbsa-linux" "sbsa-linux"

# Jetson system (ARM64) with aarch64-linux target
test_arch_detection "aarch64" "aarch64-linux" "aarch64-linux"

# DGX system with both targets (should prefer sbsa)
test_arch_detection "aarch64" "sbsa-linux aarch64-linux" "sbsa-linux"

# POWER system
test_arch_detection "ppc64le" "ppc64le-linux" "ppc64le-linux"

echo "=== Summary ==="
echo ""
echo "Key Points:"
echo "  1. On DGX (ARM64), the build system will use 'sbsa-linux' target"
echo "  2. On Jetson (ARM64), it will use 'aarch64-linux' if that's the only option"
echo "  3. If both ARM64 targets exist, 'sbsa-linux' is preferred"
echo "  4. You can verify on your DGX by running: ./scripts/detect-cuda-arch.sh"
echo ""
echo "Expected on NVIDIA DGX:"
echo "  - System Architecture: aarch64"
echo "  - CUDA Target: sbsa-linux"
echo "  - Include: /usr/local/cuda/targets/sbsa-linux/include"
echo "  - Lib: /usr/local/cuda/targets/sbsa-linux/lib"
