#include <stdio.h>

#include "cmd_sched.h"

static void print_read_bandwidth(const perf_bandwidth_t *bw) {
    printf("Read Bandwidth (sim) = %.2f MB/s\n", bw->read_mbps);
    if (bw->read_ceiling_mbps <= 0.0 && bw->read_mbps <= 0.0) {
        return;
    }
    printf("  sim(raw) = %.2f MB/s | ceiling wire = %.2f | host = %.2f | "
           "xor = %.2f MB/s\n",
           bw->read_mbps_raw, bw->read_ceiling_mbps, bw->read_ceiling_host_mbps,
           bw->read_ceiling_xor_mbps);
    printf("  utilization: %.1f%% wire | %.1f%% host | %.1f%% xor\n",
           bw->read_util_wire_pct, bw->read_util_host_pct,
           bw->read_util_xor_pct);
}

static void print_write_bandwidth(const perf_bandwidth_t *bw) {
    printf("Write Bandwidth (sim) = %.2f MB/s\n", bw->write_mbps);
    if (bw->write_ceiling_mbps <= 0.0 && bw->write_mbps <= 0.0) {
        return;
    }
    printf("  sim(raw) = %.2f MB/s | ceiling wire = %.2f | host = %.2f | "
           "xor = %.2f MB/s\n",
           bw->write_mbps_raw, bw->write_ceiling_mbps,
           bw->write_ceiling_host_mbps, bw->write_ceiling_xor_mbps);
    printf("  utilization: %.1f%% wire | %.1f%% host | %.1f%% xor\n",
           bw->write_util_wire_pct, bw->write_util_host_pct,
           bw->write_util_xor_pct);
}

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

    print_read_bandwidth(&bw);
    print_write_bandwidth(&bw);
    printf("Total Bandwidth (sim) = %.2f MB/s\n", bw.total_mbps);

    perf_cleanup();
    return 0;
}
