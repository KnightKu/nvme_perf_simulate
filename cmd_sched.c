#include "cmd_sched.h"

#include "sched_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

perf_state_t g_state;

static inline int is_power_of_two(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static inline double channel_payload_mbps(uint64_t payload_bytes,
                                          uint64_t data_time) {
    if (data_time == 0) {
        return 0.0;
    }
    return (double)payload_bytes * (double)TIME_SCALE / (double)data_time *
           1000000.0 / 1048576.0;
}

void select_target(int *chan_id, int *die_in_chan) {
    if (g_state.cfg.stripe_mode == STRIPE_CHANNEL_MAJOR) {
        int ch = g_state.rr_chan % g_state.cfg.chan_num;
        int die = g_state.stripe_cursor[ch] % g_state.d.die_per_chan;

        g_state.rr_chan = (g_state.rr_chan + 1) % g_state.cfg.chan_num;
        g_state.stripe_cursor[ch] =
            (g_state.stripe_cursor[ch] + 1) % g_state.d.die_per_chan;
        *chan_id = ch;
        *die_in_chan = die;
        return;
    }

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

static inline int recycle_plane_page(die_ctx_t *ctx, plane_slot_t *ps,
                                     int act, int data_ready_state) {
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

static inline void chan_go_idle(int chan_id) {
    g_state.chan[chan_id].act = -1;
    g_state.chan[chan_id].state = CHAN_IDLE;
    g_state.chan[chan_id].op = OP_READ;
    g_state.chan[chan_id].die = -1;
    g_state.chan[chan_id].slot = -1;
    g_state.chan[chan_id].pages_left = 0;
}

static inline int complete_wait_ops(int chan_id, uint64_t cur_time,
                                    int *inflight_cmds,
                                    uint64_t *total_cmd,
                                    uint64_t *write_cmd,
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

static inline int try_schedule_cmd(int chan_id, uint64_t cur_time, int op) {
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

static inline int try_complete_read_wait(int chan_id, uint64_t cur_time) {
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

static inline int try_schedule_read_data(int chan_id, uint64_t cur_time) {
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

static inline int try_schedule_write_data(int chan_id, uint64_t cur_time) {
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

const char *perf_default_config_path(void) {
    return "nand.conf";
}

void perf_config_defaults(perf_config_t *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->cmd_overhead = 1.7;
    cfg->cmd_overhead_sca = 0.7;
    cfg->sca = 1;
    cfg->chan_speed = 3200;
    cfg->cmd_size = 4096;
    cfg->block_size = 0;
    cfg->ecc_parity_size = 600;
    cfg->page_size = 16384;
    cfg->page_parity_size = 1952;
    cfg->tr_fast = 40;
    cfg->tR = 40;
    cfg->tprog_eff = 420;
    cfg->nand_type = 3;
    cfg->tERASE = 5000;
    cfg->qd = 1024;
    cfg->chan_num = 16;
    cfg->die_num = 128;
    cfg->plane = 4;
    cfg->iwl_slot = 512;
    cfg->read_ratio = 100;
    cfg->write_ratio = 0;
    cfg->erase_ratio = 0;
    cfg->io_pattern = PERF_IO_PATTERN_RANDOM;
    cfg->stripe_mode = PERF_STRIPE_CHANNEL_MAJOR;
    cfg->element = 16384;
}

static int set_config_value(perf_config_t *cfg, const char *key,
                            const char *value) {
    char *end = NULL;

    if (strcmp(key, "cmd_overhead") == 0) {
        cfg->cmd_overhead = strtod(value, &end);
    } else if (strcmp(key, "cmd_overhead_sca") == 0) {
        cfg->cmd_overhead_sca = strtod(value, &end);
    } else if (strcmp(key, "sca") == 0) {
        cfg->sca = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "chan_speed") == 0) {
        cfg->chan_speed = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "cmd_size") == 0) {
        cfg->cmd_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "block_size") == 0) {
        cfg->block_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "ecc_parity_size") == 0 ||
               strcmp(key, "ecc_parity") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_size") == 0) {
        cfg->page_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_parity_size") == 0) {
        cfg->page_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr_fast") == 0) {
        cfg->tr_fast = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr") == 0) {
        cfg->tR = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tprog_eff") == 0 || strcmp(key, "tprog") == 0) {
        cfg->tprog_eff = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "nand_type") == 0) {
        cfg->nand_type = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "terase") == 0) {
        cfg->tERASE = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "qd") == 0) {
        cfg->qd = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "chan_num") == 0) {
        cfg->chan_num = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "die_num") == 0) {
        cfg->die_num = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "plane") == 0) {
        cfg->plane = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "iwl_slot") == 0) {
        cfg->iwl_slot = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "read_ratio") == 0) {
        cfg->read_ratio = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "write_ratio") == 0) {
        cfg->write_ratio = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "erase_ratio") == 0) {
        cfg->erase_ratio = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "io_pattern") == 0) {
        if (strcmp(value, "random") == 0) {
            cfg->io_pattern = PERF_IO_PATTERN_RANDOM;
            end = (char *)(value + strlen(value));
        } else if (strcmp(value, "sequential") == 0 ||
                   strcmp(value, "seq") == 0) {
            cfg->io_pattern = PERF_IO_PATTERN_SEQUENTIAL;
            end = (char *)(value + strlen(value));
        } else {
            return -1;
        }
    } else if (strcmp(key, "stripe_mode") == 0) {
        if (strcmp(value, "channel_major") == 0 ||
            strcmp(value, "channel") == 0) {
            cfg->stripe_mode = PERF_STRIPE_CHANNEL_MAJOR;
            end = (char *)(value + strlen(value));
        } else if (strcmp(value, "global_die") == 0 ||
                   strcmp(value, "die") == 0) {
            cfg->stripe_mode = PERF_STRIPE_GLOBAL_DIE;
            end = (char *)(value + strlen(value));
        } else {
            return -1;
        }
    } else if (strcmp(key, "element") == 0) {
        cfg->element = (uint64_t)strtoull(value, &end, 10);
    } else {
        return 0;
    }

    if (end == value) {
        return -1;
    }
    return 1;
}

static char *trim_space(char *str) {
    char *end;

    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return str;
    }
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

static void to_lower_str(char *str) {
    for (; *str; str++) {
        *str = (char)tolower((unsigned char)*str);
    }
}

static void strip_inline_comment(char *value) {
    char *p;

    if (!value) {
        return;
    }
    p = strchr(value, '#');
    if (p) {
        *p = '\0';
    }
    p = strstr(value, "//");
    if (p) {
        *p = '\0';
    }
    trim_space(value);
}

int perf_load_config(const char *path, perf_config_t *cfg) {
    FILE *fp;
    char line[256];

    if (!path || !cfg) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char *key;
        char *value;
        int rc;

        key = trim_space(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        value = trim_space(eq + 1);
        key = trim_space(key);
        to_lower_str(key);
        strip_inline_comment(value);

        rc = set_config_value(cfg, key, value);
        if (rc < 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

int perf_init(const perf_config_t *cfg) {
    int i;
    int j;

    if (!cfg) {
        return -1;
    }

    if (cfg->chan_num <= 0 || cfg->die_num <= 0 || cfg->qd <= 0 ||
        cfg->iwl_slot <= 0 || cfg->chan_speed <= 0 || cfg->cmd_size <= 0 ||
        cfg->page_size <= 0 || cfg->plane <= 0) {
        return -1;
    }
    if (cfg->die_num % cfg->chan_num != 0) {
        return -1;
    }
    if (cfg->read_ratio + cfg->write_ratio + cfg->erase_ratio <= 0) {
        return -1;
    }
    if (cfg->nand_type != 1 && cfg->nand_type != 3 && cfg->nand_type != 4) {
        return -1;
    }
    if (cfg->tprog_eff <= 0) {
        return -1;
    }
    if (cfg->io_pattern != PERF_IO_PATTERN_RANDOM &&
        cfg->io_pattern != PERF_IO_PATTERN_SEQUENTIAL) {
        return -1;
    }
    if (cfg->stripe_mode != PERF_STRIPE_CHANNEL_MAJOR &&
        cfg->stripe_mode != PERF_STRIPE_GLOBAL_DIE) {
        return -1;
    }
    if (cfg->block_size < 0) {
        return -1;
    }
    if (cfg->block_size > 0 && cfg->block_size % cfg->page_size != 0) {
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *cfg;
    g_state.d.die_per_chan = cfg->die_num / cfg->chan_num;
    {
        uint64_t total_planes =
            (uint64_t)cfg->die_num * (uint64_t)cfg->plane;
        if (total_planes > (uint64_t)cfg->iwl_slot) {
            int per_die = cfg->iwl_slot / cfg->die_num;
            if (cfg->iwl_slot % cfg->die_num != 0 ||
                !is_power_of_two(per_die) || per_die <= 0) {
                return -1;
            }
            g_state.d.max_planes_per_die =
                (per_die > cfg->plane) ? cfg->plane : per_die;
        } else {
            g_state.d.max_planes_per_die = cfg->plane;
        }
    }

    if (cfg->block_size > 0) {
        g_state.d.pages_per_block = cfg->block_size / cfg->page_size;
        g_state.d.read_bytes_per_page = cfg->page_size;
        g_state.d.write_bytes_per_page = cfg->page_size;
    } else {
        g_state.d.pages_per_block = 1;
        g_state.d.read_bytes_per_page = cfg->cmd_size;
        g_state.d.write_bytes_per_page = cfg->page_size;
    }
    g_state.d.read_bytes_per_cmd =
        cfg->block_size > 0 ? cfg->block_size : cfg->cmd_size;
    g_state.d.write_bytes_per_cmd =
        cfg->block_size > 0 ? cfg->block_size : cfg->page_size;

    if (cfg->sca) {
        g_state.d.cmd_time = (uint64_t)(cfg->cmd_overhead_sca * TIME_SCALE);
    } else {
        g_state.d.cmd_time = (uint64_t)(cfg->cmd_overhead * TIME_SCALE);
    }
    if (cfg->cmd_size == 4096) {
        g_state.d.tread = (uint64_t)cfg->tr_fast * TIME_SCALE;
    } else {
        g_state.d.tread = (uint64_t)cfg->tR * TIME_SCALE;
    }
    g_state.d.tprog =
        (uint64_t)((uint64_t)cfg->tprog_eff * (uint64_t)cfg->nand_type) *
        TIME_SCALE;
    g_state.d.terase = (uint64_t)cfg->tERASE * TIME_SCALE;
    {
        uint64_t read_wire =
            (uint64_t)g_state.d.read_bytes_per_page +
            (uint64_t)cfg->ecc_parity_size;
        uint64_t write_wire =
            (uint64_t)g_state.d.write_bytes_per_page +
            (uint64_t)cfg->page_parity_size;

        g_state.d.data_time_read_page =
            read_wire * TIME_SCALE / (uint64_t)cfg->chan_speed;
        g_state.d.data_time_write_page =
            write_wire * TIME_SCALE / (uint64_t)cfg->chan_speed;
        g_state.d.read_ceiling_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps(read_wire, g_state.d.data_time_read_page);
        g_state.d.write_ceiling_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps(write_wire, g_state.d.data_time_write_page);
        g_state.d.read_ceiling_host_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps((uint64_t)g_state.d.read_bytes_per_page,
                                 g_state.d.data_time_read_page);
        g_state.d.write_ceiling_host_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps((uint64_t)g_state.d.write_bytes_per_page,
                                 g_state.d.data_time_write_page);
    }
    {
        int xor_ratio = (cfg->die_num > 64) ? 64 : cfg->die_num;
        g_state.d.xor_factor = ((double)xor_ratio - 1) / (double)xor_ratio;
    }
    g_state.d.read_ceiling_xor_mbps =
        g_state.d.read_ceiling_mbps * g_state.d.xor_factor;
    g_state.d.write_ceiling_xor_mbps =
        g_state.d.write_ceiling_mbps * g_state.d.xor_factor;

    g_state.map = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_op = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_target_chan = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_target_die = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_left = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_assigned = (int *)calloc(cfg->qd, sizeof(int));
    g_state.die_ctx = (die_ctx_t *)calloc(
        cfg->chan_num * g_state.d.die_per_chan, sizeof(die_ctx_t));
    g_state.rr_die = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.stripe_cursor = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.chan = (chan_t *)calloc(cfg->chan_num, sizeof(chan_t));

    if (!g_state.map || !g_state.cmd_op || !g_state.cmd_target_chan ||
        !g_state.cmd_target_die || !g_state.cmd_pages_left ||
        !g_state.cmd_pages_assigned || !g_state.die_ctx || !g_state.rr_die ||
        !g_state.stripe_cursor || !g_state.chan) {
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
    free(g_state.stripe_cursor);
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
