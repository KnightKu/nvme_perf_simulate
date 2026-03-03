#include <stdio.h>

#include "cmd_sched.h"

int main() {
    perf_stats_t stats;

    perf_init();
    perf_run(&stats);

    printf("Performance = %.2f IOPS\n", perf_calc_iops(&stats));
    return 0;
}
