#include "bus_xfer.h"

#include "cmd_pool.h"
#include "sched_internal.h"

#include <stdint.h>

void bus_xfer_init(void) {
    g_state.bus.state = BUS_IDLE;
    g_state.bus.active_act = -1;
    g_state.bus.busy_until = 0;
}

void bus_xfer_cleanup(void) {
    g_state.bus.state = BUS_IDLE;
    g_state.bus.active_act = -1;
    g_state.bus.busy_until = 0;
}

static uint64_t bus_cmd_xfer_time(void) {
    uint64_t xfer_time = g_state.d.bus_base_latency;

    if (g_state.d.bus_bandwidth_mbps != UINT64_MAX &&
        g_state.d.bus_bandwidth_mbps > 0) {
        xfer_time += (uint64_t)g_state.cfg.bus_cmd_bytes * TIME_SCALE /
                     g_state.d.bus_bandwidth_mbps;
    }
    return xfer_time;
}

static int bus_complete_xfer(uint64_t *bus_xfers, uint64_t *bus_bytes) {
    int act = g_state.bus.active_act;

    enqueue_cmd(g_state.cmd_target_chan[act], g_state.cmd_target_die[act],
                g_state.cmd_op[act], g_state.cmd_prio[act], act);
    g_state.bus.state = BUS_IDLE;
    g_state.bus.active_act = -1;
    g_state.bus.busy_until = 0;
    if (bus_xfers) {
        (*bus_xfers)++;
    }
    if (bus_bytes) {
        (*bus_bytes) += (uint64_t)g_state.cfg.bus_cmd_bytes;
    }
    return 1;
}

int bus_process(uint64_t sim_time, uint64_t *bus_xfers, uint64_t *bus_bytes) {
    int progressed = 0;

    if (g_state.bus.state == BUS_XFER &&
        sim_time >= g_state.bus.busy_until) {
        progressed |= bus_complete_xfer(bus_xfers, bus_bytes);
    }

    if (g_state.bus.state == BUS_IDLE && !cmd_pool_empty()) {
        int act = cmd_pool_pop();

        g_state.bus.state = BUS_XFER;
        g_state.bus.active_act = act;
        g_state.bus.busy_until = sim_time + bus_cmd_xfer_time();
        if (sim_time >= g_state.bus.busy_until) {
            progressed |= bus_complete_xfer(bus_xfers, bus_bytes);
        }
    }

    return progressed;
}

uint64_t bus_xfer_next_event(uint64_t sim_time) {
    if (g_state.bus.state == BUS_XFER &&
        g_state.bus.busy_until > sim_time) {
        return g_state.bus.busy_until;
    }
    return UINT64_MAX;
}
