#!/bin/bash
#SBATCH --account=uppmax2025-2-135 # Project account
#SBATCH --ntasks=1                    # Number of cores
#SBATCH --time=12:00:00               # Time limit (hh:mm:ss)

# Check if we have the right number of arguments
if [[ $# -ne 4 ]]; then
    echo "Usage: $0 <configuration> <output_dir> <output_prefix> <trace_file>"
    echo "Configurations:"
    echo "  cxl_only                   : CXL-only memory system"
    echo "  dram_only                  : DRAM-only memory system"
    echo "  hybrid-roundrobin          : Hybrid with round-robin allocation"
    echo "  hybrid-hotness-access      : Hybrid with access-count-based heatmap"
    echo "  hybrid-hotness-criticality : Hybrid with criticality-based heatmap"
    exit 1
fi

# Extract arguments
CONFIGURATION="$1"
OUTPUT_DIR="$2"
OUTPUT_PREFIX="$3"
TRACE_FILE="$4"

# Base parameters
WARMUP_INSTRUCTIONS=50000000
SIMULATION_INSTRUCTIONS=100000000
ARGS="-w $WARMUP_INSTRUCTIONS -i $SIMULATION_INSTRUCTIONS"

# Project base directory (adjust this path for your cluster environment)
CHAMPSIM_BASE="/crex/proj/uppstore2017059/iamchp/ChampSim-dev"

# Heatmap file path with trace name for identification
HEATMAP_DIR="$OUTPUT_DIR/heatmaps"
TRACE_NAME=$(basename "$TRACE_FILE" .xz)

# Configure based on the configuration argument
case "$CONFIGURATION" in
    "cxl_only")
        BINARY="$CHAMPSIM_BASE/bin/champsim_cxl_only"
        COMMAND="$BINARY $ARGS $TRACE_FILE"
        ;;
    "dram_only")
        BINARY="$CHAMPSIM_BASE/bin/champsim_dram_only"
        COMMAND="$BINARY $ARGS $TRACE_FILE"
        ;;
    "hybrid-roundrobin")
        BINARY="$CHAMPSIM_BASE/bin/champsim_hybrid"
        COMMAND="$BINARY $ARGS $TRACE_FILE"
        ;;
    "hybrid-hotness-access")
        # Two-phase execution for access-count-based heatmap allocation
        BINARY="$CHAMPSIM_BASE/bin/champsim_hybrid"
        HEATMAP_FILE="$HEATMAP_DIR/${TRACE_NAME}_access"

        echo "=== PHASE 1: Generating heatmap ==="
        GENERATE_COMMAND="$BINARY $ARGS --generate-heatmap $HEATMAP_FILE $TRACE_FILE"
        echo "Executing: $GENERATE_COMMAND"
        $GENERATE_COMMAND

        if [ $? -ne 0 ]; then
            echo "ERROR: Heatmap generation failed!"
            exit 1
        fi

        echo "=== PHASE 2: Using heatmap for simulation (access-count based) ==="
        COMMAND="$BINARY $ARGS --use-heatmap $HEATMAP_FILE $TRACE_FILE"
        ;;
    "hybrid-hotness-criticality")
        # Two-phase execution for criticality-based heatmap allocation
        BINARY="$CHAMPSIM_BASE/bin/champsim_hybrid"
        HEATMAP_FILE="$HEATMAP_DIR/${TRACE_NAME}_criticality"

        echo "=== PHASE 1: Generating heatmap ==="
        GENERATE_COMMAND="$BINARY $ARGS --generate-heatmap $HEATMAP_FILE $TRACE_FILE"
        echo "Executing: $GENERATE_COMMAND"
        $GENERATE_COMMAND

        if [ $? -ne 0 ]; then
            echo "ERROR: Heatmap generation failed!"
            exit 1
        fi

        echo "=== PHASE 2: Using heatmap for simulation (criticality based) ==="
        COMMAND="$BINARY $ARGS --use-heatmap $HEATMAP_FILE --sort-by-criticality $TRACE_FILE"
        ;;
    *)
        echo "Invalid configuration: $CONFIGURATION"
        echo "Valid configurations: cxl_only, dram_only, hybrid-roundrobin, hybrid-hotness-access, hybrid-hotness-criticality"
        exit 1
        ;;
esac

# Run the final command
echo "Executing: $COMMAND"
$COMMAND 2>&1
