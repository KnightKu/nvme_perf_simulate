# 设计文档（Design）

## 1. 目标与范围
- **目标**：在不依赖真实硬件的前提下，模拟 NVMe/NAND 侧的读写擦调度行为，输出 IOPS 估算。
- **范围**：只模拟 **channel + NAND** 侧的调度与时序；不包含内部总线/PCIe 等系统级瓶颈。

## 2. 核心设计原则
1. **读流程**：发 cmd → 释放 channel → 等 tR → channel idle 时传数据 → 结束。
2. **写流程**：发 cmd → 传 program data → 释放 channel → 等 tPROG → 结束。
3. **擦流程**：发 cmd → 释放 channel → 等 tERASE → 结束。
4. **独立性**：channel 之间独立；channel 内 die 轮询。
5. **调度优先级**：channel idle 时优先处理**新命令**；仅 **读数据**可优先于写/擦命令。

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
