#!/bin/bash
set -e
trap 'echo "Error occurred on line $LINENO"; exit 1' ERR

# Build all binary configurations for our ChampSim project
echo "Building ChampSim binaries for different hardware configurations..."

# Hybrid configuration (both CXL and DRAM enabled: 1,1)
echo "Building hybrid configuration..."
./config.sh hardware_configs/hybrid.json && make -j && make clean

# DRAM-only configuration (CXL disabled, DRAM enabled: 0,1)
echo "Building dram_only configuration..."
./config.sh hardware_configs/dram_only.json && make -j && make clean

# CXL-only configuration (CXL enabled, DRAM disabled: 1,0)
echo "Building cxl_only configuration..."
./config.sh hardware_configs/cxl_only.json && make -j && make clean

echo "All binaries built successfully!"
echo "Available binaries:"
echo "  bin/champsim_hybrid       - Hybrid memory system (CXL+DRAM)"
echo "  bin/champsim_dram_only    - DRAM-only memory system"
echo "  bin/champsim_cxl_only     - CXL-only memory system"
