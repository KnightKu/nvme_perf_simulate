#!/usr/bin/env bash
# Run legacy / sequential / mixed IO benchmark cases.
#
# Usage:
#   ./scripts/run_legacy_io_tests.sh
#   ELEMENT=8192 QD=512 ./scripts/run_legacy_io_tests.sh
#
# Outputs per-case logs under tests/out/ and a final summary:
#   tests/out/summary.csv
#   tests/out/summary.txt
#   tests/out/summary.json
#
# Requires: nvme_perf_model (build with `make` first).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/nvme_perf_model}"
BASELINE="${BASELINE:-$ROOT/tests/baseline.conf}"
OUT_DIR="${OUT_DIR:-$ROOT/tests/out}"
ELEMENT="${ELEMENT:-16384}"
QD="${QD:-1024}"
SUMMARIZE="${SUMMARIZE:-$ROOT/scripts/summarize_legacy_io.py}"

mkdir -p "$OUT_DIR"

if [[ ! -x "$BIN" ]]; then
    if [[ -f "$ROOT/nvme_perf_model.exe" ]]; then
        BIN="$ROOT/nvme_perf_model.exe"
    else
        echo "error: $BIN not found. Run 'make' in $ROOT first." >&2
        exit 1
    fi
fi

if [[ ! -f "$BASELINE" ]]; then
    echo "error: baseline config not found: $BASELINE" >&2
    exit 1
fi

write_conf() {
    local out="$1"
    shift
    cp "$BASELINE" "$out"
    sed -i.bak "s/^element=.*/element=${ELEMENT}/" "$out"
    sed -i.bak "s/^qd=.*/qd=${QD}/" "$out"
    rm -f "${out}.bak"
    local kv key val
    for kv in "$@"; do
        key="${kv%%=*}"
        val="${kv#*=}"
        if grep -q "^${key}=" "$out"; then
            sed -i.bak "s/^${key}=.*/${key}=${val}/" "$out"
        else
            echo "${key}=${val}" >> "$out"
        fi
        rm -f "${out}.bak"
    done
}

run_case() {
    local name="$1"
    shift
    local conf="$OUT_DIR/${name}.conf"
    local log="$OUT_DIR/${name}.log"

    write_conf "$conf" "$@"
    echo "==> $name"
    echo "    config: $conf"
    if ! "$BIN" "$conf" | tee "$log"; then
        echo "FAIL: $name (simulator exit != 0)" >&2
        return 1
    fi
    echo
}

summarize_results_native() {
    local csv="$OUT_DIR/summary.csv"
    echo "case,category,block,read_ratio,write_ratio,status,read_iops,write_iops,read_bw_mbps,write_bw_mbps,total_bw_mbps" > "$csv"
    local names=(
        seq_read_4k:sequential_read:4K:100:0
        seq_write_4k:sequential_write:4K:0:100
        rand_read_4k:random_read:4K:100:0
        rand_write_4k:random_write:4K:0:100
        mixed_rw_70_30:mixed:4K:70:30
        mixed_rw_50_50:mixed:4K:50:50
        seq_read_128k:sequential_read:128K:100:0
        seq_write_128k:sequential_write:128K:0:100
        rand_read_128k:random_read:128K:100:0
        rand_write_128k:random_write:128K:0:100
        mixed_rw_70_30_128k:mixed:128K:70:30
    )
    local entry name category block rr wr log status
    local read_iops write_iops read_bw write_bw total_bw
    echo ""
    echo "======== Legacy IO Benchmark Summary ========"
    printf "%-22s %-13s %-6s %-9s %11s %11s %10s %10s %10s %-6s\n" \
        "Case" "Type" "Block" "R/W" "Read IOPS" "Write IOPS" "Read MB/s" "Write MB/s" "Total MB/s" "Status"
    printf "%.0s-" {1..110}; echo
    for entry in "${names[@]}"; do
        IFS=: read -r name category block rr wr <<< "$entry"
        log="$OUT_DIR/${name}.log"
        status="MISSING"
        read_iops="-"; write_iops="-"; read_bw="-"; write_bw="-"; total_bw="-"
        if [[ -f "$log" ]]; then
            read_iops="$(sed -n 's/Read IOPS = \([0-9.]*\).*/\1/p' "$log" | head -n1)"
            write_iops="$(sed -n 's/Write IOPS = \([0-9.]*\).*/\1/p' "$log" | head -n1)"
            read_bw="$(sed -n 's/Read Bandwidth (sim) = \([0-9.]*\) MB\/s.*/\1/p' "$log" | head -n1)"
            write_bw="$(sed -n 's/Write Bandwidth (sim) = \([0-9.]*\) MB\/s.*/\1/p' "$log" | head -n1)"
            total_bw="$(sed -n 's/Total Bandwidth (sim) = \([0-9.]*\) MB\/s.*/\1/p' "$log" | head -n1)"
            [[ -z "$read_iops" ]] && read_iops="-"
            [[ -z "$write_iops" ]] && write_iops="-"
            [[ -z "$read_bw" ]] && read_bw="-"
            [[ -z "$write_bw" ]] && write_bw="-"
            [[ -z "$total_bw" ]] && total_bw="-"
            if grep -q "Performance = " "$log"; then status="PASS"; else status="FAIL"; fi
        fi
        printf "%-22s %-13s %-6s %-9s %11s %11s %10s %10s %10s %-6s\n" \
            "$name" "$category" "$block" "${rr}/${wr}" "$read_iops" "$write_iops" "$read_bw" "$write_bw" "$total_bw" "$status"
        echo "${name},${category},${block},${rr},${wr},${status},${read_iops},${write_iops},${read_bw},${write_bw},${total_bw}" >> "$csv"
    done
    echo ""
    echo "Wrote $csv"
}

summarize_results() {
    if [[ -f "$SUMMARIZE" ]]; then
        if command -v python3 >/dev/null 2>&1; then
            python3 "$SUMMARIZE" "$OUT_DIR"
            return $?
        fi
        if command -v python >/dev/null 2>&1; then
            python "$SUMMARIZE" "$OUT_DIR"
            return $?
        fi
    fi
    echo "warning: python not found; using built-in summary" >&2
    summarize_results_native
    local entry name log
    local names=(
        seq_read_4k seq_write_4k
        rand_read_4k rand_write_4k mixed_rw_70_30 mixed_rw_50_50
        seq_read_128k seq_write_128k
        rand_read_128k rand_write_128k mixed_rw_70_30_128k
    )
    for name in "${names[@]}"; do
        log="$OUT_DIR/${name}.log"
        if [[ ! -f "$log" ]] || ! grep -q "Performance = " "$log"; then
            return 1
        fi
    done
    return 0
}

FAIL=0

run_case seq_read_4k \
    workload=legacy read_ratio=100 write_ratio=0 block_size=0 \
    io_pattern=sequential stripe_mode=global_die \
    || FAIL=1

run_case seq_write_4k \
    workload=legacy read_ratio=0 write_ratio=100 block_size=0 \
    io_pattern=sequential stripe_mode=global_die \
    || FAIL=1

run_case rand_read_4k \
    read_ratio=100 write_ratio=0 block_size=0 io_pattern=random \
    || FAIL=1

run_case rand_write_4k \
    read_ratio=0 write_ratio=100 block_size=0 io_pattern=random \
    || FAIL=1

run_case mixed_rw_70_30 \
    read_ratio=70 write_ratio=30 block_size=0 io_pattern=random \
    || FAIL=1

run_case mixed_rw_50_50 \
    read_ratio=50 write_ratio=50 block_size=0 io_pattern=random \
    || FAIL=1

run_case seq_read_128k \
    workload=fulldev_seq_read block_size=131072 read_ratio=100 write_ratio=0 \
    || FAIL=1

run_case seq_write_128k \
    workload=fulldev_seq_write block_size=131072 read_ratio=0 write_ratio=100 \
    || FAIL=1

run_case rand_read_128k \
    read_ratio=100 write_ratio=0 block_size=131072 io_pattern=random \
    stripe_mode=channel_major \
    || FAIL=1

run_case rand_write_128k \
    read_ratio=0 write_ratio=100 block_size=131072 io_pattern=random \
    stripe_mode=channel_major \
    || FAIL=1

run_case mixed_rw_70_30_128k \
    read_ratio=70 write_ratio=30 block_size=131072 io_pattern=random \
    stripe_mode=channel_major \
    || FAIL=1

echo "========================================"
if [[ "$FAIL" -ne 0 ]]; then
    echo "Some simulator runs failed. Generating partial summary..." >&2
fi

if ! summarize_results; then
    FAIL=1
fi

if [[ "$FAIL" -eq 0 ]]; then
    echo "All legacy IO tests passed."
    echo "Summary: $OUT_DIR/summary.csv"
    exit 0
fi
echo "Some tests failed. See logs in $OUT_DIR" >&2
exit 1
