/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>

/* Dedicated coordinator: no power-control task or output command.
 * Core starts CAN/OTA and checks actual outputs without application callbacks. */

int main(void)
{
    while (true) k_sleep(K_FOREVER);
    return 0;
}
