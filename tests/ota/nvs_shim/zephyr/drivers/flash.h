#pragma once
#include <stddef.h>
#include <zephyr/device.h>
struct flash_pages_info { size_t size; };
int flash_get_page_info_by_offs(const struct device *, size_t, struct flash_pages_info *);
