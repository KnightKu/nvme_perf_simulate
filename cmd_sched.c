#include "cmd_sched.h"

#include <stdio.h>
#include <stdlib.h>
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
    int list[SLOT];
    uint64_t time;
} list_t;

typedef struct chan_s {
    int state;
    int act;
    int op;
    int die;
    uint64_t time;
} chan_t;

static int map[CMD_CNT];
static int cmd_op[CMD_CNT];
static list_t list_slot[CHAN_NUM][DIE_PER_CHAN];
static int die_state[CHAN_NUM][DIE_PER_CHAN];
static int rr_die[CHAN_NUM];
static chan_t chan[CHAN_NUM];

static inline void update_list_head(int chan_id, int die_in_chan, int act) {
    list_t *list = &(list_slot[chan_id][die_in_chan]);
    list->list[list->head] = act;
    list->head++;
    if (list->head == SLOT) {
        list->head = 0;
    }
    if (list->head != list->tail) {
        list->empty = 0;
    }
}

static inline int pop_list(list_t *list) {
    int act = list->list[list->tail];
    list->tail++;
    if (list->tail == SLOT) {
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
    int total = READ_RATIO + WRITE_RATIO + ERASE_RATIO;
    int r = rand() % total;
    if (r < READ_RATIO) {
        return OP_READ;
    }
    r -= READ_RATIO;
    if (r < WRITE_RATIO) {
        return OP_WRITE;
    }
    return OP_ERASE;
}

static inline void complete_wait_ops(int chan_id, uint64_t cur_time,
                                     int *inflight_cmds,
                                     uint64_t *total_cmd) {
    int j;

    for (j = 0; j < DIE_PER_CHAN; j++) {
        if ((die_state[chan_id][j] == DIE_WRITE_WAIT ||
             die_state[chan_id][j] == DIE_ERASE_WAIT) &&
            list_slot[chan_id][j].act != 0xFFF &&
            cur_time >= list_slot[chan_id][j].time) {
            int act = list_slot[chan_id][j].act;
            list_slot[chan_id][j].act = 0xFFF;
            die_state[chan_id][j] = DIE_IDLE;
            map[act] = 0;
            (*inflight_cmds)--;
            (*total_cmd)++;
        }
    }
}

static inline int try_schedule_cmd(int chan_id, uint64_t cur_time, int op) {
    int j;

    for (j = 0; j < DIE_PER_CHAN; j++) {
        int die = (rr_die[chan_id] + j) % DIE_PER_CHAN;
        int act;

        if (die_state[chan_id][die] != DIE_IDLE) {
            continue;
        }

        act = peek_list(&list_slot[chan_id][die]);
        if (act < 0 || cmd_op[act] != op) {
            continue;
        }

        act = pop_list(&list_slot[chan_id][die]);
        list_slot[chan_id][die].act = act;
        die_state[chan_id][die] = DIE_CMD;

        chan[chan_id].state = CHAN_CMD;
        chan[chan_id].time = cur_time + CMD_TIME;
        chan[chan_id].act = act;
        chan[chan_id].op = op;
        chan[chan_id].die = die;
        rr_die[chan_id] = (die + 1) % DIE_PER_CHAN;
        return 1;
    }

    return 0;
}

static inline int try_schedule_read_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < DIE_PER_CHAN; j++) {
        int die = (rr_die[chan_id] + j) % DIE_PER_CHAN;
        if (die_state[chan_id][die] == DIE_READ_WAIT &&
            cur_time >= list_slot[chan_id][die].time) {
            chan[chan_id].state = CHAN_DATA;
            chan[chan_id].time = cur_time + DATA_TIME;
            chan[chan_id].act = list_slot[chan_id][die].act;
            chan[chan_id].op = OP_READ;
            chan[chan_id].die = die;
            die_state[chan_id][die] = DIE_READ_DATA;
            rr_die[chan_id] = (die + 1) % DIE_PER_CHAN;
            return 1;
        }
    }

    return 0;
}

static inline int try_schedule_write_data(int chan_id, uint64_t cur_time) {
    int j;

    for (j = 0; j < DIE_PER_CHAN; j++) {
        int die = (rr_die[chan_id] + j) % DIE_PER_CHAN;
        if (die_state[chan_id][die] == DIE_WRITE_DATA_READY) {
            chan[chan_id].state = CHAN_DATA;
            chan[chan_id].time = cur_time + DATA_TIME;
            chan[chan_id].act = list_slot[chan_id][die].act;
            chan[chan_id].op = OP_WRITE;
            chan[chan_id].die = die;
            die_state[chan_id][die] = DIE_WRITE_DATA;
            rr_die[chan_id] = (die + 1) % DIE_PER_CHAN;
            return 1;
        }
    }

    return 0;
}

void perf_init(void) {
    int i, j;

    srand((unsigned)time(NULL));
    if (READ_RATIO + WRITE_RATIO + ERASE_RATIO <= 0) {
        printf("Error ratios! total ratio must be positive\n");
        exit(1);
    }

    for (i = 0; i < CHAN_NUM; i++) {
        for (j = 0; j < DIE_PER_CHAN; j++) {
            list_slot[i][j].head = 0;
            list_slot[i][j].tail = 0;
            list_slot[i][j].act = 0xFFF;
            list_slot[i][j].empty = 1;
            list_slot[i][j].time = 0;
            die_state[i][j] = DIE_IDLE;
        }
    }

    for (i = 0; i < CHAN_NUM; i++) {
        chan[i].state = CHAN_IDLE;
        chan[i].act = 0xFFF;
        chan[i].op = OP_READ;
        chan[i].die = -1;
        rr_die[i] = 0;
    }

    for (i = 0; i < CMD_CNT; i++) {
        map[i] = 0;
        cmd_op[i] = OP_READ;
    }
}

int perf_gen_cmd(int tmp_cmd_cnt, int *inflight_cmds) {
    int i;
    int act;
    int tmp;

    if (*inflight_cmds >= tmp_cmd_cnt) {
        return 0;
    }

    for (i = 0; i < CMD_CNT; i++) {
        if (map[i] == 0) {
            act = i;
            cmd_op[act] = select_op();
            tmp = rand() % DIE_NUM;
            update_list_head(tmp % CHAN_NUM, tmp / CHAN_NUM, act);
            map[i] = 1;
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
    int tmp_cmd_cnt = 0;
    int inflight_cmds = 0;
    uint64_t start_time = 0;
    uint64_t end_time = 0;

    while (1) {
        if (total_cmd > ELEMENT) {
            break;
        }
        if (CMD_CNT >= 512) {
            tmp_cmd_cnt = (int)(CMD_CNT * 0.75);
        } else {
            tmp_cmd_cnt = (int)(CMD_CNT * 0.8);
        }

        if (total_cmd > (uint64_t)tmp_cmd_cnt && start_time == 0) {
            start_time = get_time_us();
        }

        perf_gen_cmd(tmp_cmd_cnt, &inflight_cmds);

        for (i = 0; i < CHAN_NUM; i++) {
            switch (chan[i].state) {
                case CHAN_IDLE:
                    cur_time = get_time_us();
                    complete_wait_ops(i, cur_time, &inflight_cmds, &total_cmd);

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
                    if (cur_time >= chan[i].time) {
                        if (chan[i].op == OP_READ) {
                            die_state[i][chan[i].die] = DIE_READ_WAIT;
                            list_slot[i][chan[i].die].time =
                                cur_time + TREAD;
                            chan[i].state = CHAN_IDLE;
                            chan[i].act = 0xFFF;
                            chan[i].op = OP_READ;
                            chan[i].die = -1;
                        } else if (chan[i].op == OP_WRITE) {
                            die_state[i][chan[i].die] = DIE_WRITE_DATA_READY;
                            chan[i].state = CHAN_IDLE;
                            chan[i].act = 0xFFF;
                            chan[i].op = OP_READ;
                            chan[i].die = -1;
                        } else {
                            die_state[i][chan[i].die] = DIE_ERASE_WAIT;
                            list_slot[i][chan[i].die].time =
                                cur_time + TERASE;
                            chan[i].state = CHAN_IDLE;
                            chan[i].act = 0xFFF;
                            chan[i].op = OP_READ;
                            chan[i].die = -1;
                        }
                    }
                    break;
                case CHAN_DATA:
                    cur_time = get_time_us();
                    if (cur_time >= chan[i].time) {
                        if (chan[i].op == OP_READ) {
                            map[chan[i].act] = 0;
                            inflight_cmds--;
                            total_cmd++;
                            list_slot[i][chan[i].die].act = 0xFFF;
                            die_state[i][chan[i].die] = DIE_IDLE;
                        } else if (chan[i].op == OP_WRITE) {
                            die_state[i][chan[i].die] = DIE_WRITE_WAIT;
                            list_slot[i][chan[i].die].time =
                                cur_time + TPROG;
                        }
                        chan[i].act = 0xFFF;
                        chan[i].state = CHAN_IDLE;
                        chan[i].op = OP_READ;
                        chan[i].die = -1;
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
        stats->start_time = start_time;
        stats->end_time = end_time;
    }
}

double perf_calc_iops(const perf_stats_t *stats) {
    int xor_ratio = (DIE_NUM > 64) ? 64 : DIE_NUM;

    if (!stats) {
        return 0.0;
    }

    return (double)(stats->total_cmd - CMD_CNT) * 1000000 /
           ((stats->end_time - stats->start_time) / TIME_SCALE) *
           ((double)xor_ratio - 1) / (double)xor_ratio;
}
