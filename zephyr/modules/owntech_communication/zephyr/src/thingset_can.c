/*
 * Copyright (c) 2024-present LAAS-CNRS
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1
 */

/*
 * @date   2024
 * @author Martin Jäger <martin@libre.solar>
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

#include <thingset.h>
#include <thingset/can.h>
#include <thingset/sdk.h>

LOG_MODULE_REGISTER(ts_can, CONFIG_THINGSET_SDK_LOG_LEVEL);

extern struct thingset_context ts;

/* Commands own their bytes until workqueue processing. Never import arbitrary
 * high application IDs (including DFUCampaign) through the Control channel. */
#ifdef CONFIG_OWNTECH_OTA
#include "OtaService.h"
#endif
struct control_item { uint8_t buf[4 + CAN_MAX_DLEN]; size_t len; };
K_MSGQ_DEFINE(control_queue, sizeof(struct control_item), 8, 4);
static void can_control_work_handler(struct k_work *work)
{
    struct control_item item;
    while (!k_msgq_get(&control_queue, &item, K_NO_WAIT)) {
#ifdef CONFIG_OWNTECH_OTA
        if (ota_safety_inhibited()) continue;
#endif
        thingset_import_data(&ts, item.buf, item.len, THINGSET_WRITE_MASK,
                             THINGSET_BIN_IDS_VALUES);
    }
}
K_WORK_DEFINE(control_work, can_control_work_handler);
void can_control_rx_handler(uint16_t id, const uint8_t *value, size_t len, uint8_t source)
{
    ARG_UNUSED(source);
    if ((id != 0x8001 && id != 0x8002) || len > CAN_MAX_DLEN || !len) return;
#ifdef CONFIG_OWNTECH_OTA
    if (ota_safety_inhibited()) return;
#endif
    struct control_item item = {.buf = {0xA1, 0x19, id >> 8, id & 255}, .len = 4 + len};
    memcpy(item.buf + 4, value, len);
    if (!k_msgq_put(&control_queue, &item, K_NO_WAIT)) k_work_submit(&control_work);
}
static int can_control_init(void)
{
    return thingset_can_set_item_rx_callback(can_control_rx_handler);
}
SYS_INIT(can_control_init, APPLICATION, THINGSET_INIT_PRIORITY_DEFAULT);
