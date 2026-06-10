#include "cmd_pool.h"

#include "sched_internal.h"

int cmd_pool_init(int capacity) {
    g_state.cmd_pool.capacity = capacity;
    g_state.cmd_pool.count = 0;
    queue_init(&g_state.cmd_pool.fifo, capacity);
    if (!g_state.cmd_pool.fifo.list) {
        return -1;
    }
    return 0;
}

void cmd_pool_cleanup(void) {
    free(g_state.cmd_pool.fifo.list);
    g_state.cmd_pool.fifo.list = NULL;
    g_state.cmd_pool.count = 0;
    g_state.cmd_pool.capacity = 0;
}

int cmd_pool_push(int act) {
    cmd_pool_t *pool = &g_state.cmd_pool;

    if (pool->count >= pool->capacity) {
        return 0;
    }
    queue_push(&pool->fifo, act);
    pool->count++;
    return 1;
}

int cmd_pool_pop(void) {
    cmd_pool_t *pool = &g_state.cmd_pool;
    int act = queue_pop(&pool->fifo);

    pool->count--;
    return act;
}

int cmd_pool_empty(void) {
    return g_state.cmd_pool.count == 0;
}

int cmd_pool_is_full(void) {
    return g_state.cmd_pool.count >= g_state.cmd_pool.capacity;
}
