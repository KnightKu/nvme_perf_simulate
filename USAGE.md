# Usage (nand-core)

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
stripe_mode=global_die
```

**Mixed 70/30 random 4K**

```ini
read_ratio=70
write_ratio=30
block_size=0
io_pattern=random
```

**128K sequential read** (8 pages × 16 KiB)

```ini
read_ratio=100
write_ratio=0
block_size=131072
io_pattern=sequential
stripe_mode=global_die
```

Multi-page blocks fan out across planes on the target die. Concurrent plane
count follows DESIGN.md §7: `max_planes_per_die = iwl_slot / die_num` (must be
a power of two). With the default 128 die / 512 iwl_slot config that is **4
planes** in parallel per die.

- **Read**: one tR per block, then up to `max_planes_per_die` page DATA
  transfers in parallel (channel still serial).
- **Write**: each page does DATA then **one tprog**; up to `max_planes_per_die`
  pages pipeline DATA/tprog in parallel on the same die.

## Output metrics

- **IOPS / Bandwidth**: host-visible completion rate (xor-adjusted for RAID-like factor)
- **NAND program pages**: actual program operations completed (true NAND write count)
- **Utilization**: sim bandwidth vs channel wire/host/xor ceilings

Writes are **tprog-limited** on NAND; reads are usually channel-bandwidth limited.
