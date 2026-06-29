#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *perf_default_config_path(void) {
    return "nand.conf";
}

void perf_config_defaults(perf_config_t *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->cmd_overhead = 1.7;
    cfg->cmd_overhead_sca = 0.7;
    cfg->sca = 1;
    cfg->chan_speed = 3200;
    cfg->cmd_size = 4096;
    cfg->block_size = 0;
    cfg->ecc_parity_size = 600;
    cfg->page_size = 16384;
    cfg->page_parity_size = 1952;
    cfg->tr_fast = 40;
    cfg->tR = 40;
    cfg->tprog_eff = 420;
    cfg->nand_type = 3;
    cfg->tERASE = 5000;
    cfg->qd = 1024;
    cfg->chan_num = 16;
    cfg->die_num = 128;
    cfg->plane = 4;
    cfg->iwl_slot = 512;
    cfg->read_ratio = 100;
    cfg->write_ratio = 0;
    cfg->erase_ratio = 0;
    cfg->io_pattern = PERF_IO_PATTERN_RANDOM;
    cfg->element = 16384;
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
    } else if (strcmp(key, "block_size") == 0) {
        cfg->block_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "ecc_parity_size") == 0 ||
               strcmp(key, "ecc_parity") == 0) {
        cfg->ecc_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_size") == 0) {
        cfg->page_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "page_parity_size") == 0) {
        cfg->page_parity_size = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr_fast") == 0) {
        cfg->tr_fast = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tr") == 0) {
        cfg->tR = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "tprog_eff") == 0 || strcmp(key, "tprog") == 0) {
        cfg->tprog_eff = (int)strtol(value, &end, 10);
    } else if (strcmp(key, "nand_type") == 0) {
        cfg->nand_type = (int)strtol(value, &end, 10);
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
    } else if (strcmp(key, "io_pattern") == 0) {
        if (strcmp(value, "random") == 0) {
            cfg->io_pattern = PERF_IO_PATTERN_RANDOM;
            end = (char *)(value + strlen(value));
        } else if (strcmp(value, "sequential") == 0 ||
                   strcmp(value, "seq") == 0) {
            cfg->io_pattern = PERF_IO_PATTERN_SEQUENTIAL;
            end = (char *)(value + strlen(value));
        } else {
            return -1;
        }
    } else if (strcmp(key, "element") == 0) {
        cfg->element = (uint64_t)strtoull(value, &end, 10);
    } else if (strcmp(key, "stripe_mode") == 0) {
        /* legacy: ignored on l0-base (stripe removed) */
        end = (char *)(value + strlen(value));
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

static void strip_inline_comment(char *value) {
    char *p;

    if (!value) {
        return;
    }
    p = strchr(value, '#');
    if (p) {
        *p = '\0';
    }
    p = strstr(value, "//");
    if (p) {
        *p = '\0';
    }
    trim_space(value);
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
        strip_inline_comment(value);

        rc = set_config_value(cfg, key, value);
        if (rc < 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}
