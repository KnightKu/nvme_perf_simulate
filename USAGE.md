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
Read/Write/Erase IOPS = ...
Read Bandwidth (sim) = <MB/s with XOR>
  sim(raw) = ... | ceiling wire = ... | host = ... | xor = ... MB/s
  utilization: ...% wire | ...% host | ...% xor
Write Bandwidth (sim) = ...（格式同上）
Total Bandwidth (sim) = ...
```

| 字段 | 含义 |
|------|------|
| **sim** | 稳态窗口内主机字节带宽 × XOR（与 IOPS 同口径） |
| **sim(raw)** | 未乘 XOR，便于对 wire ceiling |
| **ceiling wire** | channel 上传 `(block+parity)` 的聚合线速上限 |
| **ceiling host** | 相同传数时间、仅计 `block_size` 的上限 |
| **ceiling xor** | wire × XOR，与 **sim** 对比 |
| **utilization %** | sim(raw) 对 wire/host；sim 对 xor |

### 与 fio 对比
- 本工具 = **后端 NAND channel 极限**；fio = **端到端**。
- 对齐建议：`fulldev_seq_read` / `fulldev_seq_write`，`block_size=131072`，`qd` 饱和，`numjobs≈chan_num`。
- fio 带宽通常介于 sim 与 ceiling wire 之间；显著低于 host ceiling 时检查 `tR`（`cmd_size`）与 `block_size` 是否分开配置。

### cmd_size 与 block_size
- `cmd_size`：读 **tR**（4K → `tr_fast`）。
- `block_size`：channel **传数**与带宽字节（如 128K 顺序带宽测试）。

## 4. 配置文件字段

**时序/性能参数**
- `cmd_overhead` / `cmd_overhead_sca` / `sca`
- `chan_speed`：channel 速率（MT/s）
- `cmd_size`：读 **tR**（4096 → `tr_fast`）
- `page_size`：NAND 页大小（如 16384）；每页写线速含 `page_parity_size`（如 1952）
- `block_size`：主机 IO 总大小，须为 `page_size` 整数倍；按页多次 `CHAN_DATA`
- `block_size=0`：legacy 单页（读=`cmd_size`+`ecc_parity_size`，写=`page_size`+`page_parity_size`）
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
- **`block_size` 非 `page_size` 整数倍**：`perf_init` 失败。
