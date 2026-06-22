#!/usr/bin/env bash
# Run NAND-backend seq / random / mixed IO benchmarks.
#
# Usage:
#   ./scripts/run_nand_tests.sh
#   ELEMENT=8192 QD=512 ./scripts/run_nand_tests.sh
#
# Outputs per-case logs under tests/out_nand/ and summary files via summarize_nand.py.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/nvme_perf_model}"
BASELINE="${BASELINE:-$ROOT/tests/baseline_nand.conf}"
OUT_DIR="${OUT_DIR:-$ROOT/tests/out_nand}"
ELEMENT="${ELEMENT:-16384}"
QD="${QD:-1024}"
SUMMARIZE="${SUMMARIZE:-$ROOT/scripts/summarize_nand.py}"

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
    echo "error: python3 required for summary (scripts/summarize_nand.py)" >&2
    return 1
}

FAIL=0

run_case seq_read_4k \
    read_ratio=100 write_ratio=0 block_size=0 \
    io_pattern=sequential stripe_mode=global_die \
    || FAIL=1

run_case seq_write_4k \
    read_ratio=0 write_ratio=100 block_size=0 \
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
    block_size=131072 read_ratio=100 write_ratio=0 \
    io_pattern=sequential stripe_mode=page_stripe \
    || FAIL=1

run_case seq_write_128k \
    block_size=131072 read_ratio=0 write_ratio=100 \
    io_pattern=sequential stripe_mode=page_stripe \
    || FAIL=1

run_case rand_read_128k \
    read_ratio=100 write_ratio=0 block_size=131072 io_pattern=random \
    stripe_mode=page_stripe \
    || FAIL=1

run_case rand_write_128k \
    read_ratio=0 write_ratio=100 block_size=131072 io_pattern=random \
    stripe_mode=page_stripe \
    || FAIL=1

run_case mixed_rw_70_30_128k \
    read_ratio=70 write_ratio=30 block_size=131072 io_pattern=random \
    stripe_mode=page_stripe \
    || FAIL=1

echo "========================================"
if [[ "$FAIL" -ne 0 ]]; then
    echo "Some simulator runs failed." >&2
fi

if ! summarize_results; then
    FAIL=1
fi

if [[ "$FAIL" -eq 0 ]]; then
    echo "All NAND IO tests passed."
    echo "Summary: $OUT_DIR/summary.csv"
    exit 0
fi
echo "Some tests failed. See logs in $OUT_DIR" >&2
exit 1
