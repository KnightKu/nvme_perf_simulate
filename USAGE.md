# Usage (l0-base)

## Quick start

```bash
make
./nvme_perf_model nand.conf
```

## Benchmark suite

```bash
make test-nand
```

Environment overrides:

```bash
ELEMENT=8192 QD=512 ./scripts/run_nand_tests.sh
```

## Single-case config examples

**Random 4K read**

```ini
read_ratio=100
write_ratio=0
block_size=0
io_pattern=random
```

**Sequential 4K write**

```ini
read_ratio=0
write_ratio=100
block_size=0
io_pattern=sequential
```

**Mixed 70/30 random 4K**

```ini
read_ratio=70
write_ratio=30
block_size=0
io_pattern=random
```

**128K sequential read** (8 pages × 16 KiB on one die, multi-plane fan-out)

```ini
read_ratio=100
write_ratio=0
block_size=131072
io_pattern=sequential
```

### Address placement (`io_pattern`)

| Mode | Behavior |
|------|----------|
| `random` | Each host command targets a uniform random global die |
| `sequential` | Round-robin global die index (wraps at device size) |

Multi-page blocks (`block_size > page_size`) keep all pages on **one die** and
fan out via multi-plane slots (`max_planes_per_die = iwl_slot / die_num`,
default **4**). Legacy `stripe_mode` in `.conf` is ignored.

**128K write bandwidth note:** host write BW is capped by global tprog slots
(`iwl_slot` / tprog ≈ 6.5 GB/s at default config). Multi-plane fan-out on a
single die does not raise the aggregate program-page ceiling.

## Output metrics

- **IOPS / Bandwidth**: host-visible completion rate (xor-adjusted for RAID-like factor)
- **NAND program pages**: actual program operations completed (true NAND write count)
- **Utilization**: sim bandwidth vs channel wire/host/xor ceilings

Writes are **tprog-limited** on NAND; reads are usually channel-bandwidth limited.
