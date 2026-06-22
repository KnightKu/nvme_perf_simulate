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

## Output metrics

- **IOPS / Bandwidth**: host-visible completion rate (xor-adjusted for RAID-like factor)
- **NAND program pages**: actual program operations completed (true NAND write count)
- **Utilization**: sim bandwidth vs channel wire/host/xor ceilings

Writes are **tprog-limited** on NAND; reads are usually channel-bandwidth limited.
