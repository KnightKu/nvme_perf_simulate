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

struct timeval tv;

static inline uint64_t get_time_us() {
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000 + tv.tv_usec);
}

typedef struct list_s {
    int head;
    int tail;
    int act;
    int empty;
    int *list;
    uint64_t time;
} list_t;

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
    list_t *list_slot;
    int *die_state;
    int *rr_die;
    chan_t *chan;
    int initialized;
} perf_state_t;

static perf_state_t g_state;

static inline list_t *list_at(int chan_id, int die) {
    return &g_state.list_slot[chan_id * g_state.d.die_per_chan + die];
}

static inline int *die_state_at(int chan_id, int die) {
    return &g_state.die_state[chan_id * g_state.d.die_per_chan + die];
}

static inline void update_list_head(int chan_id, int die_in_chan, int act) {
    list_t *list = list_at(chan_id, die_in_chan);
    list->list[list->head] = act;
    list->head++;
    if (list->head == g_state.cfg.iwl_slot) {
        list->head = 0;
    }
    if (list->head != list->tail) {
        list->empty = 0;
    }
}

static inline int pop_list(list_t *list) {
    int act = list->list[list->tail];
    list->tail++;
    if (list->tail == g_state.cfg.iwl_slot) {
        list->tail = 0;
    }
    if (list->head == list->tail) {
        list->empty = 1;
    }
    return act;
}

static inline int peek_list(const list_t *list) {
    if (list->empty) {
        return -1;
    }
    return list->list[list->tail];
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

static inline void complete_wait_ops(int chan_id, uint64_t cur_time,
                                     int *inflight_cmds,
                                     uint64_t *total_cmd,
                                     uint64_t *write_cmd,
                                     uint64_t *erase_cmd) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int state = *die_state_at(chan_id, j);
        list_t *list = list_at(chan_id, j);
        if ((state == DIE_WRITE_WAIT || state == DIE_ERASE_WAIT) &&
            list->act != 0xFFF && cur_time >= list->time) {
            int act = list->act;
            list->act = 0xFFF;
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
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        int act;

        if (*die_state_at(chan_id, die) != DIE_IDLE) {
            continue;
        }

        act = peek_list(list_at(chan_id, die));
        if (act < 0 || g_state.cmd_op[act] != op) {
            continue;
        }

        act = pop_list(list_at(chan_id, die));
        list_at(chan_id, die)->act = act;
        *die_state_at(chan_id, die) = DIE_CMD;

        g_state.chan[chan_id].state = CHAN_CMD;
        g_state.chan[chan_id].time = cur_time + g_state.d.cmd_time;
        g_state.chan[chan_id].act = act;
        g_state.chan[chan_id].op = op;
        g_state.chan[chan_id].die = die;
        g_state.rr_die[chan_id] = (die + 1) % g_state.d.die_per_chan;
        return 1;
    }

    return 0;
}

static inline int try_schedule_read_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < g_state.d.die_per_chan; j++) {
        int die = (g_state.rr_die[chan_id] + j) % g_state.d.die_per_chan;
        if (*die_state_at(chan_id, die) == DIE_READ_WAIT &&
            cur_time >= list_at(chan_id, die)->time) {
            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time = cur_time + g_state.d.data_time;
            g_state.chan[chan_id].act = list_at(chan_id, die)->act;
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
        if (*die_state_at(chan_id, die) == DIE_WRITE_DATA_READY) {
            g_state.chan[chan_id].state = CHAN_DATA;
            g_state.chan[chan_id].time = cur_time + g_state.d.data_time;
            g_state.chan[chan_id].act = list_at(chan_id, die)->act;
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
    cfg->chan_speed = 2400;
    cfg->ecc_parity = 600;
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
    cfg->element = (uint64_t)(1 * 32 * 1024);
}

static int set_config_value(perf_config_t *cfg, const char *key,
                            const char *value) {
    char *end = NULL;

    if (strcmp(key, "cmd_overhead") == 0) {
        cfg->cmd_overhead = strtod(value, &end);
    } else if (strcmp(key, "chan_speed") == 0) {
        cfg->chan_speed = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "ecc_parity") == 0) {
        cfg->ecc_parity = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tR") == 0) {
        cfg->tR = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tPROG") == 0) {
        cfg->tPROG = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tERASE") == 0) {
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
        cfg->iwl_slot <= 0 || cfg->chan_speed <= 0) {
        return -1;
    }

    if (cfg->die_num % cfg->chan_num != 0) {
        return -1;
    }

    if (cfg->read_ratio + cfg->write_ratio + cfg->erase_ratio <= 0) {
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *cfg;
    g_state.d.die_per_chan = cfg->die_num / cfg->chan_num;
    g_state.d.cmd_time = (uint64_t)(cfg->cmd_overhead * TIME_SCALE);
    g_state.d.tread = (uint64_t)cfg->tR * TIME_SCALE;
    g_state.d.tprog = (uint64_t)cfg->tPROG * TIME_SCALE;
    g_state.d.terase = (uint64_t)cfg->tERASE * TIME_SCALE;
    g_state.d.data_time =
        (uint64_t)((4096 + cfg->ecc_parity) * TIME_SCALE / cfg->chan_speed);

    g_state.map = (int *)calloc(cfg->qd, sizeof(int));
    g_state.cmd_op = (int *)calloc(cfg->qd, sizeof(int));
    g_state.list_slot =
        (list_t *)calloc(cfg->chan_num * g_state.d.die_per_chan, sizeof(list_t));
    g_state.die_state =
        (int *)calloc(cfg->chan_num * g_state.d.die_per_chan, sizeof(int));
    g_state.rr_die = (int *)calloc(cfg->chan_num, sizeof(int));
    g_state.chan = (chan_t *)calloc(cfg->chan_num, sizeof(chan_t));

    if (!g_state.map || !g_state.cmd_op || !g_state.list_slot ||
        !g_state.die_state || !g_state.rr_die || !g_state.chan) {
        perf_cleanup();
        return -1;
    }

    srand((unsigned)time(NULL));

    for (i = 0; i < cfg->chan_num; i++) {
        for (j = 0; j < g_state.d.die_per_chan; j++) {
            list_t *list = list_at(i, j);
            list->head = 0;
            list->tail = 0;
            list->act = 0xFFF;
            list->empty = 1;
            list->time = 0;
            list->list = (int *)calloc(cfg->iwl_slot, sizeof(int));
            if (!list->list) {
                perf_cleanup();
                return -1;
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
    }

    g_state.initialized = 1;
    return 0;
}

void perf_cleanup(void) {
    int i;
    int j;

    if (g_state.list_slot && g_state.d.die_per_chan > 0 &&
        g_state.cfg.chan_num > 0) {
        for (i = 0; i < g_state.cfg.chan_num; i++) {
            for (j = 0; j < g_state.d.die_per_chan; j++) {
                list_t *list = list_at(i, j);
                free(list->list);
                list->list = NULL;
            }
        }
    }

    free(g_state.map);
    free(g_state.cmd_op);
    free(g_state.list_slot);
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
            act = i;
            g_state.cmd_op[act] = select_op();
            tmp = rand() % g_state.cfg.die_num;
            update_list_head(tmp % g_state.cfg.chan_num,
                             tmp / g_state.cfg.chan_num, act);
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

                    // Follow the flowchart priority:
                    // Read (cmd -> data), then Program (data -> cmd), then Erase (cmd).
                    if (try_schedule_cmd(i, cur_time, OP_READ)) {
                        break;
                    }

                    if (try_schedule_read_data(i, cur_time)) {
                        break;
                    }

                    if (try_schedule_write_data(i, cur_time)) {
                        break;
                    }

                    if (try_schedule_cmd(i, cur_time, OP_WRITE)) {
                        break;
                    }

                    (void)try_schedule_cmd(i, cur_time, OP_ERASE);
                    break;
                case CHAN_CMD:
                    cur_time = get_time_us();
                    if (cur_time >= g_state.chan[i].time) {
                        if (g_state.chan[i].op == OP_READ) {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_READ_WAIT;
                            list_at(i, g_state.chan[i].die)->time =
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
                            list_at(i, g_state.chan[i].die)->time =
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
                            g_state.map[g_state.chan[i].act] = 0;
                            inflight_cmds--;
                            total_cmd++;
                            read_cmd++;
                            list_at(i, g_state.chan[i].die)->act = 0xFFF;
                            *die_state_at(i, g_state.chan[i].die) = DIE_IDLE;
                        } else if (g_state.chan[i].op == OP_WRITE) {
                            *die_state_at(i, g_state.chan[i].die) =
                                DIE_WRITE_WAIT;
                            list_at(i, g_state.chan[i].die)->time =
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
