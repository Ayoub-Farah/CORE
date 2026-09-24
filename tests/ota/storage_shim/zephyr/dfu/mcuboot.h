#pragma once
#define BOOT_SWAP_TYPE_NONE 1
#define BOOT_SWAP_TYPE_TEST 2
#define BOOT_SWAP_TYPE_REVERT 4
bool boot_is_img_confirmed();
int mcuboot_swap_type();
