#ifndef CMD_POOL_H
#define CMD_POOL_H

#include <stdint.h>

int cmd_pool_init(int capacity);
void cmd_pool_cleanup(void);
int cmd_pool_push(int act);
int cmd_pool_pop(void);
int cmd_pool_empty(void);
int cmd_pool_is_full(void);

#endif
