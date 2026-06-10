#ifndef OUTPUT_BUFFER_H
#define OUTPUT_BUFFER_H

#include <stdint.h>

typedef struct cw_run_ctx_s {
    int *inflight_cmds;
    uint64_t *total_cmd;
    uint64_t *read_cmd;
} cw_run_ctx_t;

int output_buffer_init(int capacity_bytes, int chunk_bytes, int qd);
void output_buffer_cleanup(void);
int output_buffer_push(int act, int host_bytes);
int output_buffer_drain(uint64_t *read_bytes, const cw_run_ctx_t *ctx);
int output_buffer_fill_bytes(void);

#endif
