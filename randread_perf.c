#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

/***************************** Usage ***************************/
// 1. Modify parameters in the following part.
// 2. g++ randread_perf.c
// 3. ./a.out (Take several seconds to finish)
// Notice it only evaluates performance on (channel + NAND) side!!!
// Need to consider internal bus/resource and PCIe limitation for product performance!!!
/***************************************************************/

/******************************* Parameters need to clarity ****************************************/
#define CMD_OVERHEAD    ((double)1.7)   // Equivalent time in micro-seconds used for one cmd
#define CHAN_SPEED      (2400)          // MT/s
#define ECC_PARITY      (600)           // How many bytes used for LDPC parity in one 4KiB codeword
#define tR              (40)            // tR in micro-seconds
#define tPROG           (800)           // tPROG in micro-seconds
#define tERASE          (3000)          // tERASE in micro-seconds
#define QD              (512)           // How many ACT cmds used
#define CHAN_NUM        (16)            // How many channels
#define DIE_NUM         (128)           // Total die num
#define PLANE           (4)             // How many planes in one die (Have not adapt 6 plane scenario)
#define IWL_SLOT        (256)           // Max parallelism for IWL read (e.g., 256 for QLTC, 512 for Aspen, but it reduces with channel num)
#define READ_RATIO      (100)           // Read ratio percentage
#define WRITE_RATIO     (0)             // Write ratio percentage
#define ERASE_RATIO     (0)             // Erase ratio percentage
/***************************************************************************************************/

// No need to change in genereal
#define TIME_SCALE      (1000ULL)
#define CMD_TIME        (CMD_OVERHEAD * TIME_SCALE)
#define TREAD           (tR * TIME_SCALE)
#define TPROG           (tPROG * TIME_SCALE)
#define TERASE          (tERASE * TIME_SCALE)
#define DATA_TIME       ((4096 + ECC_PARITY) * TIME_SCALE / CHAN_SPEED)
#define SLOT            (IWL_SLOT)
#define ELEMENT         (1 * 32 * 1024)
#define SLOT_PER_DIE    (SLOT / DIE_NUM)
#define CMD_CNT         (QD)
#define DIE_PER_CHAN    (DIE_NUM / CHAN_NUM)

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
    DIE_WRITE_DATA,
    DIE_WRITE_WAIT,
    DIE_ERASE_WAIT,
};

struct timeval tv;

inline uint64_t get_time_us() {
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

int map[CMD_CNT];
int cmd_op[CMD_CNT];
list_t list_slot[CHAN_NUM][DIE_PER_CHAN];
int die_state[CHAN_NUM][DIE_PER_CHAN];
int rr_die[CHAN_NUM];
chan_t chan[CHAN_NUM];

uint64_t total_active;
uint64_t total_loops;

inline void update_list_head(int chan, int die_in_chan, int act) {
    list_t *list = &(list_slot[chan][die_in_chan]);
    list->list[list->head] = act;
    list->head++;
    if (list->head == SLOT) {
        list->head = 0;
    }
    if (list->head != list->tail) {
        list->empty = 0;
    }
}

inline int pop_list(list_t *list) {
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

inline int select_op() {
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

int main() {
    int i, j;
    uint64_t cur_time;
    uint64_t total_cmd;
    int act;
    int tmp_cmd_cnt = 0;
    int can_break;
    int tmp;
    uint64_t start_time;
    uint64_t end_time;
    int xor_ratio;
    int inflight_cmds = 0;

    srand((unsigned)time(NULL));
    total_active = 0;
    total_loops = 0;
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

    total_cmd = 0;
    start_time = 0;
    while (1) {
        if (total_cmd > ELEMENT) {
            // printf("Enough cmds have been executed!\n");
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

        if (inflight_cmds < tmp_cmd_cnt) {
            for (i = 0; i < CMD_CNT; i++) {
                if (map[i] == 0) {
                    act = i;
                    cmd_op[act] = select_op();
                    tmp = rand() % DIE_NUM;
                    update_list_head(tmp % CHAN_NUM, tmp / CHAN_NUM, act);
                    map[i] = 1;
                    inflight_cmds++;
                    break;
                }
            }
        }

        for (i = 0; i < CHAN_NUM; i++) {
            switch (chan[i].state) {
                case CHAN_IDLE:
                    can_break = 0;
                    cur_time = get_time_us();
                    for (j = 0; j < DIE_PER_CHAN; j++) {
                        if ((die_state[i][j] == DIE_WRITE_WAIT ||
                             die_state[i][j] == DIE_ERASE_WAIT) &&
                            list_slot[i][j].act != 0xFFF &&
                            cur_time >= list_slot[i][j].time) {
                            act = list_slot[i][j].act;
                            list_slot[i][j].act = 0xFFF;
                            die_state[i][j] = DIE_IDLE;
                            map[act] = 0;
                            inflight_cmds--;
                            total_cmd++;
                        }
                    }

                    for (j = 0; j < DIE_PER_CHAN; j++) {
                        int die = (rr_die[i] + j) % DIE_PER_CHAN;
                        if (die_state[i][die] == DIE_IDLE &&
                            list_slot[i][die].empty == 0) {
                            act = pop_list(&list_slot[i][die]);
                            list_slot[i][die].act = act;
                            die_state[i][die] = DIE_CMD;
                            chan[i].state = CHAN_CMD;
                            chan[i].time = cur_time + CMD_TIME;
                            chan[i].act = act;
                            chan[i].op = cmd_op[act];
                            chan[i].die = die;
                            rr_die[i] = (die + 1) % DIE_PER_CHAN;
                            can_break = 1;
                            break;
                        }
                    }

                    if (can_break == 0) {
                        for (j = 0; j < DIE_PER_CHAN; j++) {
                            int die = (rr_die[i] + j) % DIE_PER_CHAN;
                            if (die_state[i][die] == DIE_READ_WAIT &&
                                cur_time >= list_slot[i][die].time) {
                                chan[i].state = CHAN_DATA;
                                chan[i].time = cur_time + DATA_TIME;
                                chan[i].act = list_slot[i][die].act;
                                chan[i].op = OP_READ;
                                chan[i].die = die;
                                die_state[i][die] = DIE_READ_DATA;
                                rr_die[i] = (die + 1) % DIE_PER_CHAN;
                                can_break = 1;
                                break;
                            }
                        }
                    }
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
                            die_state[i][chan[i].die] = DIE_WRITE_DATA;
                            chan[i].state = CHAN_DATA;
                            chan[i].time = cur_time + DATA_TIME;
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
    xor_ratio = (DIE_NUM > 64) ? 64 : DIE_NUM;
    printf("Performance = %.2f IOPS\n",
           (double)(total_cmd - CMD_CNT) * 1000000 /
               ((end_time - start_time) / TIME_SCALE) *
               ((double)xor_ratio - 1) / (double)xor_ratio);

    return 0;
}
