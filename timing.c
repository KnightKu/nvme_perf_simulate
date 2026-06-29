#include "timing.h"

#include <string.h>

static int is_power_of_two(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static double channel_payload_mbps(uint64_t payload_bytes, uint64_t data_time) {
    if (data_time == 0) {
        return 0.0;
    }
    return (double)payload_bytes * (double)TIME_SCALE / (double)data_time *
           1000000.0 / 1048576.0;
}

int perf_config_validate(const perf_config_t *cfg) {
    if (!cfg) {
        return -1;
    }

    if (cfg->chan_num <= 0 || cfg->die_num <= 0 || cfg->qd <= 0 ||
        cfg->iwl_slot <= 0 || cfg->chan_speed <= 0 || cfg->cmd_size <= 0 ||
        cfg->page_size <= 0 || cfg->plane <= 0) {
        return -1;
    }
    if (cfg->die_num % cfg->chan_num != 0) {
        return -1;
    }
    if (cfg->read_ratio + cfg->write_ratio + cfg->erase_ratio <= 0) {
        return -1;
    }
    if (cfg->nand_type != 1 && cfg->nand_type != 3 && cfg->nand_type != 4) {
        return -1;
    }
    if (cfg->tprog_eff <= 0) {
        return -1;
    }
    if (cfg->io_pattern != PERF_IO_PATTERN_RANDOM &&
        cfg->io_pattern != PERF_IO_PATTERN_SEQUENTIAL) {
        return -1;
    }
    if (cfg->block_size < 0) {
        return -1;
    }
    if (cfg->block_size > 0 && cfg->block_size % cfg->page_size != 0) {
        return -1;
    }

    {
        uint64_t total_planes =
            (uint64_t)cfg->die_num * (uint64_t)cfg->plane;
        if (total_planes > (uint64_t)cfg->iwl_slot) {
            int per_die = cfg->iwl_slot / cfg->die_num;
            if (cfg->iwl_slot % cfg->die_num != 0 ||
                !is_power_of_two(per_die) || per_die <= 0) {
                return -1;
            }
        }
    }

    return 0;
}

int perf_derived_init(const perf_config_t *cfg, perf_derived_t *d) {
    uint64_t read_wire;
    uint64_t write_wire;
    int xor_ratio;

    if (!cfg || !d) {
        return -1;
    }

    memset(d, 0, sizeof(*d));

    d->die_per_chan = cfg->die_num / cfg->chan_num;
    {
        uint64_t total_planes =
            (uint64_t)cfg->die_num * (uint64_t)cfg->plane;
        if (total_planes > (uint64_t)cfg->iwl_slot) {
            int per_die = cfg->iwl_slot / cfg->die_num;
            d->max_planes_per_die =
                (per_die > cfg->plane) ? cfg->plane : per_die;
        } else {
            d->max_planes_per_die = cfg->plane;
        }
    }

    if (cfg->block_size > 0) {
        d->pages_per_block = cfg->block_size / cfg->page_size;
        d->read_bytes_per_page = cfg->page_size;
        d->write_bytes_per_page = cfg->page_size;
    } else {
        d->pages_per_block = 1;
        d->read_bytes_per_page = cfg->cmd_size;
        d->write_bytes_per_page = cfg->page_size;
    }
    d->read_bytes_per_cmd =
        cfg->block_size > 0 ? cfg->block_size : cfg->cmd_size;
    d->write_bytes_per_cmd =
        cfg->block_size > 0 ? cfg->block_size : cfg->page_size;

    if (cfg->sca) {
        d->cmd_time = (uint64_t)(cfg->cmd_overhead_sca * TIME_SCALE);
    } else {
        d->cmd_time = (uint64_t)(cfg->cmd_overhead * TIME_SCALE);
    }
    if (cfg->cmd_size == 4096) {
        d->tread = (uint64_t)cfg->tr_fast * TIME_SCALE;
    } else {
        d->tread = (uint64_t)cfg->tR * TIME_SCALE;
    }
    d->tprog =
        (uint64_t)((uint64_t)cfg->tprog_eff * (uint64_t)cfg->nand_type) *
        TIME_SCALE;
    d->terase = (uint64_t)cfg->tERASE * TIME_SCALE;

    read_wire = (uint64_t)d->read_bytes_per_page + (uint64_t)cfg->ecc_parity_size;
    write_wire =
        (uint64_t)d->write_bytes_per_page + (uint64_t)cfg->page_parity_size;

    d->data_time_read_page =
        read_wire * TIME_SCALE / (uint64_t)cfg->chan_speed;
    d->data_time_write_page =
        write_wire * TIME_SCALE / (uint64_t)cfg->chan_speed;
    d->read_ceiling_mbps =
        (double)cfg->chan_num *
        channel_payload_mbps(read_wire, d->data_time_read_page);
    d->write_ceiling_mbps =
        (double)cfg->chan_num *
        channel_payload_mbps(write_wire, d->data_time_write_page);
    d->read_ceiling_host_mbps =
        (double)cfg->chan_num *
        channel_payload_mbps((uint64_t)d->read_bytes_per_page,
                             d->data_time_read_page);
    d->write_ceiling_host_mbps =
        (double)cfg->chan_num *
        channel_payload_mbps((uint64_t)d->write_bytes_per_page,
                             d->data_time_write_page);

    xor_ratio = (cfg->die_num > 64) ? 64 : cfg->die_num;
    d->xor_factor = ((double)xor_ratio - 1) / (double)xor_ratio;
    d->read_ceiling_xor_mbps = d->read_ceiling_mbps * d->xor_factor;
    d->write_ceiling_xor_mbps = d->write_ceiling_mbps * d->xor_factor;

    return 0;
}
