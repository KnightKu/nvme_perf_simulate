#include "host_port.h"

#include "chan_cw_buf.h"
#include "sched_internal.h"

int host_port_init(int qd) {
    (void)qd;
    return 0;
}

void host_port_cleanup(void) {
}

int host_port_try_feed_write(int chan_id, int act, int page_idx, int cw_idx,
                             uint64_t *write_bytes) {
    int slot;
    int chunk = g_state.cfg.host_write_chunk_bytes;

    slot = chan_cw_prog_acquire(chan_id);
    if (slot < 0) {
        return 0;
    }

    chan_cw_prog_mark_ready(chan_id, slot, act, page_idx, cw_idx);
    if (write_bytes) {
        *write_bytes += (uint64_t)chunk;
    }
    return 1;
}

int host_port_process(uint64_t sim_time, uint64_t *write_bytes) {
    (void)sim_time;
    (void)write_bytes;
    return 0;
}
