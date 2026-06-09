#include "cmd_sched.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

enum CHAN_STATE {
    CHAN_IDLE,
    CHAN_CMD,
    CHAN_DATA,
};

enum OP_TYPE {
    OP_READ,
    OP_WRITE,
    OP_ERASE,
    OP_MAX,
};

enum CMD_PRIO {
    PRIO_HIGH,
    PRIO_NORMAL,
    PRIO_LOW,
    PRIO_MAX,
};

enum IO_PATTERN {
    IO_PATTERN_RANDOM = PERF_IO_PATTERN_RANDOM,
    IO_PATTERN_SEQUENTIAL = PERF_IO_PATTERN_SEQUENTIAL,
};

enum STRIPE_MODE {
    STRIPE_CHANNEL_MAJOR = PERF_STRIPE_CHANNEL_MAJOR,
    STRIPE_GLOBAL_DIE = PERF_STRIPE_GLOBAL_DIE,
    STRIPE_PAGE_ACROSS_CHAN = PERF_STRIPE_PAGE_ACROSS_CHAN,
};

enum WORKLOAD {
    WORKLOAD_LEGACY = PERF_WORKLOAD_LEGACY,
    WORKLOAD_FULLDEV_SEQ_READ = PERF_WORKLOAD_FULLDEV_SEQ_READ,
    WORKLOAD_FULLDEV_SEQ_WRITE = PERF_WORKLOAD_FULLDEV_SEQ_WRITE,
};

// Suspend limits for read preemption.
#define MAX_SUSPEND_WRITE 8
#define MAX_SUSPEND_ERASE 15

enum DIE_STATE {
    DIE_IDLE,
    DIE_CMD,
    DIE_READ_WAIT,
    DIE_READ_DATA,
    DIE_WRITE_DATA_READY,
    DIE_WRITE_DATA,
    DIE_WRITE_WAIT,
    DIE_ERASE_WAIT,
};

struct timeval tv;

static inline uint64_t get_time_us() {
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000 + tv.tv_usec);
}

typedef struct queue_s {
    int head;
    int tail;
    int empty;
    int size;
    int *list;
} queue_t;

typedef struct plane_slot_s {
    // Per-plane slot within a die to enforce max parallelism.
    int state;
    int act;
    uint64_t time;
    int host_pages_left; /* legacy write: pages left after current tPROG */
    int page_idx;        /* page index within host block (-1 if unused) */
} plane_slot_t;

typedef struct die_ctx_s {
    queue_t q[PRIO_MAX][OP_MAX];
    plane_slot_t *slots;
    int slot_count;
    // Suspended write/erase state for read preemption.
    int suspended_slot;
    int suspended_act;
    int suspended_op;
    uint64_t suspended_time;
    int suspend_write_cnt;
    int suspend_erase_cnt;
} die_ctx_t;

typedef struct chan_s {
    int state;
    int act;
    int op;
    int die;
    int slot;
    uint64_t time;
    int pages_left; /* pages remaining in current host block transfer */
} chan_t;

typedef struct perf_derived {
    uint64_t cmd_time;
    uint64_t tread;
    uint64_t tprog;
    uint64_t terase;
    uint64_t data_time_read_page;
    uint64_t data_time_write_page;
    int pages_per_block;
    int page_unit;
    int read_xfer_size;
    int write_xfer_size;
    int read_bytes_per_page;
    int write_bytes_per_page;
    int read_bytes_per_cmd;
    int write_bytes_per_cmd;
    double read_ceiling_mbps;
    double write_ceiling_mbps;
    double read_ceiling_host_mbps;
    double write_ceiling_host_mbps;
    double read_ceiling_xor_mbps;
    double write_ceiling_xor_mbps;
    double xor_factor;
    int die_per_chan;
    int max_planes_per_die;
} perf_derived_t;

typedef struct perf_state {
    perf_config_t cfg;
    perf_derived_t d;
    int *map;
    int *cmd_op;
    int *cmd_prio;
    die_ctx_t *die_ctx;
    int *rr_die;
    int *stripe_cursor;
    int *cmd_stripe_base;
    int *cmd_pages_done;
    int *cmd_pages_launched;
    int *cmd_page_stripe;
    int global_page_stripe;
    int rr_chan;
    chan_t *chan;
    int next_die;
    int initialized;
} perf_state_t;

static perf_state_t g_state;

static inline die_ctx_t *die_ctx_at(int chan_id, int die) {
    return &g_state.die_ctx[chan_id * g_state.d.die_per_chan + die];
}

static inline plane_slot_t *slot_at(die_ctx_t *ctx, int slot) {
    return &ctx->slots[slot];
}

static inline void queue_init(queue_t *q, int size) {
    q->head = 0;
    q->tail = 0;
    q->empty = 1;
    q->size = size;
    q->list = (int *)calloc(size, sizeof(int));
}

static inline void queue_push(queue_t *q, int act) {
    q->list[q->head] = act;
    q->head++;
    if (q->head == q->size) {
        q->head = 0;
    }
    if (q->head != q->tail) {
        q->empty = 0;
    }
}

static inline int queue_pop(queue_t *q) {
    int act = q->list[q->tail];
    q->tail++;
    if (q->tail == q->size) {
        q->tail = 0;
    }
    if (q->head == q->tail) {
        q->empty = 1;
    }
    return act;
}

static inline int is_power_of_two(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static inline int select_op() {
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

static inline int select_prio() {
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

static inline int effective_stripe_mode(void) {
    if (g_state.cfg.workload == WORKLOAD_FULLDEV_SEQ_READ ||
        g_state.cfg.workload == WORKLOAD_FULLDEV_SEQ_WRITE) {
        return STRIPE_PAGE_ACROSS_CHAN;
    }
    return g_state.cfg.stripe_mode;
}

static inline int use_page_across_chan_stripe(void) {
    return effective_stripe_mode() == STRIPE_PAGE_ACROSS_CHAN &&
           g_state.cfg.block_size > 0 &&
           g_state.d.pages_per_block > 1;
}

static inline void stripe_page_target(int stripe_idx, int *chan_id,
                                      int *die_in_chan) {
    *chan_id = stripe_idx % g_state.cfg.chan_num;
    *die_in_chan =
        (stripe_idx / g_state.cfg.chan_num) % g_state.d.die_per_chan;
}

static inline void complete_host_page_stripe(int act, int op,
                                             int *inflight_cmds,
                                             uint64_t *total_cmd,
                                             uint64_t *read_cmd,
                                             uint64_t *write_cmd) {
    g_state.map[act] = 0;
    (*inflight_cmds)--;
    (*total_cmd)++;
    if (op == OP_READ) {
        (*read_cmd)++;
    } else if (op == OP_WRITE) {
        (*write_cmd)++;
    }
}

static int try_launch_striped_page(int act, int op, int page_idx,
                                   uint64_t cur_time) {
    int base = g_state.cmd_stripe_base[act];
    int ch;
    int die;
    int s;
    int slot = -1;
    die_ctx_t *ctx;
    plane_slot_t *ps;

    stripe_page_target(base + page_idx, &ch, &die);
    if (g_state.chan[ch].state != CHAN_IDLE) {
        return 0;
    }

    ctx = die_ctx_at(ch, die);
    for (s = 0; s < ctx->slot_count; s++) {
        if (slot_at(ctx, s)->state == DIE_IDLE) {
            slot = s;
            break;
        }
    }
    if (slot < 0) {
        return 0;
    }

    ps = slot_at(ctx, slot);
    ps->act = act;
    ps->page_idx = page_idx;
    ps->host_pages_left = 0;
    ps->state = DIE_CMD;

    g_state.chan[ch].state = CHAN_CMD;
    g_state.chan[ch].time = cur_time + g_state.d.cmd_time;
    g_state.chan[ch].act = act;
    g_state.chan[ch].op = op;
    g_state.chan[ch].die = die;
    g_state.chan[ch].slot = slot;
    g_state.chan[ch].pages_left = 0;
    return 1;
}

static int try_launch_striped_pages(int act, int op, uint64_t cur_time) {
    int started = 0;

    while (g_state.cmd_pages_launched[act] < g_state.d.pages_per_block) {
        int p = g_state.cmd_pages_launched[act];

        if (!try_launch_striped_page(act, op, p, cur_time)) {
            break;
        }
        g_state.cmd_pages_launched[act]++;
        started = 1;
    }
    return started;
}

static int try_launch_pending_page_stripes(uint64_t cur_time) {
    int act;
    int started = 0;

    for (act = 0; act < g_state.cfg.qd; act++) {
        if (!g_state.map[act] || !g_state.cmd_page_stripe[act]) {
            continue;
        }
        if (g_state.cmd_pages_launched[act] >= g_state.d.pages_per_block) {
            continue;
        }
        if (try_launch_striped_pages(act, g_state.cmd_op[act], cur_time)) {
            started = 1;
        }
    }
    return started;
}

static inline void select_target(int *chan_id, int *die_in_chan) {
    int stripe = effective_stripe_mode();

    if (stripe == STRIPE_CHANNEL_MAJOR) {
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

static inline double channel_payload_mbps(uint64_t payload_bytes,
                                          uint64_t data_time) {
    if (data_time == 0) {
        return 0.0;
    }
    return (double)payload_bytes * (double)TIME_SCALE / (double)data_time *
           1000000.0 / 1048576.0;
}

static inline void enqueue_cmd(int chan_id, int die_in_chan, int op, int prio,
                               int act) {
    die_ctx_t *ctx = die_ctx_at(chan_id, die_in_chan);
    queue_push(&ctx->q[prio][op], act);
}

static inline int complete_wait_ops(int chan_id, uint64_t cur_time,
                                    int *inflight_cmds,
                                    uint64_t *total_cmd,
                                    uint64_t *write_cmd,
                                    uint64_t *erase_cmd) {
    int j;
    int completed = 0;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, j);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if ((ps->state == DIE_WRITE_WAIT ||
                 ps->state == DIE_ERASE_WAIT) &&
                ps->act != 0xFFF && cur_time >= ps->time) {
                int act = ps->act;

                if (ps->state == DIE_WRITE_WAIT &&
                    g_state.cmd_op[act] == OP_WRITE &&
                    !g_state.cmd_page_stripe[act] &&
                    ps->host_pages_left > 1) {
                    ps->host_pages_left--;
                    ps->state = DIE_WRITE_DATA_READY;
                    completed++;
                    continue;
                }

                if (ps->state == DIE_WRITE_WAIT &&
                    g_state.cmd_op[act] == OP_WRITE &&
                    g_state.cmd_page_stripe[act]) {
                    g_state.cmd_pages_done[act]++;
                    ps->act = 0xFFF;
                    ps->state = DIE_IDLE;
                    ps->page_idx = -1;
                    ps->host_pages_left = 0;
                    if (g_state.cmd_pages_done[act] <
                        g_state.d.pages_per_block) {
                        completed++;
                        continue;
                    }
                    complete_host_page_stripe(act, OP_WRITE, inflight_cmds,
                                              total_cmd, NULL, write_cmd);
                    completed++;
                    continue;
                }

                ps->act = 0xFFF;
                ps->state = DIE_IDLE;
                ps->page_idx = -1;
                ps->host_pages_left = 0;
                g_state.map[act] = 0;
                (*inflight_cmds)--;
                (*total_cmd)++;
                completed++;
                if (g_state.cmd_op[act] == OP_WRITE) {
                    (*write_cmd)++;
                } else if (g_state.cmd_op[act] == OP_ERASE) {
                    (*erase_cmd)++;
                }
            }
        }
    }
    return completed;
}

static inline int try_schedule_cmd(int chan_id, uint64_t cur_time, int op) {
    int prio;
    int j;

    // Scan dies by round-robin; within each die pick highest cmd priority.
    // Pick highest priority cmd first; allow read to preempt write/erase waits.
    for (prio = PRIO_HIGH; prio < PRIO_MAX; prio++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
            int act;
            die_ctx_t *ctx;
            queue_t *q;
            int slot = -1;
            int s;

            ctx = die_ctx_at(chan_id, die);
            q = &ctx->q[prio][op];
            if (q->empty) {
                continue;
            }

            for (s = 0; s < ctx->slot_count; s++) {
                if (slot_at(ctx, s)->state == DIE_IDLE) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0 && op == OP_READ && ctx->suspended_op == OP_MAX) {
                // Read can suspend a write/erase wait slot to make room.
                for (s = 0; s < ctx->slot_count; s++) {
                    plane_slot_t *ps = slot_at(ctx, s);
                    if ((ps->state == DIE_WRITE_WAIT ||
                         ps->state == DIE_ERASE_WAIT) &&
                        ps->time > cur_time) {
                        if (ps->state == DIE_WRITE_WAIT &&
                            ctx->suspend_write_cnt >= MAX_SUSPEND_WRITE) {
                            continue;
                        }
                        if (ps->state == DIE_ERASE_WAIT &&
                            ctx->suspend_erase_cnt >= MAX_SUSPEND_ERASE) {
                            continue;
                        }
                        ctx->suspended_slot = s;
                        ctx->suspended_act = ps->act;
                        ctx->suspended_op =
                            (ps->state == DIE_WRITE_WAIT) ? OP_WRITE
                                                          : OP_ERASE;
                        ctx->suspended_time = ps->time - cur_time;
                        if (ps->state == DIE_WRITE_WAIT) {
                            ctx->suspend_write_cnt++;
                        } else {
                            ctx->suspend_erase_cnt++;
                        }
                        ps->act = 0xFFF;
                        ps->state = DIE_IDLE;
                        slot = s;
                        break;
                    }
                }
            }
            if (slot < 0) {
                continue;
            }

            act = queue_pop(q);
            if (g_state.cmd_page_stripe[act]) {
                g_state.rr_die[chan_id] =
                    (die + 1) % g_state.d.die_per_chan;
                return try_launch_striped_pages(act, op, cur_time);
            }

            slot_at(ctx, slot)->act = act;
            slot_at(ctx, slot)->page_idx = -1;
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

static inline int try_schedule_read_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int slot;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        for (slot = 0; slot < ctx->slot_count; slot++) {
            plane_slot_t *ps = slot_at(ctx, slot);
            if (ps->state == DIE_READ_WAIT && cur_time >= ps->time) {
                g_state.chan[chan_id].state = CHAN_DATA;
                if (g_state.cmd_page_stripe[ps->act]) {
                    g_state.chan[chan_id].pages_left = 0;
                } else {
                    g_state.chan[chan_id].pages_left = g_state.d.pages_per_block;
                }
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
    return "perf.conf";
}

void perf_config_defaults(perf_config_t *cfg) {
    if (!cfg) {
        return;
    }
    cfg->cmd_overhead = 1.7;
    cfg->cmd_overhead_sca = 1.7;
    cfg->sca = 0;
    cfg->chan_speed = 2400;
    cfg->cmd_size = 4096;
    cfg->ecc_parity_size = 600;
    cfg->page_size = 16384;
    cfg->page_parity_size = 1952;
    cfg->tr_fast = 40;
    cfg->tR = 40;
    cfg->tprog_eff = 800;
    cfg->nand_type = 3;
    cfg->tERASE = 3000;
    cfg->qd = 512;
    cfg->chan_num = 16;
    cfg->die_num = 128;
    cfg->plane = 4;
    cfg->iwl_slot = 256;
    cfg->read_ratio = 100;
    cfg->write_ratio = 0;
    cfg->erase_ratio = 0;
    cfg->prio_high_ratio = 0;
    cfg->prio_normal_ratio = 100;
    cfg->prio_low_ratio = 0;
    cfg->io_pattern = PERF_IO_PATTERN_RANDOM;
    cfg->stripe_mode = PERF_STRIPE_CHANNEL_MAJOR;
    cfg->workload = PERF_WORKLOAD_LEGACY;
    cfg->block_size = 0;
    cfg->element = (uint64_t)(1 * 32 * 1024);
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
    } else if (strcmp(key, "ecc_parity_size") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_size") == 0) {
        cfg->page_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_parity_size") == 0) {
        cfg->page_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "ecc_parity") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr_fast") == 0) {
        cfg->tr_fast = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr") == 0) {
        cfg->tR = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tprog_eff") == 0) {
        cfg->tprog_eff = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tprog") == 0) {
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
    } else if (strcmp(key, "prio_high_ratio") == 0) {
        cfg->prio_high_ratio = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "prio_normal_ratio") == 0) {
        cfg->prio_normal_ratio = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "prio_low_ratio") == 0) {
        cfg->prio_low_ratio = (int)strtol(value, &end, 10);
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
        } else if (strcmp(value, "page_across_chan") == 0 ||
                   strcmp(value, "page_stripe") == 0) {
            cfg->stripe_mode = PERF_STRIPE_PAGE_ACROSS_CHAN;
            end = (char *)(value + strlen(value));
        } else {
            return -1;
        }
    } else if (strcmp(key, "workload") == 0) {
        if (strcmp(value, "legacy") == 0 || strcmp(value, "mixed") == 0 ||
            strcmp(value, "default") == 0) {
            cfg->workload = PERF_WORKLOAD_LEGACY;
            end = (char *)(value + strlen(value));
        } else if (strcmp(value, "fulldev_seq_read") == 0 ||
                   strcmp(value, "seq_read_bw") == 0) {
            cfg->workload = PERF_WORKLOAD_FULLDEV_SEQ_READ;
            end = (char *)(value + strlen(value));
        } else if (strcmp(value, "fulldev_seq_write") == 0 ||
                   strcmp(value, "seq_write_bw") == 0) {
            cfg->workload = PERF_WORKLOAD_FULLDEV_SEQ_WRITE;
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
        cfg->page_size <= 0 || cfg->ecc_parity_size < 0 ||
        cfg->page_parity_size < 0 || cfg->plane <= 0) {
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
    if (cfg->prio_high_ratio + cfg->prio_normal_ratio +
            cfg->prio_low_ratio <=
        0) {
        return -1;
    }
    if (cfg->io_pattern != PERF_IO_PATTERN_RANDOM &&
        cfg->io_pattern != PERF_IO_PATTERN_SEQUENTIAL) {
        return -1;
    }
    if (cfg->stripe_mode != PERF_STRIPE_CHANNEL_MAJOR &&
        cfg->stripe_mode != PERF_STRIPE_GLOBAL_DIE &&
        cfg->stripe_mode != PERF_STRIPE_PAGE_ACROSS_CHAN) {
        return -1;
    }
    if (cfg->workload != PERF_WORKLOAD_LEGACY &&
        cfg->workload != PERF_WORKLOAD_FULLDEV_SEQ_READ &&
        cfg->workload != PERF_WORKLOAD_FULLDEV_SEQ_WRITE) {
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
    {
        int read_sz =
            cfg->block_size > 0 ? cfg->block_size : cfg->cmd_size;
        int write_sz =
            cfg->block_size > 0 ? cfg->block_size : cfg->page_size;
        int page_unit = cfg->page_size;
        int pages_per_block;
        int read_page_bytes;
        int write_page_bytes;
        uint64_t read_wire_page;
        uint64_t write_wire_page;

        if (cfg->block_size > 0) {
            pages_per_block = cfg->block_size / cfg->page_size;
            read_page_bytes = cfg->page_size;
            write_page_bytes = cfg->page_size;
        } else {
            pages_per_block = 1;
            read_page_bytes = cfg->cmd_size;
            write_page_bytes = cfg->page_size;
        }

        g_state.d.pages_per_block = pages_per_block;
        g_state.d.page_unit = page_unit;
        g_state.d.read_xfer_size = read_sz;
        g_state.d.write_xfer_size = write_sz;
        g_state.d.read_bytes_per_page = read_page_bytes;
        g_state.d.write_bytes_per_page = write_page_bytes;
        g_state.d.read_bytes_per_cmd = read_sz;
        g_state.d.write_bytes_per_cmd = write_sz;
        read_wire_page =
            (uint64_t)read_page_bytes + (uint64_t)cfg->ecc_parity_size;
        write_wire_page =
            (uint64_t)write_page_bytes + (uint64_t)cfg->page_parity_size;

        if (cfg->sca) {
            g_state.d.cmd_time =
                (uint64_t)(cfg->cmd_overhead_sca * TIME_SCALE);
        } else {
            g_state.d.cmd_time = (uint64_t)(cfg->cmd_overhead * TIME_SCALE);
        }
        /* tR from cmd_size; each page xfer uses page_size + parity per page. */
        if (cfg->cmd_size == 4096) {
            g_state.d.tread = (uint64_t)cfg->tr_fast * TIME_SCALE;
        } else {
            g_state.d.tread = (uint64_t)cfg->tR * TIME_SCALE;
        }
        g_state.d.tprog =
            (uint64_t)((uint64_t)cfg->tprog_eff * (uint64_t)cfg->nand_type) *
            TIME_SCALE;
        g_state.d.terase = (uint64_t)cfg->tERASE * TIME_SCALE;
        g_state.d.data_time_read_page =
            read_wire_page * TIME_SCALE / (uint64_t)cfg->chan_speed;
        g_state.d.data_time_write_page =
            write_wire_page * TIME_SCALE / (uint64_t)cfg->chan_speed;
        {
            int xor_ratio =
                (cfg->die_num > 64) ? 64 : cfg->die_num;
            g_state.d.xor_factor =
                ((double)xor_ratio - 1) / (double)xor_ratio;
        }
        /* Per-page wire rate × channels (steady page stream on each ch). */
        g_state.d.read_ceiling_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps(read_wire_page, g_state.d.data_time_read_page);
        g_state.d.write_ceiling_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps(write_wire_page,
                                 g_state.d.data_time_write_page);
        g_state.d.read_ceiling_host_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps((uint64_t)read_page_bytes,
                                 g_state.d.data_time_read_page);
        g_state.d.write_ceiling_host_mbps =
            (double)cfg->chan_num *
            channel_payload_mbps((uint64_t)write_page_bytes,
                                 g_state.d.data_time_write_page);
        g_state.d.read_ceiling_xor_mbps =
            g_state.d.read_ceiling_mbps * g_state.d.xor_factor;
        g_state.d.write_ceiling_xor_mbps =
            g_state.d.write_ceiling_mbps * g_state.d.xor_factor;
    }
    // data_time_*_page: one page (page_size + parity) on channel per phase.

    g_state.map = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_op = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_prio = (int *)calloc(cfg->qd, sizeof(int));
    g_state.die_ctx = (die_ctx_t *)calloc(
        cfg->chan_num * g_state.d.die_per_chan, sizeof(die_ctx_t));
    g_state.rr_die = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.stripe_cursor = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.cmd_stripe_base = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_done = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_pages_launched = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_page_stripe = (int *)calloc(cfg->qd, sizeof(int));
    g_state.chan = (chan_t *)calloc(cfg->chan_num, sizeof(chan_t));

    if (!g_state.map || !g_state.cmd_op || !g_state.cmd_prio ||
        !g_state.die_ctx || !g_state.rr_die || !g_state.stripe_cursor ||
        !g_state.cmd_stripe_base || !g_state.cmd_pages_done ||
        !g_state.cmd_pages_launched || !g_state.cmd_page_stripe ||
        !g_state.chan) {
        perf_cleanup();
        return -1;
    }

    srand((unsigned)time(NULL));

    for (i = 0; i < cfg->chan_num; i++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            int prio;
            int op;
            int slot_idx;
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
                ctx->slots[slot_idx].act = 0xFFF;
                ctx->slots[slot_idx].time = 0;
                ctx->slots[slot_idx].host_pages_left = 0;
                ctx->slots[slot_idx].page_idx = -1;
            }
            ctx->suspended_slot = -1;
            ctx->suspended_act = 0xFFF;
            ctx->suspended_op = OP_MAX;
            ctx->suspended_time = 0;
            ctx->suspend_write_cnt = 0;
            ctx->suspend_erase_cnt = 0;
            for (prio = 0; prio < PRIO_MAX; prio++) {
                for (op = 0; op < OP_MAX; op++) {
                    queue_init(&ctx->q[prio][op], cfg->iwl_slot);
                    if (!ctx->q[prio][op].list) {
                        perf_cleanup();
                        return -1;
                    }
                }
            }
        }
    }

    for (i = 0; i < cfg->chan_num; i++) {
        g_state.chan[i].state = CHAN_IDLE;
        g_state.chan[i].act = 0xFFF;
        g_state.chan[i].op = OP_READ;
        g_state.chan[i].die = -1;
        g_state.chan[i].slot = -1;
        g_state.chan[i].pages_left = 0;
        g_state.rr_die[i] = 0;
    }

    for (i = 0; i < cfg->qd; i++) {
        g_state.map[i] = 0;
        g_state.cmd_op[i] = OP_READ;
        g_state.cmd_prio[i] = PRIO_NORMAL;
    }

    g_state.next_die = 0;
    g_state.rr_chan = 0;
    g_state.global_page_stripe = 0;
    g_state.initialized = 1;
    return 0;
}

void perf_cleanup(void) {
    int i;
    int j;

    if (g_state.die_ctx && g_state.d.die_per_chan > 0 &&
        g_state.cfg.chan_num > 0) {
        for (i = 0; i < g_state.cfg.chan_num; i++) {
            for (j = 0; j < g_state.d.die_per_chan; j++) {
                int prio;
                int op;
                die_ctx_t *ctx = die_ctx_at(i, j);
                free(ctx->slots);
                ctx->slots = NULL;
                for (prio = 0; prio < PRIO_MAX; prio++) {
                    for (op = 0; op < OP_MAX; op++) {
                        free(ctx->q[prio][op].list);
                        ctx->q[prio][op].list = NULL;
                    }
                }
            }
        }
    }

    free(g_state.map);
    free(g_state.cmd_op);
    free(g_state.cmd_prio);
    free(g_state.die_ctx);
    free(g_state.rr_die);
    free(g_state.stripe_cursor);
    free(g_state.cmd_stripe_base);
    free(g_state.cmd_pages_done);
    free(g_state.cmd_pages_launched);
    free(g_state.cmd_page_stripe);
    free(g_state.chan);

    memset(&g_state, 0, sizeof(g_state));
}

int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds) {
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

    for (i = 0; i < g_state.cfg.qd; i++) {
        if (g_state.map[i] == 0) {
            int op = select_op();
            int prio = select_prio();
            act = i;
            g_state.cmd_op[act] = op;
            g_state.cmd_prio[act] = prio;
            if (use_page_across_chan_stripe()) {
                g_state.cmd_stripe_base[act] = g_state.global_page_stripe;
                g_state.global_page_stripe += g_state.d.pages_per_block;
                g_state.cmd_pages_done[act] = 0;
                g_state.cmd_pages_launched[act] = 0;
                g_state.cmd_page_stripe[act] = 1;
                stripe_page_target(g_state.cmd_stripe_base[act], &chan_id,
                                   &die_in_chan);
            } else {
                g_state.cmd_page_stripe[act] = 0;
                select_target(&chan_id, &die_in_chan);
            }
            enqueue_cmd(chan_id, die_in_chan, op, prio, act);
            g_state.map[i] = 1;
            (*inflight_cmds)++;
            return 1;
        }
    }

    return 0;
}

void perf_run(perf_stats_t *stats) {
    int i;
    uint64_t cur_time;
    uint64_t sim_time = 0;
    uint64_t total_cmd = 0;
    uint64_t read_cmd = 0;
    uint64_t write_cmd = 0;
    uint64_t erase_cmd = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    int tmp_cmd_cnt = 0;
    int inflight_cmds = 0;
    uint64_t start_time = 0;
    uint64_t end_time = 0;

    if (!g_state.initialized) {
        return;
    }

    while (1) {
        if (total_cmd > g_state.cfg.element) {
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

        perf_gen_cmd(tmp_cmd_cnt, &inflight_cmds);

        {
            int progressed = 0;
            uint64_t next_event = UINT64_MAX;

        for (i = 0; i < g_state.cfg.chan_num; i++) {
            switch (g_state.chan[i].state) {
                case CHAN_IDLE:
                    cur_time = sim_time;
                    if (complete_wait_ops(i, cur_time, &inflight_cmds, &total_cmd,
                                          &write_cmd, &erase_cmd) > 0) {
                        progressed = 1;
                    }

                    if (try_launch_pending_page_stripes(cur_time) > 0) {
                        progressed = 1;
                    }

                    // Channel idle arbitration: cmds first, read data can preempt
                    // write/erase cmds, then write data last.
                    // Priority: read cmd, read data, write/erase cmds, then write data.
                    if (try_schedule_cmd(i, cur_time, OP_READ)) {
                        progressed = 1;
                        break;
                    }

                    if (try_schedule_read_data(i, cur_time)) {
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

                    if (try_schedule_write_data(i, cur_time)) {
                        progressed = 1;
                    }
                    break;
                case CHAN_CMD:
                    cur_time = sim_time;
                    if (cur_time >= g_state.chan[i].time) {
                        if (g_state.chan[i].op == OP_READ) {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps = slot_at(ctx, g_state.chan[i].slot);
                            ps->state = DIE_READ_WAIT;
                            ps->time = cur_time + g_state.d.tread;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                            g_state.chan[i].slot = -1;
                        } else if (g_state.chan[i].op == OP_WRITE) {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps = slot_at(ctx, g_state.chan[i].slot);
                            int act = g_state.chan[i].act;

                            if (!g_state.cmd_page_stripe[act]) {
                                ps->host_pages_left = g_state.d.pages_per_block;
                            } else {
                                ps->host_pages_left = 0;
                            }
                            ps->state = DIE_WRITE_DATA_READY;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                            g_state.chan[i].slot = -1;
                        } else {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps = slot_at(ctx, g_state.chan[i].slot);
                            ps->state = DIE_ERASE_WAIT;
                            ps->time = cur_time + g_state.d.terase;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                            g_state.chan[i].slot = -1;
                        }
                        progressed = 1;
                    }
                    break;
                case CHAN_DATA:
                    cur_time = sim_time;
                    if (cur_time >= g_state.chan[i].time) {
                        if (g_state.chan[i].op == OP_READ) {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps = slot_at(ctx, g_state.chan[i].slot);
                            int act = g_state.chan[i].act;

                            read_bytes +=
                                (uint64_t)g_state.d.read_bytes_per_page;
                            if (!g_state.cmd_page_stripe[act] &&
                                g_state.chan[i].pages_left > 1) {
                                g_state.chan[i].pages_left--;
                                g_state.chan[i].time =
                                    cur_time + g_state.d.data_time_read_page;
                                progressed = 1;
                                break;
                            }

                            if (g_state.cmd_page_stripe[act]) {
                                g_state.cmd_pages_done[act]++;
                                ps->act = 0xFFF;
                                ps->state = DIE_IDLE;
                                ps->page_idx = -1;
                                if (g_state.cmd_pages_done[act] <
                                    g_state.d.pages_per_block) {
                                    g_state.chan[i].act = 0xFFF;
                                    g_state.chan[i].state = CHAN_IDLE;
                                    g_state.chan[i].op = OP_READ;
                                    g_state.chan[i].die = -1;
                                    g_state.chan[i].slot = -1;
                                    g_state.chan[i].pages_left = 0;
                                    progressed = 1;
                                    break;
                                }
                                complete_host_page_stripe(act, OP_READ,
                                                          &inflight_cmds,
                                                          &total_cmd,
                                                          &read_cmd, NULL);
                            } else {
                                g_state.map[act] = 0;
                                inflight_cmds--;
                                total_cmd++;
                                read_cmd++;
                            }
                            if (ctx->suspended_op != OP_MAX) {
                                plane_slot_t *sps =
                                    slot_at(ctx, ctx->suspended_slot);
                                sps->act = ctx->suspended_act;
                                sps->time = cur_time + ctx->suspended_time;
                                sps->state = (ctx->suspended_op == OP_WRITE)
                                                 ? DIE_WRITE_WAIT
                                                 : DIE_ERASE_WAIT;
                                if (ctx->suspended_slot != g_state.chan[i].slot) {
                                    ps->act = 0xFFF;
                                    ps->state = DIE_IDLE;
                                }
                                ctx->suspended_act = 0xFFF;
                                ctx->suspended_op = OP_MAX;
                                ctx->suspended_time = 0;
                                ctx->suspended_slot = -1;
                            } else if (!g_state.cmd_page_stripe[act]) {
                                ps->act = 0xFFF;
                                ps->state = DIE_IDLE;
                                ps->page_idx = -1;
                            }
                        } else if (g_state.chan[i].op == OP_WRITE) {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            plane_slot_t *ps = slot_at(ctx, g_state.chan[i].slot);

                            write_bytes +=
                                (uint64_t)g_state.d.write_bytes_per_page;
                            ps->state = DIE_WRITE_WAIT;
                            ps->time = cur_time + g_state.d.tprog;
                        }
                        g_state.chan[i].act = 0xFFF;
                        g_state.chan[i].state = CHAN_IDLE;
                        g_state.chan[i].op = OP_READ;
                        g_state.chan[i].die = -1;
                        g_state.chan[i].slot = -1;
                        g_state.chan[i].pages_left = 0;
                        progressed = 1;
                    }
                    break;
                default:
                    printf("Should not be here for chan state!\n");
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
                                ps->time > sim_time && ps->time < next_event) {
                                next_event = ps->time;
                            }
                        }
                    }
                }
                if (next_event == UINT64_MAX) {
                    break;
                }
                sim_time = next_event;
            }
        }
    }

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
    double xor_factor;
    double bytes_to_mbps;

    memset(&bw, 0, sizeof(bw));

    if (!stats || !g_state.initialized) {
        return bw;
    }

    bw.read_ceiling_mbps = g_state.d.read_ceiling_mbps;
    bw.write_ceiling_mbps = g_state.d.write_ceiling_mbps;
    bw.read_ceiling_host_mbps = g_state.d.read_ceiling_host_mbps;
    bw.write_ceiling_host_mbps = g_state.d.write_ceiling_host_mbps;
    bw.read_ceiling_xor_mbps = g_state.d.read_ceiling_xor_mbps;
    bw.write_ceiling_xor_mbps = g_state.d.write_ceiling_xor_mbps;

    if (stats->end_time <= stats->start_time) {
        return bw;
    }

    elapsed_us =
        (double)(stats->end_time - stats->start_time) / (double)TIME_SCALE;
    if (elapsed_us <= 0.0) {
        return bw;
    }

    xor_factor = g_state.d.xor_factor;
    bytes_to_mbps = 1000000.0 / 1048576.0 / elapsed_us;

    bw.read_mbps_raw = (double)stats->read_bytes * bytes_to_mbps;
    bw.write_mbps_raw = (double)stats->write_bytes * bytes_to_mbps;
    bw.read_mbps = bw.read_mbps_raw * xor_factor;
    bw.write_mbps = bw.write_mbps_raw * xor_factor;
    bw.total_mbps = bw.read_mbps + bw.write_mbps;

    bw.read_util_wire_pct = pct_of(bw.read_mbps_raw, bw.read_ceiling_mbps);
    bw.read_util_host_pct = pct_of(bw.read_mbps_raw, bw.read_ceiling_host_mbps);
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
    int xor_ratio;
    double xor_factor;
    uint64_t total_effective;

    if (!stats || !g_state.initialized) {
        return iops;
    }

    if (stats->end_time <= stats->start_time) {
        return iops;
    }

    elapsed = (stats->end_time - stats->start_time) / (double)TIME_SCALE;
    if (elapsed <= 0.0) {
        return iops;
    }

    scale = 1000000.0 / elapsed;
    xor_ratio = (g_state.cfg.die_num > 64) ? 64 : g_state.cfg.die_num;
    xor_factor = ((double)xor_ratio - 1) / (double)xor_ratio;
    // Apply die-level XOR scaling factor.

    total_effective =
        (stats->total_cmd > (uint64_t)g_state.cfg.qd)
            ? (stats->total_cmd - (uint64_t)g_state.cfg.qd)
            : 0;

    iops.total = (double)total_effective * scale * xor_factor;
    iops.read = (double)stats->read_cmd * scale * xor_factor;
    iops.write = (double)stats->write_cmd * scale * xor_factor;
    iops.erase = (double)stats->erase_cmd * scale * xor_factor;

    return iops;
}
