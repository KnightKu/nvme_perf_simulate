#include "cmd_sched.h"

#include "sched_internal.h"
#include "timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

perf_state_t g_state;

static void fanout_plane_pages(die_ctx_t *ctx, int act, int data_ready_state) {
    int pages = g_state.d.pages_per_block;
    int max_p = g_state.d.max_planes_per_die;
    int s;
    int launched = 0;

    for (s = 0; s < ctx->slot_count; s++) {
        plane_slot_t *ps = slot_at(ctx, s);
        if (ps->act == act && ps->state == DIE_READ_WAIT) {
            ps->act = -1;
            ps->state = DIE_IDLE;
        }
    }

    for (s = 0; s < ctx->slot_count && g_state.cmd_pages_assigned[act] < pages &&
                        launched < max_p;
         s++) {
        plane_slot_t *ps = slot_at(ctx, s);
        if (ps->state == DIE_IDLE) {
            ps->act = act;
            ps->state = data_ready_state;
            g_state.cmd_pages_assigned[act]++;
            launched++;
        }
    }
}

static int recycle_plane_page(die_ctx_t *ctx, plane_slot_t *ps, int act,
                              int data_ready_state) {
    if (g_state.cmd_pages_assigned[act] < g_state.d.pages_per_block) {
        ps->act = act;
        ps->state = data_ready_state;
        g_state.cmd_pages_assigned[act]++;
        return 1;
    }

    ps->act = -1;
    ps->state = DIE_IDLE;
    return 0;
}

static void chan_go_idle(int chan_id) {
    g_state.chan[chan_id].act = -1;
    g_state.chan[chan_id].state = CHAN_IDLE;
    g_state.chan[chan_id].op = OP_READ;
    g_state.chan[chan_id].die = -1;
    g_state.chan[chan_id].slot = -1;
    g_state.chan[chan_id].pages_left = 0;
}

static int complete_wait_ops(int chan_id, uint64_t cur_time, int *inflight_cmds,
                             uint64_t *total_cmd, uint64_t *write_cmd,
                             uint64_t *erase_cmd,
                             uint64_t *nand_program_pages) {
    int j;
    int completed = 0;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, j);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if (ps->state == DIE_WRITE_WAIT && cur_time >= ps->time) {
                int act = ps->act;

                if (nand_program_pages) {
                    (*nand_program_pages)++;
                }
                g_state.cmd_pages_left[act]--;
                if (g_state.cmd_pages_left[act] == 0) {
                    g_state.map[act] = 0;
                    (*inflight_cmds)--;
                    (*total_cmd)++;
                    (*write_cmd)++;
                    completed++;
                }
                recycle_plane_page(ctx, ps, act, DIE_WRITE_DATA_READY);
            } else if (ps->state == DIE_ERASE_WAIT && cur_time >= ps->time) {
                int act = ps->act;

                ps->act = -1;
                ps->state = DIE_IDLE;
                g_state.map[act] = 0;
                (*inflight_cmds)--;
                (*total_cmd)++;
                (*erase_cmd)++;
                completed++;
            }
        }
    }
    return completed;
}

static int try_schedule_cmd(int chan_id, uint64_t cur_time, int op) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        queue_t *q = &ctx->q[op];
        int slot = -1;
        int s;

        if (q->empty) {
            continue;
        }

        for (s = 0; s < ctx->slot_count; s++) {
            if (slot_at(ctx, s)->state == DIE_IDLE) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            continue;
        }

        {
            int act = queue_pop(q);

            slot_at(ctx, slot)->act = act;
            slot_at(ctx, slot)->state = DIE_CMD;

            g_state.chan[chan_id].state = CHAN_CMD;
            g_state.chan[chan_id].time = cur_time + g_state.d.cmd_time;
            g_state.chan[chan_id].act = act;
            g_state.chan[chan_id].op = op;
            g_state.chan[chan_id].die = die;
            g_state.chan[chan_id].slot = slot;
            g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
            return 1;
        }
    }
    return 0;
}

static int try_complete_read_wait(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, j);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if (ps->state == DIE_READ_WAIT && cur_time >= ps->time) {
                int act = ps->act;

                g_state.cmd_pages_left[act] = g_state.d.pages_per_block;
                g_state.cmd_pages_assigned[act] = 0;
                fanout_plane_pages(ctx, act, DIE_READ_DATA_READY);
                return 1;
            }
        }
    }
    return 0;
}

static int try_schedule_read_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if (ps->state == DIE_READ_DATA_READY) {
                g_state.chan[chan_id].state = CHAN_DATA;
                g_state.chan[chan_id].pages_left = 1;
                g_state.chan[chan_id].time =
                    cur_time + g_state.d.data_time_read_page;
                g_state.chan[chan_id].act = ps->act;
                g_state.chan[chan_id].op = OP_READ;
                g_state.chan[chan_id].die = die;
                g_state.chan[chan_id].slot = slot;
                ps->state = DIE_READ_DATA;
                g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
                return 1;
            }
        }
    }
    return 0;
}

static int try_schedule_write_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if (ps->state == DIE_WRITE_DATA_READY) {
                g_state.chan[chan_id].state = CHAN_DATA;
                g_state.chan[chan_id].pages_left = 0;
                g_state.chan[chan_id].time =
                    cur_time + g_state.d.data_time_write_page;
                g_state.chan[chan_id].act = ps->act;
                g_state.chan[chan_id].op = OP_WRITE;
                g_state.chan[chan_id].die = die;
                g_state.chan[chan_id].slot = slot;
                ps->state = DIE_WRITE_DATA;
                g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
                return 1;
            }
        }
    }
    return 0;
}

int perf_init(const perf_config_t *cfg) {
    int i;
    int j;

    if (perf_config_validate(cfg) != 0) {
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *cfg;
    if (perf_derived_init(cfg, &g_state.d) != 0) {
        return -1;
    }

    g_state.map = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_op = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_target_chan = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_target_die = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_left = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_assigned = (int *)calloc(cfg->qd, sizeof(int));
    g_state.die_ctx = (die_ctx_t *)calloc(
        cfg->chan_num * g_state.d.die_per_chan, sizeof(die_ctx_t));
    g_state.rr_die = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.chan = (chan_t *)calloc(cfg->chan_num, sizeof(chan_t));

    if (!g_state.map || !g_state.cmd_op || !g_state.cmd_target_chan ||
        !g_state.cmd_target_die || !g_state.cmd_pages_left ||
        !g_state.cmd_pages_assigned || !g_state.die_ctx || !g_state.rr_die ||
        !g_state.chan) {
        perf_cleanup();
        return -1;
    }

    srand((unsigned)time(NULL));

    for (i = 0; i < cfg->chan_num; i++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            int slot_idx;
            int op;
            die_ctx_t *ctx = die_ctx_at(i, j);

            ctx->slot_count = g_state.d.max_planes_per_die;
            ctx->slots = (plane_slot_t *)calloc(ctx->slot_count,
                                                sizeof(plane_slot_t));
            if (!ctx->slots) {
                perf_cleanup();
                return -1;
            }
            for (slot_idx = 0; slot_idx < ctx->slot_count; slot_idx++) {
                ctx->slots[slot_idx].state = DIE_IDLE;
                ctx->slots[slot_idx].act = -1;
            }
            for (op = 0; op < OP_MAX; op++) {
                queue_init(&ctx->q[op], cfg->iwl_slot);
                if (!ctx->q[op].list) {
                    perf_cleanup();
                    return -1;
                }
            }
        }
    }

    g_state.initialized = 1;
    return 0;
}

void perf_cleanup(void) {
    int i;
    int j;

    if (!g_state.initialized && !g_state.map) {
        return;
    }

    if (g_state.die_ctx) {
        for (i = 0; i < g_state.cfg.chan_num; i++) {
            for (j = 0; j < g_state.d.die_per_chan; j++) {
                int op;
                die_ctx_t *ctx = die_ctx_at(i, j);
                free(ctx->slots);
                for (op = 0; op < OP_MAX; op++) {
                    free(ctx->q[op].list);
                }
            }
        }
    }

    free(g_state.map);
    free(g_state.cmd_op);
    free(g_state.cmd_target_chan);
    free(g_state.cmd_target_die);
    free(g_state.cmd_pages_left);
    free(g_state.cmd_pages_assigned);
    free(g_state.die_ctx);
    free(g_state.rr_die);
    free(g_state.chan);
    memset(&g_state, 0, sizeof(g_state));
}

void perf_run(perf_stats_t *stats) {
    int i;
    uint64_t sim_time = 0;
    uint64_t total_cmd = 0;
    uint64_t read_cmd = 0;
    uint64_t write_cmd = 0;
    uint64_t erase_cmd = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    uint64_t nand_program_pages = 0;
    int tmp_cmd_cnt = 0;
    int inflight_cmds = 0;
    uint64_t start_time = 0;
    uint64_t end_time = 0;

    if (!g_state.initialized) {
        return;
    }

    while (1) {
        if (total_cmd >= g_state.cfg.element && inflight_cmds == 0) {
            break;
        }

        if (g_state.cfg.qd >= 512) {
            tmp_cmd_cnt = (int)(g_state.cfg.qd * 0.75);
        } else {
            tmp_cmd_cnt = (int)(g_state.cfg.qd * 0.8);
        }

        if (total_cmd > (uint64_t)tmp_cmd_cnt && start_time == 0) {
            start_time = sim_time;
        }

        if (total_cmd < g_state.cfg.element) {
            cmd_generate_try(tmp_cmd_cnt, &inflight_cmds);
        }

        {
            int progressed = 0;
            uint64_t next_event = UINT64_MAX;

            for (i = 0; i < g_state.cfg.chan_num; i++) {
                if (complete_wait_ops(i, sim_time, &inflight_cmds, &total_cmd,
                                      &write_cmd, &erase_cmd,
                                      &nand_program_pages) > 0) {
                    progressed = 1;
                }
            }

            for (i = 0; i < g_state.cfg.chan_num; i++) {
                switch (g_state.chan[i].state) {
                    case CHAN_IDLE: {
                        uint64_t cur_time = sim_time;

                        if (try_complete_read_wait(i, cur_time)) {
                            progressed = 1;
                            break;
                        }
                        if (try_schedule_read_data(i, cur_time)) {
                            progressed = 1;
                            break;
                        }
                        if (try_schedule_cmd(i, cur_time, OP_READ)) {
                            progressed = 1;
                            break;
                        }
                        if (try_schedule_write_data(i, cur_time)) {
                            progressed = 1;
                            break;
                        }
                        if (try_schedule_cmd(i, cur_time, OP_WRITE)) {
                            progressed = 1;
                            break;
                        }
                        if (try_schedule_cmd(i, cur_time, OP_ERASE)) {
                            progressed = 1;
                            break;
                        }
                        break;
                    }
                    case CHAN_CMD: {
                        uint64_t cur_time = sim_time;
                        if (cur_time >= g_state.chan[i].time) {
                            die_ctx_t *ctx =
                                die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps =
                                slot_at(ctx, g_state.chan[i].slot);

                            if (g_state.chan[i].op == OP_READ) {
                                ps->state = DIE_READ_WAIT;
                                ps->time = cur_time + g_state.d.tread;
                            } else if (g_state.chan[i].op == OP_WRITE) {
                                int act = g_state.chan[i].act;

                                ps->act = -1;
                                ps->state = DIE_IDLE;
                                g_state.cmd_pages_left[act] =
                                    g_state.d.pages_per_block;
                                g_state.cmd_pages_assigned[act] = 0;
                                fanout_plane_pages(ctx, act,
                                                   DIE_WRITE_DATA_READY);
                            } else {
                                ps->state = DIE_ERASE_WAIT;
                                ps->time = cur_time + g_state.d.terase;
                            }
                            chan_go_idle(i);
                            progressed = 1;
                        }
                        break;
                    }
                    case CHAN_DATA: {
                        uint64_t cur_time = sim_time;
                        if (cur_time >= g_state.chan[i].time) {
                            die_ctx_t *ctx =
                                die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps =
                                slot_at(ctx, g_state.chan[i].slot);
                            int act = g_state.chan[i].act;

                            if (g_state.chan[i].op == OP_READ) {
                                read_bytes +=
                                    (uint64_t)g_state.d.read_bytes_per_page;
                                g_state.cmd_pages_left[act]--;
                                if (g_state.cmd_pages_left[act] == 0) {
                                    g_state.map[act] = 0;
                                    inflight_cmds--;
                                    total_cmd++;
                                    read_cmd++;
                                }
                                recycle_plane_page(ctx, ps, act,
                                                   DIE_READ_DATA_READY);
                            } else if (g_state.chan[i].op == OP_WRITE) {
                                write_bytes +=
                                    (uint64_t)g_state.d.write_bytes_per_page;
                                ps->state = DIE_WRITE_WAIT;
                                ps->time = cur_time + g_state.d.tprog;
                            }
                            chan_go_idle(i);
                            progressed = 1;
                        }
                        break;
                    }
                    default:
                        fprintf(stderr, "invalid channel state\n");
                        exit(1);
                }
            }

            if (!progressed) {
                int ch;
                for (ch = 0; ch < g_state.cfg.chan_num; ch++) {
                    if (g_state.chan[ch].state != CHAN_IDLE &&
                        g_state.chan[ch].time > sim_time &&
                        g_state.chan[ch].time < next_event) {
                        next_event = g_state.chan[ch].time;
                    }
                }
                for (ch = 0; ch < g_state.cfg.chan_num; ch++) {
                    int d;
                    for (d = 0; d < g_state.d.die_per_chan; d++) {
                        int s;
                        die_ctx_t *ctx = die_ctx_at(ch, d);
                        for (s = 0; s < ctx->slot_count; s++) {
                            plane_slot_t *ps = slot_at(ctx, s);
                            if ((ps->state == DIE_READ_WAIT ||
                                 ps->state == DIE_WRITE_WAIT ||
                                 ps->state == DIE_ERASE_WAIT) &&
                                ps->time > sim_time &&
                                ps->time < next_event) {
                                next_event = ps->time;
                            }
                        }
                    }
                }
                if (next_event == UINT64_MAX) {
                    if (inflight_cmds == 0 &&
                        total_cmd >= g_state.cfg.element) {
                        goto perf_run_done;
                    }
                    break;
                }
                sim_time = next_event;
            }
        }
    }

perf_run_done:
    end_time = sim_time;
    if (stats) {
        stats->total_cmd = total_cmd;
        stats->read_cmd = read_cmd;
        stats->write_cmd = write_cmd;
        stats->erase_cmd = erase_cmd;
        stats->read_bytes = read_bytes;
        stats->write_bytes = write_bytes;
        stats->start_time = start_time;
        stats->end_time = end_time;
        stats->nand_program_pages = nand_program_pages;
    }
}
