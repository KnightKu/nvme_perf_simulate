#include <stdio.h>

#include "cmd_sched.h"

int main(int argc, char **argv) {
    perf_config_t cfg;
    perf_stats_t stats;
    perf_iops_t iops;
    perf_bandwidth_t bw;
    const char *config_path = perf_default_config_path();

    if (argc > 1) {
        config_path = argv[1];
    }

    perf_config_defaults(&cfg);
    if (perf_load_config(config_path, &cfg) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", config_path);
        return 1;
    }

    if (perf_init(&cfg) != 0) {
        fprintf(stderr, "Failed to init perf model\n");
        return 1;
    }

    perf_run(&stats);
    iops = perf_calc_iops(&stats);
    bw = perf_calc_bandwidth(&stats);

    printf("Performance = %.2f IOPS\n", iops.total);
    printf("Read IOPS = %.2f\n", iops.read);
    printf("Write IOPS = %.2f\n", iops.write);
    printf("Erase IOPS = %.2f\n", iops.erase);

    printf("Read Bandwidth = %.2f MB/s", bw.read_mbps);
    if (bw.read_ceiling_mbps > 0.0) {
        printf(" (ceiling %.2f MB/s)", bw.read_ceiling_mbps);
    }
    printf("\n");

    printf("Write Bandwidth = %.2f MB/s", bw.write_mbps);
    if (bw.write_ceiling_mbps > 0.0) {
        printf(" (ceiling %.2f MB/s)", bw.write_ceiling_mbps);
    }
    printf("\n");

    printf("Total Bandwidth = %.2f MB/s\n", bw.total_mbps);

    perf_cleanup();
    return 0;
}
