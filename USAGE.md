# 使用文档（Usage）

## 1. 构建
```bash
make clean && make
```
输出可执行文件：`nvme_perf_model`

## 2. 运行
默认配置文件为 `perf.conf`：
```bash
./nvme_perf_model
```

指定配置文件：
```bash
./nvme_perf_model /path/to/perf.conf
```

## 3. 输出说明
```
Performance = <total_iops> IOPS
Read IOPS = <read_iops>
Write IOPS = <write_iops>
Erase IOPS = <erase_iops>
Read Bandwidth = <read_mbps> MB/s (ceiling <read_ceiling> MB/s)
Write Bandwidth = <write_mbps> MB/s (ceiling <write_ceiling> MB/s)
Total Bandwidth = <total_mbps> MB/s
```

- **ceiling**：按 `chan_num` 与各 channel 的 `data_time` 估算的理论 channel 传数上限（未计 tR/cmd 争用）。
- 带宽统计窗口与 IOPS 相同（warmup 之后 `start_time` ~ `end_time`），并应用与 IOPS 相同的 XOR 折减因子。

## 4. 配置文件字段

**时序/性能参数**
- `cmd_overhead` / `cmd_overhead_sca` / `sca`
- `chan_speed`：channel 速率（MT/s）
- `cmd_size`：用于 tR 分支选择（4KiB 时用 `tr_fast`）
- `block_size`：主机 IO 大小（字节）；`0` 表示读用 `cmd_size`、写用 `page_size`
- `ecc_parity_size` / `page_size` / `page_parity_size`
- `tr_fast` / `tr` / `tprog_eff` / `nand_type` / `terase`

**资源/规模**
- `qd` / `chan_num` / `die_num` / `plane` / `iwl_slot`

**命令类型比例**（`workload=legacy` 时生效）
- `read_ratio` / `write_ratio` / `erase_ratio`

**Workload（负载类型）**
- `legacy` / `mixed` / `default`：按 ratio 随机 read/write/erase
- `fulldev_seq_read` / `seq_read_bw`：全盘顺序读极限（固定 read，channel stripe）
- `fulldev_seq_write` / `seq_write_bw`：全盘顺序写极限（固定 write，channel stripe）

**Stripe / IO pattern**
- `stripe_mode`：
  - `channel_major`（默认）：round-robin channel，每 channel 内 die 递增
  - `global_die`：使用 `io_pattern` 的 global die 顺序
- `io_pattern`（`stripe_mode=global_die` 或 legacy 时）：
  - `random` / `sequential`（`seq`）

**优先级**
- `prio_high_ratio` / `prio_normal_ratio` / `prio_low_ratio`

**运行规模**
- `element`：完成命令数上限

> 配置 key 不区分大小写；行内 `#` 与 `//` 注释会被忽略。

## 5. 示例

**全盘顺序读极限（默认 perf.conf）**
```ini
workload=fulldev_seq_read
block_size=131072
stripe_mode=channel_major
qd=1024
chan_num=16
read_ratio=100
```

**全盘顺序写极限**
```ini
workload=fulldev_seq_write
block_size=16384
stripe_mode=channel_major
write_ratio=100
read_ratio=0
```

**传统随机读（legacy）**
```ini
workload=legacy
io_pattern=random
block_size=0
cmd_size=4096
```

## 6. 常见问题
- **配置文件加载失败**：检查 key 拼写、`workload`/`stripe_mode`/`io_pattern` 取值。
- **带宽远低于 ceiling**：多为 tR、plane 槽或 channel 仲裁争用；可增大 `block_size` 或检查 `iwl_slot`。
- **plane 并发限制**：`die_num * plane > iwl_slot` 时要求 `iwl_slot / die_num` 为 2 的幂。
