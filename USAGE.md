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

**128K sequential read** (8 pages × 16 KiB, page stripe across channels)

```ini
read_ratio=100
write_ratio=0
block_size=131072
io_pattern=sequential
stripe_mode=page_stripe
```

### Stripe modes

| Mode | Use case |
|------|----------|
| `channel_major` | Random 4K; round-robin channel then die |
| `global_die` | Sequential 4K across global die index |
| `page_stripe` | Multi-page blocks (`block_size > page_size`): page `p` → `chan=(base+p)%chan_num`, `die=(base+p)/chan_num % die_per_chan` |

With `page_stripe`, one host block fans out across channels:

- **Read**: 1× CMD + 1× tR on page-0 location, then all pages transfer DATA in parallel on their channels.
- **Write**: 1× CMD on page-0 location, then each page does DATA + tprog on its channel in parallel (up to 8 channels for 128K).

Sequential IO advances `stripe_base` by `pages_per_block` per host command.

Multi-page blocks without `page_stripe` keep all pages on one die and use multi-plane fan-out (`max_planes_per_die = iwl_slot / die_num`, default **4**).

## Output metrics

- **IOPS / Bandwidth**: host-visible completion rate (xor-adjusted for RAID-like factor)
- **NAND program pages**: actual program operations completed (true NAND write count)
- **Utilization**: sim bandwidth vs channel wire/host/xor ceilings

Writes are **tprog-limited** on NAND; reads are usually channel-bandwidth limited.
