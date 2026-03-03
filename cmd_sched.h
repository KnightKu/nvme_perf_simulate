#ifndef CMD_SCHED_H
#define CMD_SCHED_H

#include <stdint.h>

/******************************* Parameters need to clarity ****************************************/
#define CMD_OVERHEAD    ((double)1.7)   // Equivalent time in micro-seconds used for one cmd
#define CHAN_SPEED      (2400)          // MT/s
#define ECC_PARITY      (600)           // How many bytes used for LDPC parity in one 4KiB codeword
#define tR              (40)            // tR in micro-seconds
#define tPROG           (800)           // tPROG in micro-seconds
#define tERASE          (3000)          // tERASE in micro-seconds
#define QD              (512)           // How many ACT cmds used
#define CHAN_NUM        (16)            // How many channels
#define DIE_NUM         (128)           // Total die num
#define PLANE           (4)             // How many planes in one die (Have not adapt 6 plane scenario)
#define IWL_SLOT        (256)           // Max parallelism for IWL read (e.g., 256 for QLTC, 512 for Aspen, but it reduces with channel num)
#define READ_RATIO      (100)           // Read ratio percentage
#define WRITE_RATIO     (0)             // Write ratio percentage
#define ERASE_RATIO     (0)             // Erase ratio percentage
/***************************************************************************************************/

// No need to change in genereal
#define TIME_SCALE      (1000ULL)
#define CMD_TIME        (CMD_OVERHEAD * TIME_SCALE)
#define TREAD           (tR * TIME_SCALE)
#define TPROG           (tPROG * TIME_SCALE)
#define TERASE          (tERASE * TIME_SCALE)
#define DATA_TIME       ((4096 + ECC_PARITY) * TIME_SCALE / CHAN_SPEED)
#define SLOT            (IWL_SLOT)
#define ELEMENT         (1 * 32 * 1024)
#define SLOT_PER_DIE    (SLOT / DIE_NUM)
#define CMD_CNT         (QD)
#define DIE_PER_CHAN    (DIE_NUM / CHAN_NUM)

typedef struct perf_stats {
    uint64_t total_cmd;
    uint64_t start_time;
    uint64_t end_time;
} perf_stats_t;

void perf_init(void);
int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds);
void perf_run(perf_stats_t *stats);
double perf_calc_iops(const perf_stats_t *stats);

#endif
