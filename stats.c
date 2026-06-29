#include "stats.h"

#include "sched_internal.h"

#include <stdio.h>
#include <string.h>

static double pct_of(double sim, double ceiling) {
    if (ceiling <= 0.0) {
        return 0.0;
    }
    return sim / ceiling * 100.0;
}

perf_bandwidth_t perf_calc_bandwidth(const perf_stats_t *stats) {
    perf_bandwidth_t bw;
    double elapsed_us;
    double bytes_to_mbps;

    memset(&bw, 0, sizeof(bw));
    if (!stats || !g_state.initialized ||
        stats->end_time <= stats->start_time) {
        return bw;
    }

    bw.read_ceiling_mbps = g_state.d.read_ceiling_mbps;
    bw.write_ceiling_mbps = g_state.d.write_ceiling_mbps;
    bw.read_ceiling_host_mbps = g_state.d.read_ceiling_host_mbps;
    bw.write_ceiling_host_mbps = g_state.d.write_ceiling_host_mbps;
    bw.read_ceiling_xor_mbps = g_state.d.read_ceiling_xor_mbps;
    bw.write_ceiling_xor_mbps = g_state.d.write_ceiling_xor_mbps;

    elapsed_us =
        (double)(stats->end_time - stats->start_time) / (double)TIME_SCALE;
    if (elapsed_us <= 0.0) {
        return bw;
    }

    bytes_to_mbps = 1000000.0 / 1048576.0 / elapsed_us;
    bw.read_mbps_raw = (double)stats->read_bytes * bytes_to_mbps;
    bw.write_mbps_raw = (double)stats->write_bytes * bytes_to_mbps;
    bw.read_mbps = bw.read_mbps_raw * g_state.d.xor_factor;
    bw.write_mbps = bw.write_mbps_raw * g_state.d.xor_factor;
    bw.total_mbps = bw.read_mbps + bw.write_mbps;

    bw.read_util_wire_pct = pct_of(bw.read_mbps_raw, bw.read_ceiling_mbps);
    bw.read_util_host_pct =
        pct_of(bw.read_mbps_raw, bw.read_ceiling_host_mbps);
    bw.read_util_xor_pct = pct_of(bw.read_mbps, bw.read_ceiling_xor_mbps);
    bw.write_util_wire_pct = pct_of(bw.write_mbps_raw, bw.write_ceiling_mbps);
    bw.write_util_host_pct =
        pct_of(bw.write_mbps_raw, bw.write_ceiling_host_mbps);
    bw.write_util_xor_pct = pct_of(bw.write_mbps, bw.write_ceiling_xor_mbps);
    return bw;
}

perf_iops_t perf_calc_iops(const perf_stats_t *stats) {
    perf_iops_t iops = {0.0, 0.0, 0.0, 0.0};
    double elapsed;
    double scale;
    uint64_t total_effective;

    if (!stats || !g_state.initialized ||
        stats->end_time <= stats->start_time) {
        return iops;
    }

    elapsed = (stats->end_time - stats->start_time) / (double)TIME_SCALE;
    if (elapsed <= 0.0) {
        return iops;
    }

    scale = 1000000.0 / elapsed;
    total_effective =
        (stats->total_cmd > (uint64_t)g_state.cfg.qd)
            ? (stats->total_cmd - (uint64_t)g_state.cfg.qd)
            : 0;

    iops.total = (double)total_effective * scale * g_state.d.xor_factor;
    iops.read = (double)stats->read_cmd * scale * g_state.d.xor_factor;
    iops.write = (double)stats->write_cmd * scale * g_state.d.xor_factor;
    iops.erase = (double)stats->erase_cmd * scale * g_state.d.xor_factor;
    return iops;
}

void perf_print_nand_counters(const perf_stats_t *stats) {
    if (!stats) {
        return;
    }
    printf("NAND read CMDs = %llu (tR=%llu tr_fast=%llu b2n_prog=%llu)\n",
           (unsigned long long)stats->nand_read_cmds,
           (unsigned long long)stats->tR_count,
           (unsigned long long)stats->tr_fast_count,
           (unsigned long long)stats->b2n_program_count);
    if (g_state.initialized) {
        printf("Spec units: pt=%d cw=%d b2n_pt=%d host_pt=%d pages_per_pt=%d "
               "(use_spec_units=%d)\n",
               g_state.d.pt_bytes, g_state.d.cw_bytes, g_state.d.b2n_pt_count,
               g_state.d.host_pt_count, g_state.d.pages_per_pt,
               g_state.cfg.use_spec_units);
    }
}
