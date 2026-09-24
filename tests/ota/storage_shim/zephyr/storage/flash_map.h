#pragma once
#include <stddef.h>
#include <stdint.h>
#define slot0_partition 0
#define slot1_partition 1
#define FIXED_PARTITION_ID(name) (name)
#define FIXED_PARTITION_SIZE(name) 1024U
struct flash_area { unsigned id; uint32_t fa_size; };
int flash_area_open(unsigned, const flash_area **);
void flash_area_close(const flash_area *);
int flash_area_read(const flash_area *, uint32_t, void *, size_t);
int flash_area_erase(const flash_area *, uint32_t, size_t);
