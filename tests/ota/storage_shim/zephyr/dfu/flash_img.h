#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zephyr/storage/flash_map.h>
#define CONFIG_IMG_BLOCK_BUF_SIZE 512
struct flash_img_context { uint8_t buf[512]; const flash_area *flash_area; size_t written, pending; };
struct flash_img_check { const uint8_t *match; size_t clen; };
int flash_img_init_id(flash_img_context *, uint8_t);
int flash_img_buffered_write(flash_img_context *, const uint8_t *, size_t, bool);
size_t flash_img_bytes_written(flash_img_context *);
int flash_img_check(flash_img_context *, const struct flash_img_check *, uint8_t);
