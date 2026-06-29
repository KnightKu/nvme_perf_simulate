# NAND 后端性能仿真设计规格 v1.0

> 本文档整理自 v1.0 需求讨论，描述 Host 至 NAND 的完整数据通路与资源模型。

---

## 1. 目标与范围

### 1.1 评估场景

| 场景 | 说明 |
|------|------|
| 大块顺序读/写 | 64K / 128K |
| 4K 随机读/写 | 含随机写凑 B2N |
| 4K 读写混合 | mixed workload |

### 1.2 建模层次

```
PCIe → 总线 → SRAM / DDR → Channel R/W Buffer → Die / Plane → NAND (tR / tprog / tERASE)
```

- **Channel 聚合带宽**远高于 PCIe、总线、SRAM/DDR，后端通常不是首瓶颈；**读路径**上 OutBuffer（64K）对接 PCIe，可能成为读带宽瓶颈并形成反压。
- **写路径**上限多为 PCIe 注入速率；瓶颈可能在 Channel 堵塞或 NAND program/erase，而非 Host 侧反压。

---

## 2. 基本数据单元

| 术语 | 大小 | 说明 |
|------|------|------|
| **Codeword (CW)** | 4 KB | 最小传输粒度（Host、总线、Buffer 管道进出） |
| **Page Type (PT)** | 64 KB | 16 × 4K；读路径按 PT 组织 channel 占用 |
| **vqid** | = 1 PT (64 KB) | SRAM 空间管理单位；传完 1 PT 释放 1 vqid |
| **B2N** | TLC: 3 PT (192 KB)<br>QLC: 4 PT (256 KB) | **Program 单位**；buffer 内凑满 1 B2N 才下发 program |
| **tprog** | `tprog_eff × nand_type` | TLC×3，QLC×4；与 B2N 内 PT 数一致 |
| **tR** | **1× / NAND 读 CMD**（非 4K 路径） | 每条 **NAND 侧读 CMD** 触发 1 次；读范围通常为 **1 PT（64K）** |
| **tr_fast (tR_fast)** | **1× / NAND 读 CMD**（4K 路径） | 每条 **4K 粒度 NAND 读 CMD** 触发 1 次 |

### 2.1 NAND 读时序：tR 与 tr_fast

读延迟在仿真中抽象为 **tR 等待**（die 进入 `READ_WAIT`，channel 已释放）。**tR / tr_fast 的计费单元是 NAND 读 CMD**，不是 Host IO 命令。

| 参数 | 典型值 | 适用 NAND 读 CMD | 计费 |
|------|--------|------------------|------|
| **tr_fast** | 40 µs | **4K 读路径**：NAND 读 CMD 范围为 **4K（1 CW）** | 每发 **1 条** NAND 读 CMD → **1 次** tr_fast |
| **tR** | 40 µs（可独立配置） | **非 4K 读路径**：NAND 读 CMD 范围为 **1 PT（64K）** | 每发 **1 条** NAND 读 CMD → **1 次** tR |

**Host 读与 NAND 读 CMD 的映射**

| Host 读粒度 | NAND 读 CMD 拆分 | tR 选用 | 计费次数 |
|-------------|------------------|---------|----------|
| **4K** | 1 条 Host 读 ≈ **1 条** NAND 读 CMD（4K） | tr_fast | **1** |
| **64K（1 PT）** | 1 条 Host 读 = **1 条** NAND 读 CMD（1 PT） | tR | **1** |
| **128K（2 PT）** | 1 条 Host 读 → **2 条** NAND 读 CMD（各 1 PT） | tR × 2 | **2** |
| **128K + stripe** | 同上，各 PT 按条带落到对应 channel/die | tR × PT 数 | **= PT 数** |

- **4K 随机读**可与 **1 Host 读 = 1 NAND 读 CMD** 等效，走 **tr_fast**。
- **大于 1 PT 的 Host 读**须按 **PT（64K）** 拆成 **多条 NAND 读 CMD**；每条 CMD 独立经历 **读 CMD → tR → DATA**，各自计 **1 次 tR**（非 4K 路径）或对应路径时序。
- 拆分后各 PT 可 **并行** 在不同 channel/die 上传 DATA（受 stripe 布局与 plane 并发约束，见 §6）。

**读流程中的挂接点**（与 §8.1 一致）

```
NAND 读 CMD（1 PT 或 4K 范围）→ 释放 channel → tR / tr_fast → channel idle 传 DATA（4K 管道，§5）
```

配置键别名：`tr_fast` ≡ `tR_fast`（不区分大小写）。

---

## 3. Host 侧缓冲：SRAM 与 DDR

### 3.1 特性

| 类型 | 容量 | 带宽 |
|------|------|------|
| **SRAM** | 小（几 MB～几十 MB） | 高 |
| **DDR** | 大 | 相对较低 |

### 3.2 写入模式

| 模式 | 行为 |
|------|------|
| **DDR only** | 数据仅经 DDR，不使用 SRAM |
| **SRAM if possible** | 数据 **总是** 进 DDR；若 SRAM 有空间则 **额外** 送一份到 SRAM。仿真可通过 **DDR/SRAM 比例** 控制双写行为 |

### 3.3 释放条件

- DDR / SRAM 中的数据须 **全部传到对应 channel 的 write buffer（program 管道）** 后才释放该路径上的占用。
- **SRAM 容量判定（vqid）**：
  - 至少保留 **1 vqid（64 KB）** 粒度管理；
  - 每传完 **1 PT** 释放 **1 vqid**；
  - 若剩余空间 **不足 64 KB**，视为 SRAM 已满。

### 3.4 Stripe 对 SRAM 的影响

- Stripe = N 时，需 **同时驻留 N 个 B2N**（各 stripe 各一份），填满后 **一起** 轮询下发；
- SRAM 占用 ≈ N × B2N 规模，显著高于非 stripe 模式。

---

## 4. B2N 与 Program

### 4.1 基本规则

- **B2N** 是 program 的逻辑单位；物理 program 时间 = `tprog_eff × nand_type`（TLC/QLC）。
- 写路径在 buffer 中 **凑满 1 B2N** 才向 NAND 侧发起 program（经 channel write buffer → die/plane）。

### 4.2 填 B2N 与下发（无 Stripe）

- 顺序：**B2N pt0 → B2N pt1 → B2N pt2**（TLC 三 PT 凑满一发）；
- 凑满 **1 个 B2N** 即按 **channel 轮询** 下发 program；
- 4K 随机写同样需 **凑 B2N**，仅凑包速度慢于顺序写。

### 4.3 填 B2N 与下发（Stripe = N）

- Buffer 内同时维护 **N 条 stripe 的 B2N**；
- 填包顺序（以 N=3 为例）：
  ```
  B2N0-PT0, B2N1-PT0, B2N2-PT0,
  B2N0-PT1, B2N1-PT1, B2N2-PT1,
  B2N0-PT2, B2N1-PT2, B2N2-PT2  → 三个 B2N 齐，轮询发 program
  ```
- 下发：在对应 channel / die 上 **轮询发送**（与 stripe 布局一致）。

---

## 5. Per-Channel Read / Write Buffer

### 5.1 原设计与仿真抽象

- 原硬件：每 channel **2 × 8K** codeword ping-pong；实际传输 **4K** 粒度。
- **仿真管道**（简化）：
  - **读**：每 channel 一个 **64K（1 PT）** 管道，进出均为 **4K** 粒度；
  - **写**：每 channel 一个 **192K（1 B2N，TLC）** 管道，进出均为 **4K** 粒度。

### 5.2 管道语义：边填边取

- Die/plane 侧从 **第一个 4K 进入** 即按 4K 粒度 poll 取走；
- 总传输时间短；**最后一个 4K 进入后可立即被取走**，中间无额外等待；
- **时间建模**：按 4K 次传输累加 channel 占用时间；管道满/空决定反压。

### 5.3 读路径 Channel 占用

- **读引擎**按 4K 粒度 **轮询** 各 channel，向 read buffer 填数；
- **64K 全部传完** 后 channel 才可释放（例如 16 channel 各 64K → 轮询 **16 × 16 = 256 次 4K** 量级，具体取决于布局）；
- **Read buffer 数据被总线取走后**，channel 才释放变 idle，才能从 NAND 拉下一批数据。

### 5.4 写路径 Channel 占用

- 4K 粒度填入 write buffer（B2N 管道）；
- TLC：**48 次 4K**（192K / 4K）凑满 1 B2N 后，向下 program；
- 与 stripe、轮询下发规则叠加（见 §4、§6）。

---

## 6. Stripe 对 Program 与读的影响

### 6.1 Program（写）

| 模式 | 行为 |
|------|------|
| **非 stripe** | 凑齐 **1 个 B2N** → channel 轮询下发 program |
| **stripe = N** | 凑齐 **N 个 B2N**（每条 stripe 一份）→ **一起** 轮询下发到对应 channel/die |

### 6.2 读（无 B2N 概念）

- 读以 **1 PT（64K）** 为 **NAND 读 CMD** 单位；layout 受 **写 stripe 布局** 影响（见 §2.1 Host→NAND 拆分）。
- **非 stripe**：例如 ch0-die0 上 PT 0,1,2… 顺序读。
- **stripe**：读按条带在 channel 间 **轮流** 分配，例如 128K Host 读（2 PT）：PT0 → ch0，PT1 → ch1，各 **独立 1 条 NAND 读 CMD + 1 次 tR**。
- Stripe 同时增加 **SRAM 驻留**（N 个 B2N 并行填满，见 §3.4）。

---

## 7. 总线效率

- 总线同时传 **CMD** 与 **DATA**；DATA 占主导。
- CMD 开销可忽略，总线有效带宽按 **~95%**（可配置）折算。

---

## 8. 调度与仲裁

### 8.1 Channel / Die 流程（NAND 侧）

| 操作 | 流程 |
|------|------|
| **读** | **NAND 读 CMD** → 释放 channel → **tR / tr_fast**（§2.1，按 CMD 计次）→ channel idle 传 DATA → 结束 |
| **写 (program)** | CMD → 传 program DATA → 释放 channel → tPROG → 结束 |
| **擦 (trim)** | CMD → 释放 channel → tERASE → 结束 |

- Channel 之间 **独立**；channel 内 **die 轮询**。
- Channel 在任意时刻传 **CMD 或 DATA**（二选一）。

### 8.2 命令优先级（Die 队列）

- **high / normal** 两档；调度时 high 优先。

### 8.3 Channel 传输优先级

（从高到低）

1. 读 CMD  
2. 读 DATA  
3. 写 CMD  
4. 擦 CMD  
5. 写 DATA  

### 8.4 Suspend

- 读可 **suspend** 进行中的 write/erase（write 最多 8 次，erase 最多 15 次）；
- 仅 **WRITE_WAIT / ERASE_WAIT** 可打断；保存剩余时间，读完成后恢复。

### 8.5 Plane 并发

- 总 plane 数 = `die_num × plane`；
- 若总 plane > `iwl_slot`：每 die 最大并发 plane = `iwl_slot / die_num`（须为 **2 的幂**）；
- 每 die 最大并发 **读** plane 数同样受 `iwl_slot / die_num` 限制。

---

## 9. 数据传输链路

```
PCIe ←→ OutBuffer(64K) ←→ 总线 ←→ SRAM / DDR ←→ Channel R/W Buffer ←→ NAND
```

| 链路 | 读 | 写 |
|------|----|----|
| **瓶颈倾向** | OutBuffer → PCIe；Host 取数慢则总线无法送入 OB，反压至 read buffer → channel 利用率 < 100% | 上限多为 PCIe；总线可持续消耗 inbuffer；瓶颈在 channel / NAND |
| **反压** | 有（OB 满 → 总线 hold → read buffer → channel） | 无 Host 侧反压（受 PCIe 上限约束） |
| **Channel 释放（读）** | Read buffer 数据被总线取走后，channel 才 idle |

- 四段带宽（PCIe、总线、SRAM/DDR、Channel）在同一通路上；**Channel 聚合能力通常最高**。

---

## 10. GC 的影响

### 10.1 与 Hot Write 的资源竞争

- GC **读、写** 均占用 SRAM，与 **hot write** 竞争。
- GC 读可能 **partial read**，凑满 **1 B2N** 再发 GC program；一进一出占用 SRAM/DDR 带宽。
- GC **无 stripe**；凑够 1 B2N 即下发。

### 10.2 GC 未启动 vs 已启动

| 阶段 | Hot write | GC | SRAM |
|------|-----------|-----|------|
| **GC 未启动** | 可走 SRAM if possible | — | 可全给 hot write |
| **GC 已启动** | **DDR only** | **全走 SRAM** | 全让给 GC |

- 依据：GC 启动时 WA≈2～3，GC 所需带宽为 hot 写的数倍；hot 写已受限，**DDR only 足够**，SRAM 让给 GC。

### 10.3 GC 对 SRAM 的占用粒度

- GC 以 **4K** 为单位占用 SRAM（**非 vqid**）；
- GC 读按 4K 填入，凑满 1 B2N（如 48×4K）后 program；
- 每向 write buffer 传 **1×4K** 释放 **1×4K** SRAM；
- 第 48 个 4K 传完时，前面 47×4K 大致已传至 NAND，**transfer 结束即可释放**，**不必等 program 完成**（与 hot write 走 SRAM 类似）。
- 硬件可预留可配置 DDR 给 GC；**v1.0 模型暂仅考虑 SRAM 情形**。

### 10.4 GC 优先级

- GC 读为 **低优先级**；
- 受 die 冲突、高优先级 suspend 等调度规则约束。

---

## 11. 建模方法（v1.0）

### 11.1 结构

- **ch → die → plane** 三级建模，**事件驱动**（CMD 为事件源）；
- **Channel 层**：调度与仲裁；
- **Read/Write buffer**：**管道模型**，4K 粒度边填边取；
- Channel 内 die **顺序轮询**；
- Die 维护 **high / normal** 两路 CMD 队列；
- Plane 为 **独立 worker**（并发受 §8.5 限制）。

### 11.2 时间放大

- 将 tR、tprog、数据传输等时间 **放大**（如 ×10000），以抵消仿真框架、线程调度等 overhead，使测得带宽接近物理比例。

---

## 12. Erase 与异常事件

### 12.1 问题

- Erase 时间 **ms 级**，与 program/read 争用 **die**，产生冲突与等待；
- 进行中的 **B2N program** 可能被 hold；
- SRAM 占用无法释放 → **SRAM 不足** → 数据被迫走 DDR → **带宽目标无法保证**。

### 12.2 待 v1.0 明确的控制策略（开放项）

- Erase 等待期间，hot write 是否 **强制 DDR only**？
- 是否 **暂停** 新 B2N 下发直至 erase 完成？
- SRAM vqid / GC 模式切换的 **优先级与阈值**？
- 与 §3.2 两种写入模式的 **联动规则**？

> 需在实现前定稿 **erase 冲突时的 backpressure 策略表**。

---

## 13. 开放问题汇总

1. **B2N 管道时间**：最后一包 4K「零等待」与 channel 计时如何在事件模型里闭合？  
2. **Stripe 读 layout**：写 stripe 与读 stripe 映射表是否配置化？  
3. **GC 仅 SRAM**：DDR 预留是否在 v1.1 强制纳入？  
4. **Erase hold**：B2N 半满、SRAM 满、DDR 带宽三者同时触发时的 **优先级**？  
5. **QLC vs TLC**：B2N PT 数、program 时间是否仅通过 `nand_type` 切换？  
6. **统计口径**：Host 完成、NAND 读 CMD、tR 次数、B2N 下发、program 完成、SRAM vqid 释放 — 各报表对应哪一层？

---

*文档版本：v1.0 | 状态：需求整理，待评审*
