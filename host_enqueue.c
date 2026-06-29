#include "host_enqueue.h"

#include "sched_internal.h"

#include <stdlib.h>

static void select_target(int *chan_id, int *die_in_chan) {
    if (g_state.cfg.io_pattern == IO_PATTERN_SEQUENTIAL) {
        int global_die = g_state.next_die;

        g_state.next_die = (g_state.next_die + 1) % g_state.cfg.die_num;
        *chan_id = global_die % g_state.cfg.chan_num;
        *die_in_chan = global_die / g_state.cfg.chan_num;
        return;
    }

    {
        int global_die = rand() % g_state.cfg.die_num;

        *chan_id = global_die % g_state.cfg.chan_num;
        *die_in_chan = global_die / g_state.cfg.chan_num;
    }
}

void enqueue_cmd(int chan_id, int die_in_chan, int op, int act) {
    die_ctx_t *ctx = die_ctx_at(chan_id, die_in_chan);
    queue_push(&ctx->q[op], act);
}

void enqueue_host_cmd(int act, int op) {
    select_target(&g_state.cmd_target_chan[act],
                  &g_state.cmd_target_die[act]);
    enqueue_cmd(g_state.cmd_target_chan[act], g_state.cmd_target_die[act], op,
                act);
}
