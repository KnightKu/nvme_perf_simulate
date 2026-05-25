#ifndef CMD_SCHED_H
#define CMD_SCHED_H

#include <stdint.h>

// No need to change in genereal
#define TIME_SCALE (1000ULL)

#define PERF_IO_PATTERN_RANDOM 0
#define PERF_IO_PATTERN_SEQUENTIAL 1

#define PERF_STRIPE_CHANNEL_MAJOR 0
#define PERF_STRIPE_GLOBAL_DIE 1

#define PERF_WORKLOAD_LEGACY 0
#define PERF_WORKLOAD_FULLDEV_SEQ_READ 1
#define PERF_WORKLOAD_FULLDEV_SEQ_WRITE 2

typedef struct perf_config {
    double cmd_overhead;       // base cmd overhead (us)
    double cmd_overhead_sca;   // cmd overhead when SCA enabled (us)
    int sca;                   // 0=off, 1=on
    int chan_speed;
    int cmd_size;          // cmd payload for tR selection (bytes)
    int block_size;        // host IO size for data xfer + bandwidth (0 -> cmd_size/page_size)
    int ecc_parity_size;   // ECC parity size in bytes
    int page_size;         // page data size in bytes (write)
    int page_parity_size;  // page parity size in bytes (write)
    int tr_fast;           // tR for 4KiB cmd_size
    int tR;
    int tprog_eff;         // base tPROG (before NAND type scaling)
    int nand_type;         // SLC=1, TLC=3, QLC=4
    int tERASE;
    int qd;
    int chan_num;
    int die_num;
    int plane;
    int iwl_slot;
    int read_ratio;
    int write_ratio;
    int erase_ratio;
    int prio_high_ratio;
    int prio_normal_ratio;
    int prio_low_ratio;
    int io_pattern;   // PERF_IO_PATTERN_* (legacy / non-fulldev)
    int stripe_mode;  // PERF_STRIPE_* (fulldev uses channel_major)
    int workload;     // PERF_WORKLOAD_*
    uint64_t element;
} perf_config_t;

typedef struct perf_stats {
    uint64_t total_cmd;
    uint64_t read_cmd;
    uint64_t write_cmd;
    uint64_t erase_cmd;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t start_time;
    uint64_t end_time;
} perf_stats_t;

typedef struct perf_iops {
    double total;
    double read;
    double write;
    double erase;
} perf_iops_t;

typedef struct perf_bandwidth {
    double read_mbps;
    double write_mbps;
    double total_mbps;
    double read_ceiling_mbps;
    double write_ceiling_mbps;
} perf_bandwidth_t;

const char *perf_default_config_path(void);
void perf_config_defaults(perf_config_t *cfg);
int perf_load_config(const char *path, perf_config_t *cfg);
int perf_init(const perf_config_t *cfg);
void perf_cleanup(void);
int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds);
void perf_run(perf_stats_t *stats);
perf_iops_t perf_calc_iops(const perf_stats_t *stats);
perf_bandwidth_t perf_calc_bandwidth(const perf_stats_t *stats);

#endif
