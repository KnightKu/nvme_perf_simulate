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
#define QD              (512)           // How many ACT cmds used
#define CHAN_NUM        (16)            // How many channels
#define DIE_NUM         (128)           // Total die num
#define PLANE           (4)             // How many planes in one die (Have not adapt 6 plane scenario)
#define IWL_SLOT        (256)           // Max parallelism for IWL read (e.g., 256 for QLTC, 512 for Aspen, but it reduces with channel num)
/***************************************************************************************************/

// No need to change in genereal
#define TIME_SCALE      (1000ULL)
#define CMD_TIME        (CMD_OVERHEAD * TIME_SCALE)
#define TREAD           (tR * TIME_SCALE)
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
    uint32_t list_info;
    uint64_t time;
} chan_t;

int map[CMD_CNT];
list_t list_slot[CHAN_NUM][DIE_PER_CHAN][PLANE];
chan_t chan[CHAN_NUM];

uint64_t total_active;
uint64_t total_loops;

inline void update_list_head(int plane, int plane_share, int act) {
    int die = plane / PLANE;
    int chan = die % CHAN_NUM;
    int die_in_chan = die / CHAN_NUM;
    int list_dest = (plane % PLANE) / plane_share;
    list_t *list = &(list_slot[chan][die_in_chan][list_dest]);
    list->list[list->head] = act;
    list->head++;
    if (list->head == SLOT) {
        list->head = 0;
    }
    if (list->head != list->tail) {
        list->empty = 0;
    }
}

inline void update_list_time(uint64_t time, uint64_t add_time, list_t *list) {
    list->time = time + add_time;
}

inline void update_list_tail(list_t *list) {
    list->act = list->list[list->tail];
    list->tail++;
    if (list->tail == SLOT) {
        list->tail = 0;
    }
    if (list->head == list->tail) {
        list->empty = 1;
    }
}

int main() {
    int i, j, k;
    long cnt = 0;
    uint64_t IOPS;
    uint64_t list_info;
    uint64_t cur_time;
    uint64_t total_cmd;
    int plane_share = 1;
    int plane;
    int act;
    int cmd_cnt = 0;
    int tmp_cmd_cnt = 0;
    int can_break;
    int active, print;
    int tmp;
    uint64_t start_time;
    uint64_t end_time;
    int xor_ratio;

    srand((unsigned)time(NULL));
    total_active = 0;
    total_loops = 0;

    if (PLANE * DIE_NUM > SLOT) {
        plane_share = PLANE * DIE_NUM / SLOT;
        if (plane_share != 2 && plane_share != 4) {
            printf("Error planes! plane_share = %d\n", plane_share);
            exit(1);
        }
    }

    for (i = 0; i < CHAN_NUM; i++) {
        for (j = 0; j < DIE_PER_CHAN; j++) {
            for (k = 0; k < PLANE; k++) {
                list_slot[i][j][k].head = 0;
                list_slot[i][j][k].tail = 0;
                list_slot[i][j][k].act = 0xFFF;
                list_slot[i][j][k].empty = 1;
                list_slot[i][j][k].time = 0;
            }
        }
    }

    for (i = 0; i < CHAN_NUM; i++) {
        chan[i].state = CHAN_IDLE;
    }

    for (i = 0; i < CMD_CNT; i++) {
        map[i] = 0;
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

        if (total_cmd > tmp_cmd_cnt && start_time == 0) {
            start_time = get_time_us();
        }

        if (cmd_cnt < tmp_cmd_cnt) {
            for (i = 0; i < CMD_CNT; i++) {
                if (map[i] == 0) {
                    act = i;
                    plane = rand() % (PLANE * DIE_NUM);
                    update_list_head(plane, plane_share, act);
                    map[i] = 1;
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
                        for (k = 0; k < PLANE / plane_share; k++) {
                            if (list_slot[i][j][k].empty == 0 &&
                                list_slot[i][j][k].act == 0xFFF) {
                                chan[i].state = CHAN_CMD;
                                chan[i].time = cur_time + CMD_TIME;
                                tmp = list_slot[i][j][k].tail;
                                chan[i].act = list_slot[i][j][k].list[tmp];
                                chan[i].list_info = (j << 16) | k;
                                update_list_tail(&list_slot[i][j][k]);
                                can_break = 1;
                                break;
                            }
                        }

                        if (can_break) {
                            break;
                        }
                    }

                    if (can_break == 0) {
                        for (j = 0; j < DIE_PER_CHAN; j++) {
                            for (k = 0; k < PLANE / plane_share; k++) {
                                if (list_slot[i][j][k].act != 0xFFF &&
                                    cur_time >= list_slot[i][j][k].time) {
                                    chan[i].state = CHAN_DATA;
                                    chan[i].time = cur_time + DATA_TIME;
                                    chan[i].act = list_slot[i][j][k].act;
                                    chan[i].list_info = (j << 16) | k;
                                    can_break = 1;
                                    break;
                                }
                            }
                            if (can_break) {
                                break;
                            }
                        }
                    }
                    break;
                case CHAN_CMD:
                    cur_time = get_time_us();
                    if (cur_time >= chan[i].time) {
                        chan[i].state = CHAN_IDLE;
                        list_info = chan[i].list_info;
                        list_slot[i][list_info >> 16][list_info & 0xFFFF].act =
                            chan[i].act;
                        chan[i].act = 0xFFF;
                        chan[i].list_info = 0xFFFFFFFF;
                        update_list_time(
                            cur_time,
                            TREAD,
                            &list_slot[i][list_info >> 16][list_info & 0xFFFF]);
                    }
                    break;
                case CHAN_DATA:
                    if (get_time_us() >= chan[i].time) {
                        map[chan[i].act] = 0;
                        chan[i].act = 0xFFF;
                        chan[i].state = CHAN_IDLE;

                        list_info = chan[i].list_info;
                        list_slot[i][list_info >> 16][list_info & 0xFFFF].act =
                            0xFFF;
                        chan[i].list_info = 0xFFFFFFFF;

                        total_cmd++;
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
