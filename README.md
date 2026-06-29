# nvme_perf_simulate (nand-core / l0-base)

Minimal multi-channel NAND backend performance simulator.

On branch **`l0-base`**, sources are split into reusable modules (see
[`L0_BASE.md`](L0_BASE.md)). Stripe modes are removed; placement uses
`io_pattern` (random / sequential global die).

## Scope

This branch models **NAND flash channels only** — no PCIe/NVMe stack, no write cache,
no bus transfer layer, no codeword path. Commands flow:

```
Host inject → per-die queues → channel FSM (CMD/DATA) → tR / tprog / tERASE
```

## Build

```bash
make
./nvme_perf_model              # uses nand.conf
./nvme_perf_model my.conf
```

## NAND IO benchmarks

Run sequential / random / mixed read-write cases (4K and 128K):

```bash
make nand-core
# or
make test-nand
./scripts/run_nand_tests.sh
python3 scripts/summarize_nand.py tests/out_nand
```

Per-case configs are in `tests/nand/*.conf` (11 files). Run a single case:

```bash
./nvme_perf_model tests/nand/seq_read_4k.conf
```

Outputs land in `tests/out_nand/` (`*.log`, `summary.csv`, `summary.txt`, `summary.json`).

## Key config keys

| Key | Meaning |
|-----|---------|
| `chan_num`, `die_num`, `plane` | Topology |
| `chan_speed` | MB/s per channel (wire rate) |
| `cmd_size`, `block_size` | 4K legacy vs multi-page (128K = 8×16K pages) |
| `read_ratio`, `write_ratio`, `erase_ratio` | Workload mix |
| `io_pattern` | `random` or `sequential` (global die placement) |
| `element`, `qd` | Commands to complete and queue depth |
| `tR`, `tr_fast`, `tprog_eff`, `tERASE` | NAND timings (µs) |

See `tests/baseline_nand.conf` and `nand.conf` for defaults.

## Legacy branch

Full-stack features (write cache, bus, codeword buffers) remain on `active-dev`.
Use `make test-legacy` there.
