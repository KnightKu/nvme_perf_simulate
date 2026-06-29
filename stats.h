#ifndef STATS_H
#define STATS_H

#include "cmd_sched.h"

perf_iops_t perf_calc_iops(const perf_stats_t *stats);
perf_bandwidth_t perf_calc_bandwidth(const perf_stats_t *stats);

#endif
