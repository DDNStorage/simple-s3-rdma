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

# Clean script for simple-s3-rdma project
# Removes all build artifacts and generated files

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Simple S3 RDMA Client - Clean Script ===${NC}"
echo ""

# Get the script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

cd "$PROJECT_ROOT"

# Function to remove directory/file if it exists
remove_if_exists() {
    local path="$1"
    local description="$2"
    
    if [ -e "$path" ]; then
        echo -e "${YELLOW}Removing ${description}:${NC} $path"
        rm -rf "$path"
    fi
}

# Check for --all flag (cleans vcpkg too)
CLEAN_VCPKG=false
if [ "$1" = "--all" ] || [ "$1" = "-a" ]; then
    CLEAN_VCPKG=true
    echo -e "${YELLOW}Running full clean (including vcpkg)...${NC}"
    echo ""
fi

echo -e "${GREEN}Step 1: Removing build directories...${NC}"
remove_if_exists "build" "build directory"
remove_if_exists "cmake-build-debug" "cmake-build-debug directory"
remove_if_exists "build-debug" "build-debug directory"
remove_if_exists "cmake-build-release" "cmake-build-release directory"
remove_if_exists "out" "out directory (Visual Studio)"

echo ""
echo -e "${GREEN}Step 2: Removing CMake cache files from project root...${NC}"
remove_if_exists "CMakeCache.txt" "CMake cache"
remove_if_exists "CMakeFiles" "CMake files directory"
remove_if_exists "cmake_install.cmake" "CMake install file"
remove_if_exists "Makefile" "generated Makefile"
remove_if_exists "compile_commands.json" "compile commands"

echo ""
echo -e "${GREEN}Step 3: Removing object files and libraries...${NC}"
# Find and remove .o, .a, .so files (but not in vcpkg)
if find . -path ./vcpkg -prune -o \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.so.*" \) -type f -print | grep -q .; then
    find . -path ./vcpkg -prune -o \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.so.*" \) -type f -print -delete
    echo "  Removed object files and libraries"
else
    echo "  No object files or libraries found"
fi

echo ""
echo -e "${GREEN}Step 4: Removing editor/IDE files...${NC}"
remove_if_exists ".vscode/ipch" "VS Code IntelliSense cache"
remove_if_exists ".cache" "clangd/other caches"

echo ""
echo -e "${GREEN}Step 5: Removing log files...${NC}"
remove_if_exists "cufile.log" "cufile log"
# Remove any .log files in the project root
if ls *.log 1> /dev/null 2>&1; then
    echo -e "${YELLOW}Removing log files:${NC}"
    rm -f *.log
fi

echo ""
echo -e "${GREEN}Step 6: Removing temporary files...${NC}"
# Remove common temporary files
find . -name "*~" -type f -delete 2>/dev/null || true
find . -name "*.swp" -type f -delete 2>/dev/null || true
find . -name "*.swo" -type f -delete 2>/dev/null || true
find . -name ".DS_Store" -type f -delete 2>/dev/null || true
echo "  Removed temporary and backup files"

# Optional: Clean vcpkg
if [ "$CLEAN_VCPKG" = true ]; then
    echo ""
    echo -e "${GREEN}Step 7: Cleaning vcpkg build cache...${NC}"
    VCPKG_ROOT="${VCPKG_ROOT:-/tmp/vcpkg}"
    if [ -d "$VCPKG_ROOT" ]; then
        echo -e "${YELLOW}Cleaning vcpkg buildtrees and packages...${NC}"
        remove_if_exists "$VCPKG_ROOT/buildtrees" "vcpkg buildtrees"
        remove_if_exists "$VCPKG_ROOT/packages" "vcpkg packages"
        remove_if_exists "$VCPKG_ROOT/downloads" "vcpkg downloads"
        echo -e "${YELLOW}Note: vcpkg will need to rebuild packages on next build${NC}"
    else
        echo "  vcpkg not found at $VCPKG_ROOT"
    fi
fi

# Summary
echo ""
echo -e "${GREEN}=== Clean Complete ===${NC}"
echo ""
echo "Cleaned:"
echo "  ✓ Build directories (build/, cmake-build-debug/, etc.)"
echo "  ✓ CMake cache files"
echo "  ✓ Object files and libraries"
echo "  ✓ Log files"
echo "  ✓ Temporary files"
if [ "$CLEAN_VCPKG" = true ]; then
    echo "  ✓ vcpkg build cache"
fi
echo ""
echo "To rebuild the project, run:"
echo "  ./scripts/build-ubuntu.sh"
echo ""
echo "Usage:"
echo "  ./scripts/clean.sh       # Clean build artifacts only"
echo "  ./scripts/clean.sh --all # Also clean vcpkg cache (slower rebuild)"
echo ""
