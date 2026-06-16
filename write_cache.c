#include "write_cache.h"

#include "sched_internal.h"

void write_cache_init(void) {
}

void write_cache_cleanup(void) {
}

static int write_cache_enabled_for_act(int act) {
    if (!g_state.cfg.write_cache) {
        return 0;
    }
    if (g_state.cfg.block_size != 0 || g_state.cfg.use_codeword_buffers) {
        return 0;
    }
    if (g_state.cmd_op[act] != OP_WRITE) {
        return 0;
    }
    if (g_state.cmd_page_stripe[act]) {
        return 0;
    }
    return 1;
}

int write_cache_on_write_cmd(int act, const write_cache_ctx_t *ctx) {
    int chan = g_state.cmd_target_chan[act];
    int die = g_state.cmd_target_die[act];
    die_ctx_t *die_ctx = die_ctx_at(chan, die);

    if (!write_cache_enabled_for_act(act) || !ctx) {
        return 0;
    }

    die_ctx->wr_cache.fill_frags++;
    g_state.map[act] = 0;
    (*ctx->inflight_cmds)--;
    (*ctx->total_cmd)++;
    (*ctx->write_cmd)++;
    (*ctx->write_bytes) += (uint64_t)g_state.d.write_fragment_bytes;
    return 1;
}

int write_cache_try_schedule_flush(int chan_id, uint64_t cur_time) {
    int j;

    if (!g_state.cfg.write_cache || g_state.cfg.block_size != 0) {
        return 0;
    }

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        plane_slot_t *ps;

        if (ctx->wr_cache.fill_frags < g_state.d.frags_per_write_page) {
            continue;
        }

        for (slot = 0; slot < ctx->slot_count; slot++) {
            if (slot_at(ctx, slot)->state == DIE_IDLE) {
                break;
            }
        }
        if (slot >= ctx->slot_count) {
            continue;
        }

        ps = slot_at(ctx, slot);
        ps->act = 0xFFF;
        ps->page_idx = -1;
        ps->host_pages_left = 0;
        ps->cw_idx = 0;
        ps->coalesce_prog = 0;
        ps->cache_flush = 1;
        ps->state = DIE_WRITE_DATA;

        g_state.chan[chan_id].state = CHAN_DATA;
        g_state.chan[chan_id].time = cur_time + g_state.d.data_time_write_page;
        g_state.chan[chan_id].act = 0xFFF;
        g_state.chan[chan_id].op = OP_WRITE;
        g_state.chan[chan_id].die = die;
        g_state.chan[chan_id].slot = slot;
        g_state.chan[chan_id].pages_left = 0;
        g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
        return 1;
    }

    return 0;
}

int write_cache_has_pending(void) {
    int chan_id;
    int die;

    if (!g_state.cfg.write_cache || g_state.cfg.block_size != 0) {
        return 0;
    }

    for (chan_id = 0; chan_id < g_state.cfg.chan_num; chan_id++) {
        for (die = 0; die < g_state.d.die_per_chan; die++) {
            int slot;
            die_ctx_t *ctx = die_ctx_at(chan_id, die);

            for (slot = 0; slot < ctx->slot_count; slot++) {
                plane_slot_t *ps = slot_at(ctx, slot);

                if (ps->cache_flush) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int write_cache_chan_data_complete(int chan_id, uint64_t cur_time) {
    die_ctx_t *ctx = die_ctx_at(chan_id, g_state.chan[chan_id].die);
    plane_slot_t *ps = slot_at(ctx, g_state.chan[chan_id].slot);

    if (!ps->cache_flush || ps->state != DIE_WRITE_DATA) {
        return 0;
    }

    ctx->wr_cache.fill_frags -= g_state.d.frags_per_write_page;
    if (ctx->wr_cache.fill_frags < 0) {
        ctx->wr_cache.fill_frags = 0;
    }

    ps->state = DIE_WRITE_WAIT;
    ps->time = cur_time + g_state.d.tprog;
    return 1;
}

int write_cache_tprog_complete(int chan_id, int die, int slot, uint64_t cur_time,
                               uint64_t *nand_program_pages) {
    die_ctx_t *ctx = die_ctx_at(chan_id, die);
    plane_slot_t *ps = slot_at(ctx, slot);

    (void)cur_time;
    if (!ps->cache_flush) {
        return 0;
    }

    if (nand_program_pages) {
        (*nand_program_pages)++;
    }
    ps->cache_flush = 0;
    ps->act = 0xFFF;
    ps->state = DIE_IDLE;
    ps->page_idx = -1;
    return 1;
}
