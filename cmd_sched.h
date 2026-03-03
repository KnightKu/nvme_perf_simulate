#ifndef CMD_SCHED_H
#define CMD_SCHED_H

#include <stdint.h>

// No need to change in genereal
#define TIME_SCALE (1000ULL)

typedef struct perf_config {
    double cmd_overhead;
    int chan_speed;
    int ecc_parity;
    int tR;
    int tPROG;
    int tERASE;
    int qd;
    int chan_num;
    int die_num;
    int plane;
    int iwl_slot;
    int read_ratio;
    int write_ratio;
    int erase_ratio;
    uint64_t element;
} perf_config_t;

typedef struct perf_stats {
    uint64_t total_cmd;
    uint64_t read_cmd;
    uint64_t write_cmd;
    uint64_t erase_cmd;
    uint64_t start_time;
    uint64_t end_time;
} perf_stats_t;

typedef struct perf_iops {
    double total;
    double read;
    double write;
    double erase;
} perf_iops_t;

const char *perf_default_config_path(void);
void perf_config_defaults(perf_config_t *cfg);
int perf_load_config(const char *path, perf_config_t *cfg);
int perf_init(const perf_config_t *cfg);
void perf_cleanup(void);
int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds);
void perf_run(perf_stats_t *stats);
perf_iops_t perf_calc_iops(const perf_stats_t *stats);

#endif
