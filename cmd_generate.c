#include "sched_internal.h"

#include <stdlib.h>

static int select_op(void) {
    int total = g_state.cfg.read_ratio + g_state.cfg.write_ratio +
                g_state.cfg.erase_ratio;
    int r = rand() % total;

    if (r < g_state.cfg.read_ratio) {
        return OP_READ;
    }
    r -= g_state.cfg.read_ratio;
    if (r < g_state.cfg.write_ratio) {
        return OP_WRITE;
    }
    return OP_ERASE;
}

int cmd_generate_try(int tmp_cmd_cnt, int *inflight_cmds) {
    int i;

    if (!g_state.initialized || !inflight_cmds) {
        return 0;
    }
    if (*inflight_cmds >= tmp_cmd_cnt) {
        return 0;
    }

    for (i = 0; i < g_state.cfg.qd; i++) {
        if (g_state.map[i] == 0) {
            int op = select_op();
            int chan_id;
            int die_in_chan;

            g_state.cmd_op[i] = op;
            select_target(&chan_id, &die_in_chan);
            g_state.cmd_target_chan[i] = chan_id;
            g_state.cmd_target_die[i] = die_in_chan;
            enqueue_cmd(chan_id, die_in_chan, op, i);
            g_state.map[i] = 1;
            (*inflight_cmds)++;
            return 1;
        }
    }
    return 0;
}
