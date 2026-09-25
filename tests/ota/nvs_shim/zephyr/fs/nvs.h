#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
struct nvs_fs {
    size_t offset;
    const struct device *flash_device;
    size_t sector_size = 0;
    unsigned sector_count = 0;
};
int nvs_mount(struct nvs_fs *);
int nvs_read(struct nvs_fs *, uint16_t, void *, size_t);
int nvs_write(struct nvs_fs *, uint16_t, const void *, size_t);
int nvs_clear(struct nvs_fs *);
int nvs_calc_free_space(struct nvs_fs *);
