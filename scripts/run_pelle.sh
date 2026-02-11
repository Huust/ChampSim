#!/bin/bash
#SBATCH --account=uppmax2025-2-337 # Project account
#SBATCH --ntasks=1                    # Number of cores
#SBATCH --time=24:00:00               # Time limit (hh:mm:ss)

# ChampSim SLURM Execution Script
#
# This script handles 11 configurations with proper file organization:
#   - Prerequisite configs: dram_only, interleaving (generate shared files)
#   - Heatmap-based configs: access/criticality × ratio (1:1, 1:3) × skip/no-skip
#   - Interleaving-based: interleaving_skip
#
# File organization:
#   results/
#   ├── heatmaps/
#   │   └── <trace_name>.heatmap          # Shared by all heatmap configs
#   ├── interleavings/
#   │   └── <trace_name>.interleaving     # Used by interleaving_skip
#   └── <config>/
#       ├── stdout/
#       │   └── <trace_name>.out
#       └── stderr/
#           └── <trace_name>.err
#
# Usage: run_pelle.sh <configuration> <results_dir> <full_trace_path>
# This script isn't run individually

# Check if we have the right number of arguments
if [[ $# -ne 3 ]]; then
    echo "Incorrect # of arguments passed to this script! See below:"
    echo "Usage: $0 <configuration> <results_dir> <full_trace_path>"
    echo "Configurations:"
    echo "  dram_only               : DRAM system + generate heatmap (prerequisite)"
    echo "  interleaving            : Tiered interleaving + generate interleaving map (prerequisite)"
    echo "  access_r1_1             : Access-based heatmap, ratio 1:1"
    echo "  access_r1_3             : Access-based heatmap, ratio 1:3"
    echo "  criticality_r1_1        : Criticality-based heatmap, ratio 1:1"
    echo "  criticality_r1_3        : Criticality-based heatmap, ratio 1:3"
    echo "  access_r1_1_skip        : Access-based heatmap with skip, ratio 1:1"
    echo "  access_r1_3_skip        : Access-based heatmap with skip, ratio 1:3"
    echo "  criticality_r1_1_skip   : Criticality-based heatmap with skip, ratio 1:1"
    echo "  criticality_r1_3_skip   : Criticality-based heatmap with skip, ratio 1:3"
    echo "  interleaving_skip       : Interleaving allocation with skip"
    exit 1
fi

# Extract arguments
CONFIGURATION="$1"
RESULTS_DIR="$2"
FULL_TRACE_PATH="$3"

# Base parameters
WARMUP_INSTRUCTIONS=50000000
SIMULATION_INSTRUCTIONS=100000000
ARGS="-w $WARMUP_INSTRUCTIONS -i $SIMULATION_INSTRUCTIONS"

# Project base directory
CHAMPSIM_BASE="/proj/uart_chp_cxl_trans/songtao/ChampSim-dev"

# Binary paths
DRAM_BINARY="$CHAMPSIM_BASE/bin/champsim_dram_only"
TIERED_BINARY="$CHAMPSIM_BASE/bin/champsim_tiered_memory"
TIERED_SKIP_BINARY="$CHAMPSIM_BASE/bin/champsim_tiered_memory_skip"
CXL_BINARY="$CHAMPSIM_BASE/bin/champsim_cxl_only"

# Heatmap and interleaving directories
HEATMAP_DIR="$RESULTS_DIR/heatmaps"
INTERLEAVING_DIR="$RESULTS_DIR/interleavings"

# Extract trace name
TRACE_NAME=$(basename "$FULL_TRACE_PATH" .xz | sed 's/\.champsimtrace$//')

# Shared file paths (used by multiple configurations)
HEATMAP_FILE="$HEATMAP_DIR/${TRACE_NAME}.heatmap"
INTERLEAVING_FILE="$INTERLEAVING_DIR/${TRACE_NAME}.interleaving"

# Configure based on the configuration argument
case "$CONFIGURATION" in
    "dram_only")
        # Prerequisite: Generate heatmap for all heatmap-based configs
        echo "=== Running DRAM-only + generating heatmap ==="
        COMMAND="$DRAM_BINARY $ARGS --generate-heatmap $HEATMAP_FILE $FULL_TRACE_PATH"
        ;;

    "interleaving")
        # Prerequisite: Generate interleaving map for interleaving_skip
        echo "=== Running interleaving + generating interleaving map ==="
        COMMAND="$TIERED_BINARY $ARGS --generate-interleaving $INTERLEAVING_FILE $FULL_TRACE_PATH"
        ;;

    # Non-skip heatmap-based configurations
    "access_r1_1")
        echo "=== Running access-based (1:1) with heatmap ==="
        COMMAND="$TIERED_BINARY $ARGS --use-heatmap $HEATMAP_FILE --ratio 1:1 $FULL_TRACE_PATH"
        ;;

    "access_r1_3")
        echo "=== Running access-based (1:3) with heatmap ==="
        COMMAND="$TIERED_BINARY $ARGS --use-heatmap $HEATMAP_FILE --ratio 1:3 $FULL_TRACE_PATH"
        ;;

    "criticality_r1_1")
        echo "=== Running criticality-based (1:1) with heatmap ==="
        COMMAND="$TIERED_BINARY $ARGS --use-heatmap $HEATMAP_FILE --sort-by-criticality --ratio 1:1 $FULL_TRACE_PATH"
        ;;

    "criticality_r1_3")
        echo "=== Running criticality-based (1:3) with heatmap ==="
        COMMAND="$TIERED_BINARY $ARGS --use-heatmap $HEATMAP_FILE --sort-by-criticality --ratio 1:3 $FULL_TRACE_PATH"
        ;;

    # Skip-translation heatmap-based configurations
    "access_r1_1_skip")
        echo "=== Running access-based (1:1) with heatmap + skip translation ==="
        COMMAND="$TIERED_SKIP_BINARY $ARGS --use-heatmap $HEATMAP_FILE --ratio 1:1 $FULL_TRACE_PATH"
        ;;

    "access_r1_3_skip")
        echo "=== Running access-based (1:3) with heatmap + skip translation ==="
        COMMAND="$TIERED_SKIP_BINARY $ARGS --use-heatmap $HEATMAP_FILE --ratio 1:3 $FULL_TRACE_PATH"
        ;;

    "criticality_r1_1_skip")
        echo "=== Running criticality-based (1:1) with heatmap + skip translation ==="
        COMMAND="$TIERED_SKIP_BINARY $ARGS --use-heatmap $HEATMAP_FILE --sort-by-criticality --ratio 1:1 $FULL_TRACE_PATH"
        ;;

    "criticality_r1_3_skip")
        echo "=== Running criticality-based (1:3) with heatmap + skip translation ==="
        COMMAND="$TIERED_SKIP_BINARY $ARGS --use-heatmap $HEATMAP_FILE --sort-by-criticality --ratio 1:3 $FULL_TRACE_PATH"
        ;;

    # Interleaving-based skip configuration
    "interleaving_skip")
        echo "=== Running interleaving + skip translation ==="
        COMMAND="$TIERED_SKIP_BINARY $ARGS --use-interleaving $INTERLEAVING_FILE $FULL_TRACE_PATH"
        ;;

    *)
        echo "Invalid configuration: $CONFIGURATION"
        echo "Valid configurations:"
        echo "  dram_only, interleaving,"
        echo "  access_r1_1, access_r1_3, criticality_r1_1, criticality_r1_3,"
        echo "  access_r1_1_skip, access_r1_3_skip, criticality_r1_1_skip, criticality_r1_3_skip,"
        echo "  interleaving_skip"
        exit 1
        ;;
esac

# Execute command
echo "Executing: $COMMAND"
$COMMAND 2>&1
