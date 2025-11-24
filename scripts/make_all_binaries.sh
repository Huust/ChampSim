#!/bin/bash
set -e
trap 'echo "Error occurred on line $LINENO"; exit 1' ERR

# Build all binary configurations for our ChampSim project
echo "Building ChampSim binaries for different hardware configurations..."

# Clean bin directory before building
echo "Cleaning bin directory..."
rm -f bin/champsim_*

# Tiered memory configuration
echo "Building tiered_memory configuration..."
./config.sh hardware_configs/tiered_memory.json && make -j && make clean

# DRAM-only configuration
echo "Building dram_only configuration..."
./config.sh hardware_configs/dram_only.json && make -j && make clean

# CXL-only configuration
echo "Building cxl_only configuration..."
./config.sh hardware_configs/cxl_only.json && make -j && make clean

# Tiered memory with SKIP_TRANSLATION
# Modify output file name (add suffix '_skip'), after compilation, recover its name
echo "Building tiered_memory with skip configuration..."
sed -i '' 's/"champsim_tiered_memory"/"champsim_tiered_memory_skip"/' hardware_configs/tiered_memory.json
./config.sh hardware_configs/tiered_memory.json && make -j SKIP_TRANSLATION=1 && make clean
sed -i '' 's/"champsim_tiered_memory_skip"/"champsim_tiered_memory"/' hardware_configs/tiered_memory.json

echo "All binaries built successfully!"
echo "Available binaries:"
echo "  bin/champsim_tiered_memory       - Tiered memory system"
echo "  bin/champsim_tiered_memory_skip  - Tiered memory with skip translation"
echo "  bin/champsim_dram_only           - Local DRAM memory system"
echo "  bin/champsim_cxl_only            - CXL memory system"
