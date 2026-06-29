# 设计文档（Design）

## 1. 目标与范围
- **目标**：在不依赖真实硬件的前提下，模拟 NVMe/NAND 侧的读写擦调度行为，输出 IOPS 与带宽估算。
- **范围**：只模拟 **channel + NAND** 侧的调度与时序；不包含内部总线/PCIe 等系统级瓶颈。

## 2. 核心设计原则
1. **读流程**：发 cmd → 释放 channel → 等 tR → channel idle 时传数据 → 结束。
2. **写流程**：发 cmd → 传 program data → 释放 channel → 等 tPROG → 结束。
3. **擦流程**：发 cmd → 释放 channel → 等 tERASE → 结束。
4. **独立性**：channel 之间独立；channel 内 die 轮询。
5. **Channel 仲裁**：读 cmd → 读 data → 写/擦 cmd → 写 data。

## 3. 数据模型
### 3.1 配置（perf_config_t）
从 `perf.conf` 读取，包括时序、规模、比例、优先级权重等。

### 3.2 状态
- **Channel 状态**：IDLE / CMD / DATA
- **Die 状态**：IDLE / CMD / READ_WAIT / READ_DATA / WRITE_DATA_READY /
  WRITE_DATA / WRITE_WAIT / ERASE_WAIT

### 3.3 命令优先级
命令优先级分为 **high / normal / low**。  
- 调度时：高优先级先执行，再 normal，再 low。
- 生成时：由配置文件的 `prio_*_ratio` 决定。

### 3.4 IO 模式与块大小（命令生成）

**地址放置（l0-base）**
- `io_pattern=random`：随机 global die。
- `io_pattern=sequential`：按 global die 顺序递增。
- 多块 IO 在同一 die 上 multi-plane fan-out。

**块大小与 tR（cmd_size / block_size / page_size）**
- `cmd_size`：仅用于读 **tR**（`4096` → `tr_fast`）。
- `block_size`：主机 IO 总字节；须为 `page_size` 整数倍（`block_size=0` 时 legacy 单页）。
- 传数按页拆分：`pages_per_block = block_size / page_size`，每页一次 `CHAN_DATA`。
- 读每页线速：`page_size + ecc_parity_size`；写每页：`page_size + page_parity_size`（如 16K+1952）。
- 写 tPROG：**每页**一次 `tprog`；`host_pages_left` 未归零前不释放 host 命令（仍计 1 次 write IOPS）。
- 例：`block_size=131072`、`page_size=16384` → 8 次页传数 + 一次 tR/块。

### 3.5 带宽统计
- 读/写字节：每完成一页 DATA 累加 `read_bytes_per_page` / `write_bytes_per_page`（= `page_size`）。
- 整块完成时累计等于 `block_size`。
- **sim**：`bytes / 稳态时间 × XOR_factor`（与 IOPS 同因子）。
- **ceiling wire**：各 channel 按 `(host+parity)` 传数时间求和的上限。
- **ceiling host**：同一时间窗内仅计 host 字节的上限。
- **ceiling xor**：wire × XOR_factor，与 sim 对比。

### 3.6 与 fio / 实测对比（范围说明）
本工具只建模 **NAND channel + die 调度**，不含 PCIe、DMA、对齐、文件系统与主机栈。
- fio `128K sequential` 报告的是 **端到端** 带宽，通常 ≤ 本工具 **ceiling wire**。
- 对比时建议：fio 使用相近 `numjobs`≈`chan_num`、`iodepth` 饱和；本工具用 `read_ratio`/`write_ratio` + `block_size=128K` + `io_pattern=sequential`。
- sim 接近 **ceiling xor** 或 **wire** 表示后端 channel 已饱和；明显低于 host ceiling 则查 tR/plane/调度。

## 4. Suspend 机制（读优先）
当同一 die 上存在读与写/擦冲突时，读命令拥有最高优先级，可 **suspend**：
- **写/Program**：最多可被打断 **8 次**
- **擦/Erase**：最多可被打断 **15 次**

实现要点：
1. 仅在 **WRITE_WAIT / ERASE_WAIT** 阶段可被打断（保留剩余时间）。
2. 超过阈值后不再打断，读命令需等待。
3. 读完成后恢复被打断操作并继续倒计时。

## 5. IOPS 统计
- 统计总 IOPS 与 **读/写/擦**三类 IOPS。
- 读完成在数据传输结束时计入。
- 写/擦在等待完成后计入。

## 6. 关键权衡
- **优先级调度**：保证高优先级请求优先执行，但仍保持 die 轮询公平性。
- **简化假设**：不建模内部总线与 PCIe；时序全部基于配置文件参数。

## 7. Plane 并发限制
- 总 plane 数 = `die_num * plane`
- 当总 plane 数 > `iwl_slot` 时：
  - 每 die 最大并发 plane 数 = `iwl_slot / die_num`
  - 要求为 **2 的幂**
