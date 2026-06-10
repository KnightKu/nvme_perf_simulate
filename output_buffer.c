#include "output_buffer.h"

#include "cw_path.h"
#include "sched_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int act;
} out_chunk_t;

static int s_capacity;
static int s_chunk_bytes;
static int s_fill;
static int s_qd;
static out_chunk_t *s_fifo;
static int s_fifo_cap;
static int s_fifo_head;
static int s_fifo_tail;
static int s_fifo_count;

int output_buffer_init(int capacity_bytes, int chunk_bytes, int qd) {
    int slots;

    if (capacity_bytes <= 0 || chunk_bytes <= 0 || qd <= 0) {
        return -1;
    }
    if (capacity_bytes % chunk_bytes != 0) {
        return -1;
    }

    s_capacity = capacity_bytes;
    s_chunk_bytes = chunk_bytes;
    s_qd = qd;
    s_fill = 0;
    s_fifo_head = 0;
    s_fifo_tail = 0;
    s_fifo_count = 0;

    slots = (capacity_bytes / chunk_bytes) + qd * 64;
    s_fifo_cap = slots;
    s_fifo = (out_chunk_t *)calloc(slots, sizeof(out_chunk_t));
    if (!s_fifo) {
        return -1;
    }
    return 0;
}

void output_buffer_cleanup(void) {
    free(s_fifo);
    s_fifo = NULL;
    s_capacity = 0;
    s_chunk_bytes = 0;
    s_fill = 0;
    s_fifo_count = 0;
    s_fifo_cap = 0;
}

int output_buffer_fill_bytes(void) {
    return s_fill;
}

int output_buffer_push(int act, int host_bytes) {
    int chunks = host_bytes / s_chunk_bytes;
    int i;

    if (chunks <= 0 || host_bytes % s_chunk_bytes != 0) {
        return -1;
    }
    if (s_fill + host_bytes > s_capacity) {
        return -1;
    }

    for (i = 0; i < chunks; i++) {
        if (s_fifo_count >= s_fifo_cap) {
            return -1;
        }
        s_fifo[s_fifo_tail].act = act;
        s_fifo_tail = (s_fifo_tail + 1) % s_fifo_cap;
        s_fifo_count++;
    }
    s_fill += host_bytes;
    return 0;
}

int output_buffer_drain(uint64_t *read_bytes, const cw_run_ctx_t *ctx) {
    int drained = 0;

    while (s_fill >= s_chunk_bytes && s_fifo_count > 0) {
        int act = s_fifo[s_fifo_head].act;

        s_fifo_head = (s_fifo_head + 1) % s_fifo_cap;
        s_fifo_count--;
        s_fill -= s_chunk_bytes;
        if (read_bytes) {
            *read_bytes += (uint64_t)s_chunk_bytes;
        }
        g_state.cmd_cw_read_done[act]++;
        cw_host_read_chunk_done(act, ctx);
        drained++;
    }

    return drained;
}
