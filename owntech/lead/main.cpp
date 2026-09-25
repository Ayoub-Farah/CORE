/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include <zephyr/kernel.h>

/* Dedicated coordinator: no power-control task or output command.
 * Core still inhibits and checks actual power output state during maintenance. */
extern "C" int owntech_ota_enter_maintenance(void) { return 0; }
extern "C" int owntech_ota_check_health(void) { return 0; }

int main(void)
{
    while (true) k_sleep(K_FOREVER);
    return 0;
}
