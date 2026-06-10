#include "cw_path.h"

#include "chan_cw_buf.h"
#include "host_port.h"
#include "output_buffer.h"
#include "read_bus.h"
#include "sched_internal.h"

#include <stdint.h>

int use_codeword_buffers(void) {
    return g_state.cfg.use_codeword_buffers != 0;
}

int cw_cws_per_read_page(void) {
    int host = g_state.cfg.codeword_host_bytes;

    if (host <= 0) {
        return 1;
    }
    return (g_state.d.read_bytes_per_page + host - 1) / host;
}

int cw_cws_per_write_page(void) {
    int host = g_state.cfg.codeword_host_bytes;

    if (host <= 0) {
        return 1;
    }
    return (g_state.d.write_bytes_per_page + host - 1) / host;
}

int cw_total_read_cws(int act) {
    if (g_state.cmd_page_stripe[act] || g_state.d.pages_per_block > 1) {
        return g_state.d.pages_per_block * cw_cws_per_read_page();
    }
    return cw_cws_per_read_page();
}

static int cw_write_channel(int act, int page_idx, int *die) {
    int ch;

    if (g_state.cmd_page_stripe[act] && page_idx >= 0) {
        stripe_page_target(g_state.cmd_stripe_base[act], page_idx, &ch, die);
        return ch;
    }
    *die = g_state.cmd_target_die[act];
    return g_state.cmd_target_chan[act];
}

void cw_host_read_chunk_done(int act, const cw_run_ctx_t *ctx) {
    if (g_state.cmd_cw_read_done[act] < cw_total_read_cws(act)) {
        return;
    }
    if (g_state.cmd_page_stripe[act]) {
        complete_host_page_stripe(act, OP_READ, ctx->inflight_cmds,
                                  ctx->total_cmd, ctx->read_cmd, NULL);
        return;
    }
    if (!g_state.map[act]) {
        return;
    }
    g_state.map[act] = 0;
    (*ctx->inflight_cmds)--;
    (*ctx->total_cmd)++;
    (*ctx->read_cmd)++;
}

int cw_prepare_write_page(int act, int page_idx, uint64_t *write_bytes) {
    int die;
    int ch = cw_write_channel(act, page_idx, &die);

    return host_port_try_feed_write(ch, act, page_idx, 0, write_bytes);
}

int cw_try_schedule_read_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);

        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            int cw_slot;
            int cws = cw_cws_per_read_page();
            int ready = 0;

            if (ps->state == DIE_READ_WAIT && cur_time >= ps->time) {
                ps->cw_idx = 0;
                ready = 1;
            } else if (ps->state == DIE_READ_DATA && ps->cw_idx < cws) {
                ready = 1;
            }
            if (!ready) {
                continue;
            }

            cw_slot = chan_cw_read_acquire(chan_id);
            if (cw_slot < 0) {
                continue;
            }

            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time =
                cur_time + g_state.d.data_time_read_cw;
            g_state.chan[chan_id].act = ps->act;
            g_state.chan[chan_id].op = OP_READ;
            g_state.chan[chan_id].die = die;
            g_state.chan[chan_id].slot = slot;
            g_state.chan[chan_id].cw_idx = ps->cw_idx;
            g_state.chan[chan_id].cw_buf_slot = cw_slot;
            g_state.chan[chan_id].pages_left =
                g_state.cmd_page_stripe[ps->act]
                    ? 0
                    : g_state.d.pages_per_block;
            ps->state = DIE_READ_DATA;
            g_state.rr_die[chan_id] =
                (die + 1) % g_state.d.die_per_chan;
            return 1;
        }
    }

    return 0;
}

int cw_try_schedule_write_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);

        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            int prog_slot;

            if (ps->state != DIE_WRITE_DATA_READY) {
                continue;
            }

            (void)host_port_try_feed_write(chan_id, ps->act, ps->page_idx,
                                           ps->cw_idx, NULL);
            prog_slot = chan_cw_prog_pick_ready(chan_id, ps->act, ps->page_idx,
                                                ps->cw_idx);
            if (prog_slot < 0) {
                continue;
            }

            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time =
                cur_time + g_state.d.data_time_write_cw;
            g_state.chan[chan_id].act = ps->act;
            g_state.chan[chan_id].op = OP_WRITE;
            g_state.chan[chan_id].die = die;
            g_state.chan[chan_id].slot = slot;
            g_state.chan[chan_id].cw_idx = ps->cw_idx;
            g_state.chan[chan_id].cw_buf_slot = prog_slot;
            g_state.chan[chan_id].pages_left = 0;
            ps->state = DIE_WRITE_DATA;
            g_state.rr_die[chan_id] =
                (die + 1) % g_state.d.die_per_chan;
            return 1;
        }
    }

    return 0;
}

int cw_chan_data_read_complete(int chan_id, uint64_t cur_time,
                               int *inflight_cmds, uint64_t *total_cmd,
                               uint64_t *read_cmd) {
    die_ctx_t *ctx = die_ctx_at(chan_id, g_state.chan[chan_id].die);
    plane_slot_t *ps = slot_at(ctx, g_state.chan[chan_id].slot);
    int act = g_state.chan[chan_id].act;
    int cw_slot = g_state.chan[chan_id].cw_buf_slot;
    int cws = cw_cws_per_read_page();

    (void)inflight_cmds;
    (void)total_cmd;
    (void)read_cmd;

    chan_cw_read_mark_ready(chan_id, cw_slot, act, ps->page_idx,
                            g_state.chan[chan_id].cw_idx);
    g_state.chan_read_wire_bytes +=
        (uint64_t)g_state.d.codeword_wire_bytes;

    ps->cw_idx++;
    if (ps->cw_idx < cws) {
        g_state.chan[chan_id].act = 0xFFF;
        g_state.chan[chan_id].state = CHAN_IDLE;
        g_state.chan[chan_id].op = OP_READ;
        g_state.chan[chan_id].die = -1;
        g_state.chan[chan_id].slot = -1;
        g_state.chan[chan_id].pages_left = 0;
        return 1;
    }

    ps->cw_idx = 0;
    if (!g_state.cmd_page_stripe[act] && g_state.chan[chan_id].pages_left > 1) {
        g_state.chan[chan_id].pages_left--;
        ps->state = DIE_READ_WAIT;
        ps->time = cur_time;
        g_state.chan[chan_id].act = 0xFFF;
        g_state.chan[chan_id].state = CHAN_IDLE;
        g_state.chan[chan_id].op = OP_READ;
        g_state.chan[chan_id].die = -1;
        g_state.chan[chan_id].slot = -1;
        return 1;
    }

    if (g_state.cmd_page_stripe[act]) {
        g_state.cmd_pages_done[act]++;
        ps->act = 0xFFF;
        ps->state = DIE_IDLE;
        ps->page_idx = -1;
        if (g_state.cmd_pages_done[act] < g_state.d.pages_per_block) {
            g_state.chan[chan_id].act = 0xFFF;
            g_state.chan[chan_id].state = CHAN_IDLE;
            g_state.chan[chan_id].op = OP_READ;
            g_state.chan[chan_id].die = -1;
            g_state.chan[chan_id].slot = -1;
            g_state.chan[chan_id].pages_left = 0;
            return 1;
        }
    } else {
        ps->act = 0xFFF;
        ps->state = DIE_IDLE;
        ps->page_idx = -1;
    }

    if (ctx->suspended_op != OP_MAX) {
        plane_slot_t *sps = slot_at(ctx, ctx->suspended_slot);

        sps->act = ctx->suspended_act;
        sps->time = cur_time + ctx->suspended_time;
        sps->state = (ctx->suspended_op == OP_WRITE) ? DIE_WRITE_WAIT
                                                       : DIE_ERASE_WAIT;
        ctx->suspended_act = 0xFFF;
        ctx->suspended_op = OP_MAX;
        ctx->suspended_time = 0;
        ctx->suspended_slot = -1;
    }

    g_state.chan[chan_id].act = 0xFFF;
    g_state.chan[chan_id].state = CHAN_IDLE;
    g_state.chan[chan_id].op = OP_READ;
    g_state.chan[chan_id].die = -1;
    g_state.chan[chan_id].slot = -1;
    g_state.chan[chan_id].pages_left = 0;
    return 1;
}

int cw_chan_data_write_complete(int chan_id, uint64_t cur_time,
                                uint64_t *write_bytes) {
    die_ctx_t *ctx = die_ctx_at(chan_id, g_state.chan[chan_id].die);
    plane_slot_t *ps = slot_at(ctx, g_state.chan[chan_id].slot);
    int prog_slot = g_state.chan[chan_id].cw_buf_slot;
    int cws = cw_cws_per_write_page();
    int ch;
    int die;

    (void)ctx;
    chan_cw_prog_release(chan_id, prog_slot);
    g_state.chan_write_wire_bytes +=
        (uint64_t)g_state.d.codeword_write_wire_bytes;

    ps->cw_idx++;
    if (ps->cw_idx < cws) {
        ps->state = DIE_WRITE_DATA_READY;
        ch = cw_write_channel(ps->act, ps->page_idx, &die);
        (void)host_port_try_feed_write(ch, ps->act, ps->page_idx, ps->cw_idx,
                                       write_bytes);
        g_state.chan[chan_id].act = 0xFFF;
        g_state.chan[chan_id].state = CHAN_IDLE;
        g_state.chan[chan_id].op = OP_READ;
        g_state.chan[chan_id].die = -1;
        g_state.chan[chan_id].slot = -1;
        g_state.chan[chan_id].pages_left = 0;
        return 1;
    }

    ps->cw_idx = 0;
    ps->state = DIE_WRITE_WAIT;
    ps->time = cur_time + g_state.d.tprog;
    g_state.chan[chan_id].act = 0xFFF;
    g_state.chan[chan_id].state = CHAN_IDLE;
    g_state.chan[chan_id].op = OP_READ;
    g_state.chan[chan_id].die = -1;
    g_state.chan[chan_id].slot = -1;
    g_state.chan[chan_id].pages_left = 0;
    return 1;
}

int cw_post_step(uint64_t sim_time, uint64_t *read_bytes, uint64_t *write_bytes,
                 const cw_run_ctx_t *ctx) {
    int progressed = 0;

    if (read_bus_process(sim_time, &g_state.read_bus_bytes) > 0) {
        progressed = 1;
    }
    if (output_buffer_drain(read_bytes, ctx) > 0) {
        progressed = 1;
    }
    if (host_port_process(sim_time, write_bytes) > 0) {
        progressed = 1;
    }
    return progressed;
}

uint64_t cw_next_event(uint64_t sim_time) {
    return read_bus_next_event(sim_time);
}

int cw_path_init(void) {
    if (!use_codeword_buffers()) {
        return 0;
    }
    if (chan_cw_buf_init(g_state.cfg.chan_num) != 0) {
        return -1;
    }
    read_bus_init();
    if (output_buffer_init(g_state.cfg.output_buffer_bytes,
                           g_state.cfg.host_read_chunk_bytes,
                           g_state.cfg.qd) != 0) {
        cw_path_cleanup();
        return -1;
    }
    if (host_port_init(g_state.cfg.qd) != 0) {
        cw_path_cleanup();
        return -1;
    }
    return 0;
}

void cw_path_cleanup(void) {
    host_port_cleanup();
    output_buffer_cleanup();
    read_bus_cleanup();
    chan_cw_buf_cleanup();
}
