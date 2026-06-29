# NAND 仿真演进 Checklist

> 基线：**`l0-base`**（[`L0_BASE.md`](L0_BASE.md)）+ [`design_spec_v1.0.md`](design_spec_v1.0.md)  
> 原则：**NAND 调度为核，逐层向外堆积木**；每步可开关、可回归、可停住。

状态：`[x]` 已有 · `[~]` 部分 · `[ ]` 待做

---

## 0. 分层（自内向外）

```
L5  GC / Erase 背压          §10–§12
L4  SRAM / DDR / vqid        §3
L3  总线 / PCIe / OutBuffer   §7、§9
L2  Host 注入 / QD 完成       §11
L1  Channel 4K 管道 + 仲裁    §5、§8.3
L0  NAND 读 CMD / B2N / Stripe §2、§4、§6
N0  NAND Core（今天）          §8.1、§8.5
```

**规则**：只向下依赖；新功能默认 **OFF**，关开关 = 回退到已验收下层。

---

## 1. N0 — NAND Core（l0-base 今天）

**规格**：§8.1、§8.5（子集）

| 项 | 状态 |
|----|------|
| ch FSM（IDLE/CMD/DATA）+ die 轮询 | `[x]` |
| plane slot，`iwl_slot` 限并发 | `[x]` |
| tR/tr_fast、tprog×nand_type、tERASE | `[x]` |
| Host QD + `element` 停条件 | `[x]` |
| `io_pattern` global die 落点 | `[x]` |
| 多块 IO 同 die multi-plane fan-out | `[x]` |
| 模块拆分（config/timing/host_enqueue/nand_sched/stats） | `[x]` |
| `make test-nand` 11 case | `[x]` |

**与 spec 的主要差距**

| spec | N0 现状 |
|------|---------|
| PT=64K，读按 PT | 按 `page_size=16K`，无 PT |
| tR **按 NAND 读 CMD** | host 块 **1× tR**（leader 页 CMD） |
| 128K → 2 PT → **2× tR** | 1× tR + 8×16K DATA |
| B2N=3 PT 才 tprog | **每 16K page 一次** tprog |
| Channel 4K 管道 | 整页一次 CHAN_DATA |
| Stripe | **已移除**（M3 再加） |
| SRAM/DDR/总线/GC | 无 |

**出口**：开关全 OFF = 当前行为；`make test-nand` PASS。

---

## 2. 里程碑

| # | 层 | 主题 | spec | 依赖 |
|---|-----|------|------|------|
| **M1** | N0+ | 数据单元 + 计数器 | §2 | N0 |
| **M2** | L0 | NAND 读 CMD + tR/tr_fast | §2.1、§8.1 | M1 |
| **M3** | L0 | B2N program | §4.1–4.2 | M1 |
| **M4** | L0 | Stripe 读写 | §4.3、§6 | M2,M3 |
| **M5** | L1 | Channel 4K 管道 + 仲裁 | §5、§8.3 | M2,M3 |
| **M6** | L2 | Host 4K 注入 + 完成语义 | §11 | M5 |
| **M7** | L3 | 总线 + OutBuffer 反压 | §7、§9 | M5,M6 |
| **M8** | L4 | SRAM/DDR + vqid | §3 | M7 |
| **M9** | L5 | GC | §10 | M8 |
| **M10** | L5 | Erase 背压 | §12 | M8,M3 |
| **M11** | 横切 | 优先级 + Suspend | §8.2–8.4 | M5 |

---

## 3. 分步任务

### M1 — 数据单元（零行为变化）

- [ ] `pt_bytes=65536`、`cw_bytes=4096`
- [ ] `b2n_pt_count` ← `nand_type`（TLC=3，QLC=4）
- [ ] Host 块 → PT 数：`block_size / pt_bytes`
- [ ] 计数器：`nand_read_cmds`、`tR_count`、`tr_fast_count`、`b2n_program_count`
- [ ] `use_spec_units=0` 时数值与 N0 一致

**出口**：统计可见，行为不变。

---

### M2 — NAND 读 CMD + tR（★ 读侧核心修正）

| Host 读 | NAND 读 CMD | tR |
|---------|-------------|-----|
| 4K | 1×（4K） | 1× tr_fast |
| 64K | 1×（1 PT） | 1× tR |
| 128K | **2×**（各 1 PT） | **2× tR** |

- [ ] Host 读入队按 PT 拆成多条 NAND 读 CMD
- [ ] 每条独立：CMD → 释 channel → tR/tr_fast → DATA
- [ ] 废弃「整块 1× tR」路径（`use_nand_read_cmd=1`）
- [ ] 改/增 `seq_read_128k`：`tR_count ≈ 2 × host_read`

**文件**：`nand_sched.c`、`sched_internal.h`、`stats.c`

**出口**：tR 次数与 spec §2.1 表一致；128K BW 可能低于 N0（预期）。

---

### M3 — B2N program

- [ ] 写侧按 PT 顺序凑 B2N（TLC 3 PT = 192K）
- [ ] 凑满 1 B2N → **1× tprog**（替代每 page 一次）
- [ ] 4K 随机写 IOPS 下降（凑包慢）
- [ ] `use_b2n=0` 回退 N0

**文件**：`nand_sched.c`（或新 `b2n.c/h`）

**测试**：`rand_write_4k` program 次数 ≈ PT 数/3

---

### M4 — Stripe

- [ ] 读：128K → 2 PT 映射到不同 channel，各独立 tR
- [ ] 写：N 条 stripe B2N 并行填包，齐发 program（§4.3）
- [ ] `stripe_mode` / layout 可配置
- [ ] `use_stripe=0` 回退 M2/M3 非 stripe

**测试**：`seq_read_128k`、`seq_write_128k` + stripe

---

### M5 — Channel 4K 管道

- [ ] 读 pipe：64K PT，16×4K 进出
- [ ] 写 pipe：192K B2N，48×4K 进出；边填边取（§5.2）
- [ ] 仲裁：读 CMD > 读 DATA > 写 CMD > 擦 CMD > 写 DATA
- [ ] 读 channel 释放：pipe 传完且 drained（M7 前 drained=立即）
- [ ] `use_ch_pipe=0` 回退整页 CHAN_DATA

**文件**：新 `chan_pipe.c/h`；`perf_run` 多阶段 tick

---

### M6 — Host 注入

- [ ] `host_write_feed` / `host_read_drain`：4K 速率
- [ ] 完成语义：读=全部 PT/CW 交付；写=全部 B2N program 完成
- [ ] QD / inflight 严格一致；分层统计（Host / NAND CMD / tR / B2N）
- [ ] `use_host_rate=0` 回退隐式无限 Host

---

### M7 — 总线 + OutBuffer

- [ ] `bus_bandwidth`（~95% 效率）、`pcie_bandwidth`
- [ ] `out_buffer_bytes=65536`
- [ ] 读反压链：OB 满 → 总线停 → read pipe → channel 不 idle
- [ ] 写上限：min(PCIe, channel, B2N×tprog)

---

### M8 — SRAM / DDR

- [ ] `sram_bytes`、vqid=64K 粒度
- [ ] `ddr_only` | `sram_if_possible`（§3.2）
- [ ] PT 全部进 write pipe 后释放 vqid
- [ ] stripe N：N×B2N 驻留可统计

---

### M9 — GC

- [ ] GC 前/后 hot write 与 GC 的 SRAM/DDR 分流（§10.2）
- [ ] GC 4K SRAM 粒度、凑 B2N、低优先级、无 stripe

---

### M10 — Erase 背压

- [ ] 先定 §12 策略表（hold B2N / DDR only / stall）
- [ ] ms 级 tERASE、die 冲突、SRAM 满联动

---

### M11 — 优先级 + Suspend（可提前到 M5 后）

- [ ] die high/normal 双队列
- [ ] 读 suspend WRITE_WAIT/ERASE_WAIT（8/15 次）

---

## 4. 推荐顺序

```
N0 (l0-base)
 → M1          # 参数/计数，零变化
 → M2          # ★ 读 CMD + tR（最大语义偏差）
 → M3          # B2N 写
 → M4          # Stripe（可选提前，依赖 M2+M3）
 → M5          # 4K 管道
 → M6 → M7     # Host → 总线/OB
 → M8          # SRAM/DDR
 → M9 → M10    # GC → Erase
 → M11         # Suspend（横切）
```

**M2 优先于 M3/M5**：spec 已明确 tR 按 NAND 读 CMD；改动 confined 在读 FSM，写路径与 N0 测试隔离。

---

## 5. 配置开关

| 开关 | 默认 | 里程碑 | OFF = |
|------|------|--------|-------|
| `use_spec_units` | 0 | M1 | N0 page 模型 |
| `use_nand_read_cmd` | 0 | M2 | N0 整块 1× tR |
| `use_b2n` | 0 | M3 | N0 每 page tprog |
| `use_stripe` | 0 | M4 | 单 die 落点 |
| `use_ch_pipe` | 0 | M5 | 整页 DATA |
| `use_host_rate` | 0 | M6 | 无限 Host |
| `use_bus` | 0 | M7 | 无总线/OB |
| `use_sram_ddr` | 0 | M8 | 无 SRAM |
| `gc_active` | 0 | M9 | 无 GC |
| `use_erase_bp` | 0 | M10 | 无 erase 背压 |
| `use_prio_suspend` | 0 | M11 | 单优先级 |

---

## 6. 测试

| 目录 | 用途 |
|------|------|
| `tests/nand/` | N0 永久回归（开关全 OFF） |
| `tests/mN/` | 里程碑验收 + `tR_count` / `b2n_count` 断言 |

**关键预期变化**

| Case | N0 | M2 |
|------|-----|-----|
| `rand_read_4k` | 1 tr_fast/cmd | 不变 |
| `seq_read_128k` | 1 tR/cmd | **2 tR/cmd** |
| `rand_write_4k` | 每 page tprog | M3 后 ≈ PT/3 |

---

## 7. 代码映射（l0-base 模块）

| 里程碑 | 文件 |
|--------|------|
| M1 | `config.c/h`、`timing.c/h`、`stats.c/h` |
| M2–M4 | `nand_sched.c`、`sched_internal.h` |
| M5 | `chan_pipe.c/h`、`nand_sched.c` |
| M6 | `host_port.c/h`、`cmd_generate.c` |
| M7 | `bus.c/h`、`out_buffer.c/h` |
| M8 | `sram_ddr.c/h` |
| M9–M10 | `gc.c/h`、`space_model.c/h` |

---

## 8. 下一步

**M1 + M2**（约 1–2 迭代）

1. M1 只加参数与计数，不改 FSM  
2. M2 读路径 PT 拆分 + 按 CMD 计 tR  
3. 新建 `tests/m2/seq_read_128k.conf`，断言 `tR_count = 2 × read_cmd`  
4. 存档 M2 baseline（与 N0 分开）

---

| 版本 | 说明 |
|------|------|
| v1.0 | 基于 l0-base（stripe 已移除）+ design_spec v1.0；11 里程碑，N0 基线 |

*冲突时先修 [`design_spec_v1.0.md`](design_spec_v1.0.md) 再改代码。*
