#!/usr/bin/env bash
# Run NAND-backend seq / random / mixed IO benchmarks.
#
# Usage:
#   ./scripts/run_nand_tests.sh
#   ELEMENT=8192 QD=512 ./scripts/run_nand_tests.sh
#
# Case configs live in tests/nand/*.conf (one file per benchmark case).
# Outputs per-case logs under tests/out_nand/ and summary files via summarize_nand.py.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/nvme_perf_model}"
CASES_DIR="${CASES_DIR:-$ROOT/tests/nand}"
OUT_DIR="${OUT_DIR:-$ROOT/tests/out_nand}"
ELEMENT="${ELEMENT:-16384}"
QD="${QD:-1024}"
SUMMARIZE="${SUMMARIZE:-$ROOT/scripts/summarize_nand.py}"

CASES=(
    seq_read_4k
    seq_write_4k
    rand_read_4k
    rand_write_4k
    mixed_rw_70_30
    mixed_rw_50_50
    seq_read_128k
    seq_write_128k
    rand_read_128k
    rand_write_128k
    mixed_rw_70_30_128k
)

mkdir -p "$OUT_DIR"

if [[ ! -x "$BIN" ]]; then
    if [[ -f "$ROOT/nvme_perf_model.exe" ]]; then
        BIN="$ROOT/nvme_perf_model.exe"
    else
        echo "error: $BIN not found. Run 'make' in $ROOT first." >&2
        exit 1
    fi
fi

prepare_conf() {
    local name="$1"
    local src="$CASES_DIR/${name}.conf"
    local out="$OUT_DIR/${name}.conf"

    if [[ ! -f "$src" ]]; then
        echo "error: case config not found: $src" >&2
        return 1
    fi
    cp "$src" "$out"
    sed -i.bak "s/^element=.*/element=${ELEMENT}/" "$out"
    sed -i.bak "s/^qd=.*/qd=${QD}/" "$out"
    rm -f "${out}.bak"
}

run_case() {
    local name="$1"
    local conf="$OUT_DIR/${name}.conf"
    local log="$OUT_DIR/${name}.log"

    prepare_conf "$name"
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

for name in "${CASES[@]}"; do
    run_case "$name" || FAIL=1
done

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
