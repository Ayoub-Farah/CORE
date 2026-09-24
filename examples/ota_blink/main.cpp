/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include "OtaService.h"

/* The service owns the LED. Both versions keep the exact same receiver and
 * coordinator capabilities. No power outputs or critical task are started. */
int main(void)
{
    for (;;) k_sleep(K_SECONDS(1));
    return 0;
}
