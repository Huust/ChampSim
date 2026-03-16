#!/bin/bash
# Phase 1: Multi-page-size (4KB + 2MB) verification tests
# Short runs: -w 1000000 -i 5000000
set -uo pipefail

BINARY="bin/champsim"
WARMUP=1000000
SIM=5000000
TRACE_DIR="/proj/uart_chp_cxl_trans/champsim_traces"

# Use a single trace for quick validation
BENCH="429.mcf"
TRACE="429.mcf-184B.champsimtrace.xz"
RDIR="test_results/$BENCH"

extract_stats() {
    local outfile="$1"
    local label="$2"

    local ipc=$(grep "cumulative IPC" "$outfile" | tail -1 | awk '{print $4}')
    local dtlb=$(grep "cpu0->cpu0_DTLB TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')
    local dtlb_2m=$(grep "cpu0->cpu0_DTLB_2M TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')
    local itlb=$(grep "cpu0->cpu0_ITLB TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')
    local itlb_2m=$(grep "cpu0->cpu0_ITLB_2M TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')
    local stlb=$(grep "cpu0->cpu0_STLB TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')
    local stlb_2m=$(grep "cpu0->cpu0_STLB_2M TOTAL" "$outfile" | tail -1 | awk '{for(i=1;i<=NF;i++) if($i=="ACCESS:") print $(i+1)}')

    printf "  %-15s IPC=%-8s DTLB=%-10s DTLB_2M=%-10s ITLB=%-10s ITLB_2M=%-10s STLB=%-10s STLB_2M=%-10s\n" \
           "$label" "${ipc:-N/A}" "${dtlb:-0}" "${dtlb_2m:-0}" "${itlb:-0}" "${itlb_2m:-0}" "${stlb:-0}" "${stlb_2m:-0}"
}

echo "Phase 1: Multi-page-size verification ($BENCH)"
echo "================================================"

PASS=0
FAIL=0

# Test A: No pmap baseline
echo ""
echo "[A] No pmap baseline..."
if timeout 300 $BINARY -w $WARMUP -i $SIM "$TRACE_DIR/$TRACE" > "$RDIR/no_pmap.out" 2>&1; then
    echo "  PASS"
    PASS=$((PASS+1))
else
    echo "  FAIL (rc=$?)"
    FAIL=$((FAIL+1))
fi

# Test B: All 4KB pmap
echo "[B] All 4KB pmap..."
if timeout 300 $BINARY -w $WARMUP -i $SIM --pmap "$RDIR/all_4k.pmap" "$TRACE_DIR/$TRACE" > "$RDIR/all_4k.out" 2>&1; then
    echo "  PASS"
    PASS=$((PASS+1))
else
    echo "  FAIL (rc=$?)"
    FAIL=$((FAIL+1))
fi

# Test C: All 2MB pmap
echo "[C] All 2MB pmap..."
if timeout 300 $BINARY -w $WARMUP -i $SIM --pmap "$RDIR/all_2m.pmap" "$TRACE_DIR/$TRACE" > "$RDIR/all_2m.out" 2>&1; then
    echo "  PASS"
    PASS=$((PASS+1))
else
    echo "  FAIL (rc=$?)"
    FAIL=$((FAIL+1))
fi

# Test D: Mixed pmap
echo "[D] Mixed 4KB+2MB pmap..."
if timeout 300 $BINARY -w $WARMUP -i $SIM --pmap "$RDIR/mixed.pmap" "$TRACE_DIR/$TRACE" > "$RDIR/mixed.out" 2>&1; then
    echo "  PASS"
    PASS=$((PASS+1))
else
    echo "  FAIL (rc=$?)"
    FAIL=$((FAIL+1))
fi

echo ""
echo "--- Stats Summary ---"
for cfg in no_pmap all_4k all_2m mixed; do
    [ -f "$RDIR/${cfg}.out" ] && extract_stats "$RDIR/${cfg}.out" "$cfg"
done

echo ""
echo "Phase 1 result: $PASS/4 passed, $FAIL failed"
