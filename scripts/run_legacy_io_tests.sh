#!/usr/bin/env bash
# Run legacy random / mixed read-write benchmark cases.
#
# Usage:
#   ./scripts/run_legacy_io_tests.sh
#   ELEMENT=8192 QD=512 ./scripts/run_legacy_io_tests.sh
#
# Requires: nvme_perf_model (build with `make` first).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/nvme_perf_model}"
BASELINE="${BASELINE:-$ROOT/tests/baseline.conf}"
OUT_DIR="${OUT_DIR:-$ROOT/tests/out}"
ELEMENT="${ELEMENT:-16384}"
QD="${QD:-1024}"

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

FAIL=0

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
if [[ "$FAIL" -eq 0 ]]; then
    echo "All legacy IO tests passed."
    echo "Logs: $OUT_DIR/*.log"
    exit 0
fi
echo "Some tests failed. See logs in $OUT_DIR" >&2
exit 1
