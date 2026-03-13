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
```

## 4. 配置文件字段
**时序/性能参数**
- `cmd_overhead`：单条 cmd 等效耗时（微秒）
- `chan_speed`：channel 速率（MT/s）
- `cmd_size`：单次 data 传输的 cmd 大小（字节）
- `ecc_parity_size`：ECC parity 大小（字节）
- `tr`：读 tR（微秒）
- `tprog`：写 tPROG（微秒）
- `terase`：擦 tERASE（微秒）

**资源/规模参数**
- `qd`：队列深度
- `chan_num`：channel 数量
- `die_num`：die 总数
- `plane`：die 内 plane 数（当前不参与调度计算）
- `iwl_slot`：每 die 的队列容量

**命令类型比例**
- `read_ratio` / `write_ratio` / `erase_ratio`

**优先级比例**
- `prio_high_ratio` / `prio_normal_ratio` / `prio_low_ratio`

**运行规模**
- `element`：执行命令数量上限

> 说明：比例字段为相对权重，无需加和为 100，但总和需大于 0。

## 5. 示例配置（默认 perf.conf）
```ini
cmd_overhead=1.7
chan_speed=2400
cmd_size=4096
ecc_parity_size=600
tr=40
tprog=800
terase=3000
qd=512
chan_num=16
die_num=128
plane=4
iwl_slot=256
read_ratio=100
write_ratio=0
erase_ratio=0
prio_high_ratio=0
prio_normal_ratio=100
prio_low_ratio=0
element=32768
```

## 6. 常见问题
- **配置文件加载失败**：检查路径、权限与字段拼写（全部为小写）。
- **输出 IOPS 为 0**：检查 `element`、`qd` 与比例字段是否合理。
