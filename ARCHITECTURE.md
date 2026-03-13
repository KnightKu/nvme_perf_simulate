# 架构文档（Architecture）

## 1. 模块结构
```
main.c         入口程序，加载配置、运行仿真、输出 IOPS
cmd_sched.h    对外接口与配置结构
cmd_sched.c    核心调度逻辑与状态机实现
perf.conf      配置文件
```

## 2. 组件职责
### 2.1 main
- 读取配置文件（`perf.conf` 或命令行指定路径）
- 初始化调度器
- 运行仿真
- 输出总 IOPS 与读/写/擦 IOPS

### 2.2 cmd_sched
#### 配置与初始化
- `perf_config_t`：配置参数载体
- `perf_load_config()`：解析配置文件
- `perf_init()`：初始化全局状态和队列

#### 调度核心
- **Channel 状态机**：IDLE / CMD / DATA
- **Die 状态机**：IDLE / CMD / READ_WAIT / READ_DATA / WRITE_DATA_READY /
  WRITE_DATA / WRITE_WAIT / ERASE_WAIT
- **优先级队列**：每个 die 独立维护 `prio × op` 多队列
- **Suspend 机制**：读可打断 write/erase（阈值控制）

#### 统计
- `perf_stats_t`：记录总数及读/写/擦计数
- `perf_calc_iops()`：计算总/读/写/擦 IOPS

## 3. 数据流
```
配置文件 -> perf_load_config -> perf_init
                        |
                        v
                   perf_run
                        |
                        v
                 perf_calc_iops
                        |
                        v
                      输出
```

## 4. 调度时序（简述）
### 读
CMD -> 等 tR -> 数据传输 -> 完成

### 写
CMD -> 数据传输 -> 等 tPROG -> 完成

### 擦
CMD -> 等 tERASE -> 完成

## 5. 调度优先级
1. 读命令
2. 读数据
3. 写命令
4. 擦命令
5. 写数据

命令队列内部优先级：**high > normal > low**。

## 6. 关键状态与恢复
- 写/擦等待阶段可被读打断（最多 8/15 次）
- 被打断时保存剩余时间；读完成后恢复

## 7. 时间模型
全部时序基于配置：
- `cmd_overhead`、`tr_fast`、`tr`、`tprog`、`terase`、`chan_speed`、
  `cmd_size`、`ecc_parity_size`
- 统一以微秒计时
