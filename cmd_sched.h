#ifndef CMD_SCHED_H
#define CMD_SCHED_H

#include <stdint.h>

// No need to change in genereal
#define TIME_SCALE (1000ULL)

#define PERF_IO_PATTERN_RANDOM 0
#define PERF_IO_PATTERN_SEQUENTIAL 1

#define PERF_STRIPE_CHANNEL_MAJOR 0
#define PERF_STRIPE_GLOBAL_DIE 1
#define PERF_STRIPE_PAGE_ACROSS_CHAN 2
#define PERF_STRIPE_PAGE_DIE_ROTATE 3

#define PERF_WORKLOAD_LEGACY 0
#define PERF_WORKLOAD_FULLDEV_SEQ_READ 1
#define PERF_WORKLOAD_FULLDEV_SEQ_WRITE 2

typedef struct perf_config {
    double cmd_overhead;       // base cmd overhead (us)
    double cmd_overhead_sca;   // cmd overhead when SCA enabled (us)
    int sca;                   // 0=off, 1=on
    int chan_speed;
    int cmd_size;          // NAND read cmd size for tR selection (tr_fast when 4096)
    int block_size;        // host IO bytes; must be N * page_size when > 0
    int ecc_parity_size;   // ECC parity per read page (with page_size)
    int page_size;         // NAND page size for per-page channel xfer
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
    int cmd_pool_size;    /* host cmd pool capacity (default 512) */
    int bus_bandwidth;    /* MB/s; 0 = unlimited */
    int bus_cmd_bytes;    /* bytes per host cmd on bus (default 64) */
    int bus_base_latency; /* fixed bus latency per cmd (us) */
    int use_codeword_buffers; /* 0=legacy page DATA; 1=codeword ping-pong path */
    int codeword_host_bytes;
    int output_buffer_bytes;
    int host_read_chunk_bytes;
    int host_write_chunk_bytes;
    int read_bus_bandwidth; /* MB/s; 0 = unlimited */
    int write_page_coalesce; /* legacy block_size=0: merge host frags before tprog */
    int write_cache;         /* legacy block_size=0: host complete on cache, async flush */
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
    uint64_t pool_rejects;
    uint64_t bus_xfers;
    uint64_t bus_bytes;
    uint64_t read_bus_bytes;
    uint64_t chan_read_wire_bytes;
    uint64_t chan_write_wire_bytes;
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
    /* Channel wire-rate ceiling (host + parity on the bus). */
    double read_ceiling_mbps;
    double write_ceiling_mbps;
    /* Host-byte ceiling at the same wire transfer time as above. */
    double read_ceiling_host_mbps;
    double write_ceiling_host_mbps;
    /* Wire ceiling scaled by die XOR factor (matches reported sim BW). */
    double read_ceiling_xor_mbps;
    double write_ceiling_xor_mbps;
    /* Simulated BW without XOR scaling. */
    double read_mbps_raw;
    double write_mbps_raw;
    /* Utilization: sim vs wire / host / xor ceilings (%). */
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
int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds);
void perf_run(perf_stats_t *stats);
perf_iops_t perf_calc_iops(const perf_stats_t *stats);
perf_bandwidth_t perf_calc_bandwidth(const perf_stats_t *stats);

#endif
