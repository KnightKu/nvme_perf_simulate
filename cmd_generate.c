#include "cmd_generate.h"

#include "cmd_pool.h"
#include "sched_internal.h"

#include <stdlib.h>

static int select_op(void) {
    int total;
    int r;

    if (g_state.cfg.workload == WORKLOAD_FULLDEV_SEQ_READ) {
        return OP_READ;
    }
    if (g_state.cfg.workload == WORKLOAD_FULLDEV_SEQ_WRITE) {
        return OP_WRITE;
    }

    total = g_state.cfg.read_ratio + g_state.cfg.write_ratio +
            g_state.cfg.erase_ratio;
    r = rand() % total;
    if (r < g_state.cfg.read_ratio) {
        return OP_READ;
    }
    r -= g_state.cfg.read_ratio;
    if (r < g_state.cfg.write_ratio) {
        return OP_WRITE;
    }
    return OP_ERASE;
}

static int select_prio(void) {
    int total = g_state.cfg.prio_high_ratio + g_state.cfg.prio_normal_ratio +
                g_state.cfg.prio_low_ratio;
    int r = rand() % total;

    if (r < g_state.cfg.prio_high_ratio) {
        return PRIO_HIGH;
    }
    r -= g_state.cfg.prio_high_ratio;
    if (r < g_state.cfg.prio_normal_ratio) {
        return PRIO_NORMAL;
    }
    return PRIO_LOW;
}

int cmd_generate_try(int tmp_cmd_cnt, int *inflight_cmds,
                     uint64_t *pool_rejects) {
    int i;
    int act;
    int chan_id;
    int die_in_chan;

    if (!g_state.initialized || !inflight_cmds) {
        return 0;
    }

    if (*inflight_cmds >= tmp_cmd_cnt) {
        return 0;
    }

    if (cmd_pool_is_full()) {
        if (pool_rejects) {
            (*pool_rejects)++;
        }
        return 0;
    }

    for (i = 0; i < g_state.cfg.qd; i++) {
        if (g_state.map[i] == 0) {
            int op = select_op();
            int prio = select_prio();

            act = i;
            g_state.cmd_op[act] = op;
            g_state.cmd_prio[act] = prio;
            if (use_page_block_stripe()) {
                g_state.cmd_stripe_base[act] = g_state.global_page_stripe;
                g_state.global_page_stripe += g_state.d.pages_per_block;
                g_state.cmd_pages_done[act] = 0;
                g_state.cmd_pages_launched[act] = 0;
                g_state.cmd_write_cmd_sent[act] = 0;
                g_state.cmd_write_cmd_done[act] = 0;
                g_state.cmd_page_stripe[act] = 1;
                stripe_page_target(g_state.cmd_stripe_base[act], 0, &chan_id,
                                   &die_in_chan);
            } else {
                g_state.cmd_page_stripe[act] = 0;
                select_target(&chan_id, &die_in_chan);
            }
            g_state.cmd_target_chan[act] = chan_id;
            g_state.cmd_target_die[act] = die_in_chan;

            if (!cmd_pool_push(act)) {
                if (pool_rejects) {
                    (*pool_rejects)++;
                }
                return 0;
            }

            g_state.map[i] = 1;
            (*inflight_cmds)++;
            return 1;
        }
    }

    return 0;
}
