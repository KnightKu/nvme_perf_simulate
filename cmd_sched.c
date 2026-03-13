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

typedef struct die_ctx_s {
    int act;
    uint64_t time;
    queue_t q[PRIO_MAX][OP_MAX];
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
    uint64_t time;
} chan_t;

typedef struct perf_derived {
    uint64_t cmd_time;
    uint64_t tread;
    uint64_t tprog;
    uint64_t terase;
    uint64_t data_time;
    int die_per_chan;
} perf_derived_t;

typedef struct perf_state {
    perf_config_t cfg;
    perf_derived_t d;
    int *map;
    int *cmd_op;
    int *cmd_prio;
    die_ctx_t *die_ctx;
    int *die_state;
    int *rr_die;
    chan_t *chan;
    int initialized;
} perf_state_t;

static perf_state_t g_state;

static inline die_ctx_t *die_ctx_at(int chan_id, int die) {
    return &g_state.die_ctx[chan_id * g_state.d.die_per_chan + die];
}

static inline int *die_state_at(int chan_id, int die) {
    return &g_state.die_state[chan_id * g_state.d.die_per_chan + die];
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

static inline int select_op() {
    int total = g_state.cfg.read_ratio + g_state.cfg.write_ratio +
                g_state.cfg.erase_ratio;
    int r = rand() % total;
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

static inline void enqueue_cmd(int chan_id, int die_in_chan, int op, int prio,
                               int act) {
    die_ctx_t *ctx = die_ctx_at(chan_id, die_in_chan);
    queue_push(&ctx->q[prio][op], act);
}

static inline void complete_wait_ops(int chan_id, uint64_t cur_time,
                                     int *inflight_cmds,
                                     uint64_t *total_cmd,
                                     uint64_t *write_cmd,
                                     uint64_t *erase_cmd) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int state = *die_state_at(chan_id, j);
        die_ctx_t *ctx = die_ctx_at(chan_id, j);
        if ((state == DIE_WRITE_WAIT || state == DIE_ERASE_WAIT) &&
            ctx->act != 0xFFF && cur_time >= ctx->time) {
            int act = ctx->act;
            ctx->act = 0xFFF;
            *die_state_at(chan_id, j) = DIE_IDLE;
            g_state.map[act] = 0;
            (*inflight_cmds)--;
            (*total_cmd)++;
            if (g_state.cmd_op[act] == OP_WRITE) {
                (*write_cmd)++;
            } else if (g_state.cmd_op[act] == OP_ERASE) {
                (*erase_cmd)++;
            }
        }
    }
}

static inline int try_schedule_cmd(int chan_id, uint64_t cur_time, int op) {
    int prio;
    int j;

    for (prio = PRIO_HIGH; prio < PRIO_MAX; prio++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
            int act;
            die_ctx_t *ctx;
            queue_t *q;
            int state;

            ctx = die_ctx_at(chan_id, die);
            q = &ctx->q[prio][op];
            if (q->empty) {
                continue;
            }

            state = *die_state_at(chan_id, die);
            if (state != DIE_IDLE) {
                if (op != OP_READ) {
                    continue;
                }
                if (state != DIE_WRITE_WAIT && state != DIE_ERASE_WAIT) {
                    continue;
                }
                if (state == DIE_WRITE_WAIT &&
                    ctx->suspend_write_cnt >= MAX_SUSPEND_WRITE) {
                    continue;
                }
                if (state == DIE_ERASE_WAIT &&
                    ctx->suspend_erase_cnt >= MAX_SUSPEND_ERASE) {
                    continue;
                }
                if (ctx->time <= cur_time) {
                    continue;
                }
                ctx->suspended_act = ctx->act;
                ctx->suspended_op = (state == DIE_WRITE_WAIT) ? OP_WRITE
                                                              : OP_ERASE;
                ctx->suspended_time = ctx->time - cur_time;
                if (state == DIE_WRITE_WAIT) {
                    ctx->suspend_write_cnt++;
                } else {
                    ctx->suspend_erase_cnt++;
                }
            }

            act = queue_pop(q);
            ctx->act = act;
            *die_state_at(chan_id, die) = DIE_CMD;

            g_state.chan[chan_id].state = CHAN_CMD;
            g_state.chan[chan_id].time = cur_time + g_state.d.cmd_time;
            g_state.chan[chan_id].act = act;
            g_state.chan[chan_id].op = op;
            g_state.chan[chan_id].die = die;
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
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        if (*die_state_at(chan_id, die) == DIE_READ_WAIT &&
            cur_time >= ctx->time) {
            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time = cur_time + g_state.d.data_time;
            g_state.chan[chan_id].act = ctx->act;
            g_state.chan[chan_id].op = OP_READ;
            g_state.chan[chan_id].die = die;
            *die_state_at(chan_id, die) = DIE_READ_DATA;
            g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
            return 1;
        }
    }

    return 0;
}

static inline int try_schedule_write_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        die_ctx_t *ctx = die_ctx_at(chan_id, die);
        if (*die_state_at(chan_id, die) == DIE_WRITE_DATA_READY) {
            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time = cur_time + g_state.d.data_time;
            g_state.chan[chan_id].act = ctx->act;
            g_state.chan[chan_id].op = OP_WRITE;
            g_state.chan[chan_id].die = die;
            *die_state_at(chan_id, die) = DIE_WRITE_DATA;
            g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
            return 1;
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
    cfg->tr_fast = 40;
    cfg->tR = 40;
    cfg->tPROG = 800;
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
    } else if (strcmp(key, "ecc_parity_size") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "ecc_parity") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr_fast") == 0) {
        cfg->tr_fast = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr") == 0) {
        cfg->tR = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tprog") == 0) {
        cfg->tPROG = (int)strtol(value, &end, 10);
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
        cfg->ecc_parity_size < 0) {
        return -1;
    }

    if (cfg->die_num % cfg->chan_num != 0) {
        return -1;
    }

    if (cfg->read_ratio + cfg->write_ratio + cfg->erase_ratio <= 0) {
        return -1;
    }
    if (cfg->prio_high_ratio + cfg->prio_normal_ratio +
            cfg->prio_low_ratio <=
        0) {
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *cfg;
    g_state.d.die_per_chan = cfg->die_num / cfg->chan_num;
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
    g_state.d.tprog = (uint64_t)cfg->tPROG * TIME_SCALE;
    g_state.d.terase = (uint64_t)cfg->tERASE * TIME_SCALE;
    g_state.d.data_time =
        (uint64_t)(((uint64_t)cfg->cmd_size + (uint64_t)cfg->ecc_parity_size) *
                   TIME_SCALE / cfg->chan_speed);

    g_state.map = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_op = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_prio = (int *)calloc(cfg->qd, sizeof(int));
    g_state.die_ctx = (die_ctx_t *)calloc(
        cfg->chan_num * g_state.d.die_per_chan, sizeof(die_ctx_t));
    g_state.die_state =
        (int *)calloc(cfg->chan_num * g_state.d.die_per_chan, sizeof(int));
    g_state.rr_die = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.chan = (chan_t *)calloc(cfg->chan_num, sizeof(chan_t));

    if (!g_state.map || !g_state.cmd_op || !g_state.cmd_prio ||
        !g_state.die_ctx || !g_state.die_state || !g_state.rr_die ||
        !g_state.chan) {
        perf_cleanup();
        return -1;
    }

    srand((unsigned)time(NULL));

    for (i = 0; i < cfg->chan_num; i++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            int prio;
            int op;
            die_ctx_t *ctx = die_ctx_at(i, j);
            ctx->act = 0xFFF;
            ctx->time = 0;
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
            *die_state_at(i, j) = DIE_IDLE;
        }
    }

    for (i = 0; i < cfg->chan_num; i++) {
        g_state.chan[i].state = CHAN_IDLE;
        g_state.chan[i].act = 0xFFF;
        g_state.chan[i].op = OP_READ;
        g_state.chan[i].die = -1;
        g_state.rr_die[i] = 0;
    }

    for (i = 0; i < cfg->qd; i++) {
        g_state.map[i] = 0;
        g_state.cmd_op[i] = OP_READ;
        g_state.cmd_prio[i] = PRIO_NORMAL;
    }

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
    free(g_state.die_state);
    free(g_state.rr_die);
    free(g_state.chan);

    memset(&g_state, 0, sizeof(g_state));
}

int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds) {
    int i;
    int act;
    int tmp;

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
            tmp = rand() % g_state.cfg.die_num;
            enqueue_cmd(tmp % g_state.cfg.chan_num,
                        tmp / g_state.cfg.chan_num, op, prio, act);
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
    uint64_t total_cmd = 0;
    uint64_t read_cmd = 0;
    uint64_t write_cmd = 0;
    uint64_t erase_cmd = 0;
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
            start_time = get_time_us();
        }

        perf_gen_cmd(tmp_cmd_cnt, &inflight_cmds);

        for (i = 0; i < g_state.cfg.chan_num; i++) {
            switch (g_state.chan[i].state) {
                case CHAN_IDLE:
                    cur_time = get_time_us();
                    complete_wait_ops(i, cur_time, &inflight_cmds, &total_cmd,
                                      &write_cmd, &erase_cmd);

                    // Priority: read cmd, read data, write/erase cmds, then write data.
                    if (try_schedule_cmd(i, cur_time, OP_READ)) {
                        break;
                    }

                    if (try_schedule_read_data(i, cur_time)) {
                        break;
                    }

                    if (try_schedule_cmd(i, cur_time, OP_WRITE)) {
                        break;
                    }

                    if (try_schedule_cmd(i, cur_time, OP_ERASE)) {
                        break;
                    }

                    (void)try_schedule_write_data(i, cur_time);
                    break;
                case CHAN_CMD:
                    cur_time = get_time_us();
                    if (cur_time >= g_state.chan[i].time) {
                        if (g_state.chan[i].op == OP_READ) {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_READ_WAIT;
                            die_ctx_at(i, g_state.chan[i].die)->time =
                                cur_time + g_state.d.tread;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                        } else if (g_state.chan[i].op == OP_WRITE) {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_WRITE_DATA_READY;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                        } else {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_ERASE_WAIT;
                            die_ctx_at(i, g_state.chan[i].die)->time =
                                cur_time + g_state.d.terase;
                            g_state.chan[i].state = CHAN_IDLE;
                            g_state.chan[i].act = 0xFFF;
                            g_state.chan[i].op = OP_READ;
                            g_state.chan[i].die = -1;
                        }
                    }
                    break;
                case CHAN_DATA:
                    cur_time = get_time_us();
                    if (cur_time >= g_state.chan[i].time) {
                        if (g_state.chan[i].op == OP_READ) {
                            die_ctx_t *ctx = die_ctx_at(i, g_state.chan[i].die);
                            g_state.map[g_state.chan[i].act] = 0;
                            inflight_cmds--;
                            total_cmd++;
                            read_cmd++;
                            if (ctx->suspended_op != OP_MAX) {
                                ctx->act = ctx->suspended_act;
                                ctx->time = cur_time + ctx->suspended_time;
                                *die_state_at(i, g_state.chan[i].die) =
                                    (ctx->suspended_op == OP_WRITE)
                                        ? DIE_WRITE_WAIT
                                        : DIE_ERASE_WAIT;
                                ctx->suspended_act = 0xFFF;
                                ctx->suspended_op = OP_MAX;
                                ctx->suspended_time = 0;
                            } else {
                                ctx->act = 0xFFF;
                                *die_state_at(i, g_state.chan[i].die) =
                                    DIE_IDLE;
                            }
                        } else if (g_state.chan[i].op == OP_WRITE) {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_WRITE_WAIT;
                            die_ctx_at(i, g_state.chan[i].die)->time =
                                cur_time + g_state.d.tprog;
                        }
                        g_state.chan[i].act = 0xFFF;
                        g_state.chan[i].state = CHAN_IDLE;
                        g_state.chan[i].op = OP_READ;
                        g_state.chan[i].die = -1;
                    }
                    break;
                default:
                    printf("Should not be here for chan state!\n");
                    exit(1);
            }
        }
    }

    end_time = get_time_us();
    if (stats) {
        stats->total_cmd = total_cmd;
        stats->read_cmd = read_cmd;
        stats->write_cmd = write_cmd;
        stats->erase_cmd = erase_cmd;
        stats->start_time = start_time;
        stats->end_time = end_time;
    }
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
