#include <stdio.h>

#include "cmd_sched.h"

int main(int argc, char **argv) {
    perf_config_t cfg;
    perf_stats_t stats;
    perf_iops_t iops;
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

    printf("Performance = %.2f IOPS\n", iops.total);
    printf("Read IOPS = %.2f\n", iops.read);
    printf("Write IOPS = %.2f\n", iops.write);
    printf("Erase IOPS = %.2f\n", iops.erase);

    perf_cleanup();
    return 0;
}
