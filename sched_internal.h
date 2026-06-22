#ifndef SCHED_INTERNAL_H
#define SCHED_INTERNAL_H

#include "cmd_sched.h"

#include <stdint.h>
#include <stdlib.h>

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

enum IO_PATTERN {
    IO_PATTERN_RANDOM = PERF_IO_PATTERN_RANDOM,
    IO_PATTERN_SEQUENTIAL = PERF_IO_PATTERN_SEQUENTIAL,
};

enum STRIPE_MODE {
    STRIPE_CHANNEL_MAJOR = PERF_STRIPE_CHANNEL_MAJOR,
    STRIPE_GLOBAL_DIE = PERF_STRIPE_GLOBAL_DIE,
};

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

typedef struct queue_s {
    int head;
    int tail;
    int empty;
    int size;
    int *list;
} queue_t;

typedef struct plane_slot_s {
    int state;
    int act;
    uint64_t time;
    int host_pages_left;
} plane_slot_t;

typedef struct die_ctx_s {
    queue_t q[OP_MAX];
    plane_slot_t *slots;
    int slot_count;
} die_ctx_t;

typedef struct chan_s {
    int state;
    int act;
    int op;
    int die;
    int slot;
    uint64_t time;
    int pages_left;
} chan_t;

typedef struct perf_derived {
    uint64_t cmd_time;
    uint64_t tread;
    uint64_t tprog;
    uint64_t terase;
    uint64_t data_time_read_page;
    uint64_t data_time_write_page;
    int pages_per_block;
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
    int *cmd_target_chan;
    int *cmd_target_die;
    die_ctx_t *die_ctx;
    int *rr_die;
    int *stripe_cursor;
    chan_t *chan;
    int rr_chan;
    int next_die;
    int initialized;
} perf_state_t;

extern perf_state_t g_state;

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

void enqueue_cmd(int chan_id, int die_in_chan, int op, int act);
void select_target(int *chan_id, int *die_in_chan);
int cmd_generate_try(int tmp_cmd_cnt, int *inflight_cmds);

#endif
