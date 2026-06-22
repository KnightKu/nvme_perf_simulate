#ifndef CMD_SCHED_H
#define CMD_SCHED_H

#include <stdint.h>

#define TIME_SCALE (1000ULL)

#define PERF_IO_PATTERN_RANDOM 0
#define PERF_IO_PATTERN_SEQUENTIAL 1

#define PERF_STRIPE_CHANNEL_MAJOR 0
#define PERF_STRIPE_GLOBAL_DIE 1

typedef struct perf_config {
    double cmd_overhead;
    double cmd_overhead_sca;
    int sca;
    int chan_speed;
    int cmd_size;
    int block_size;
    int ecc_parity_size;
    int page_size;
    int page_parity_size;
    int tr_fast;
    int tR;
    int tprog_eff;
    int nand_type;
    int tERASE;
    int qd;
    int chan_num;
    int die_num;
    int plane;
    int iwl_slot;
    int read_ratio;
    int write_ratio;
    int erase_ratio;
    int io_pattern;
    int stripe_mode;
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
    uint64_t nand_program_pages;
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
    double read_ceiling_host_mbps;
    double write_ceiling_host_mbps;
    double read_ceiling_xor_mbps;
    double write_ceiling_xor_mbps;
    double read_mbps_raw;
    double write_mbps_raw;
    double read_util_wire_pct;
    double read_util_host_pct;
    double read_util_xor_pct;
    double write_util_wire_pct;
    double write_util_host_pct;
    double write_util_xor_pct;
} perf_bandwidth_t;

const char *perf_default_config_path(void);
void perf_config_defaults(perf_config_t *cfg);
int perf_load_config(const char *path, perf_config_t *cfg);
int perf_init(const perf_config_t *cfg);
void perf_cleanup(void);
void perf_run(perf_stats_t *stats);
perf_iops_t perf_calc_iops(const perf_stats_t *stats);
perf_bandwidth_t perf_calc_bandwidth(const perf_stats_t *stats);

#endif
