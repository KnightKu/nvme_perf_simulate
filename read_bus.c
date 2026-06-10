#include "read_bus.h"

#include "chan_cw_buf.h"
#include "output_buffer.h"
#include "sched_internal.h"

#include <stdint.h>

static int s_rr_chan;

void read_bus_init(void) {
    s_rr_chan = 0;
}

void read_bus_cleanup(void) {
    s_rr_chan = 0;
}

static int cw_wire_bytes(void) {
    return g_state.d.codeword_wire_bytes;
}

static uint64_t cw_read_bus_time(void) {
    uint64_t bw = (uint64_t)g_state.cfg.read_bus_bandwidth;

    if (bw == 0) {
        return 0;
    }
    return (uint64_t)cw_wire_bytes() * 1000000ULL / bw;
}

int read_bus_process(uint64_t sim_time, uint64_t *read_bus_bytes) {
    int ch;
    int slot;
        const cw_slot_t *ps;
    int wire = cw_wire_bytes();
    int host = g_state.cfg.codeword_host_bytes;
    uint64_t xfer_time;
    int pushed = 0;

    if (g_state.read_bus_busy_until > sim_time) {
        return 0;
    }

    for (ch = 0; ch < g_state.cfg.chan_num; ch++) {
        int try_ch = (s_rr_chan + ch) % g_state.cfg.chan_num;

        slot = chan_cw_read_pick_ready(try_ch);
        if (slot < 0) {
            continue;
        }

        ps = chan_cw_read_slot_info(try_ch, slot);
        if (!ps || output_buffer_push(ps->act, host) != 0) {
            break;
        }

        chan_cw_read_release(try_ch, slot);
        if (read_bus_bytes) {
            *read_bus_bytes += (uint64_t)wire;
        }
        pushed = 1;
        s_rr_chan = (try_ch + 1) % g_state.cfg.chan_num;

        xfer_time = cw_read_bus_time();
        if (xfer_time > 0) {
            g_state.read_bus_busy_until = sim_time + xfer_time;
        }
        break;
    }

    return pushed;
}

uint64_t read_bus_next_event(uint64_t sim_time) {
    if (g_state.cfg.read_bus_bandwidth == 0) {
        return UINT64_MAX;
    }
    if (g_state.read_bus_busy_until > sim_time) {
        return g_state.read_bus_busy_until;
    }
    return UINT64_MAX;
}
