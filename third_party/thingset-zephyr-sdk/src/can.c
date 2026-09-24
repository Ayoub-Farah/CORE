/*
 * Copyright (c) The ThingSet Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/random/random.h>

#include <thingset.h>
#include <thingset/can.h>
#include <thingset/sdk.h>
#include <thingset/storage.h>

LOG_MODULE_REGISTER(thingset_can, CONFIG_THINGSET_SDK_LOG_LEVEL);

extern uint8_t eui64[8];

#define EVENT_ADDRESS_CLAIM_MSG_SENT    BIT(1)
#define EVENT_ADDRESS_CLAIMING_FINISHED BIT(2)
#define EVENT_ADDRESS_ALREADY_USED      BIT(3)
#define EVENT_ADDRESS_CLAIM_TIMED_OUT   BIT(4)

#ifdef CONFIG_THINGSET_CAN_ITEM_RX
static const struct can_filter sf_report_filter = {
    .id = THINGSET_CAN_TYPE_SF_REPORT,
    .mask = THINGSET_CAN_TYPE_MASK,
    .flags = CAN_FILTER_IDE,
};
#endif /* CONFIG_THINGSET_CAN_ITEM_RX */

#ifdef CONFIG_THINGSET_CAN_REPORT_RX
static const struct can_filter mf_report_filter = {
    .id = THINGSET_CAN_TYPE_MF_REPORT,
    .mask = THINGSET_CAN_TYPE_MASK,
    .flags = CAN_FILTER_IDE,
};
#endif /* CONFIG_THINGSET_CAN_REPORT_RX */

static const struct isotp_fast_opts fc_opts = {
    .bs = 8, /* block size */
    .stmin = CONFIG_THINGSET_CAN_FRAME_SEPARATION_TIME,
    .addressing_mode = ISOTP_FAST_ADDRESSING_MODE_CUSTOM,
#ifdef CONFIG_CAN_FD_MODE
    .flags = ISOTP_MSG_FDF,
#endif
};

#ifdef CONFIG_THINGSET_CAN_REPORT_RX
/* Slots also remain owned while the application copies a completed report. */
struct thingset_can_rx_context
{
    struct thingset_can *owner;
    int64_t deadline;
    uint8_t src_addr;
    uint8_t route;
    uint8_t msg;
    uint8_t seq;
    bool used;
    bool dispatching;
    size_t len;
    uint8_t data[CONFIG_THINGSET_CAN_REPORT_RX_BUFFER_SIZE];
};

static struct thingset_can_rx_context rx_slots[CONFIG_THINGSET_CAN_REPORT_RX_NUM_BUFFERS];
static struct k_spinlock rx_slots_lock;
static int64_t rx_expiry_deadline;
static void thingset_can_report_expiry(struct k_timer *timer);
K_TIMER_DEFINE(thingset_can_report_expiry_timer, thingset_can_report_expiry, NULL);

/* Caller holds rx_slots_lock. No periodic timer runs when the pool is idle.
 * Completed slots stay owned by the application callback but need no timeout.
 */
static void thingset_can_schedule_report_expiry_locked(int64_t now)
{
    int64_t next = 0;
    for (size_t i = 0; i < ARRAY_SIZE(rx_slots); i++) {
        struct thingset_can_rx_context *slot = &rx_slots[i];
        if (slot->used && !slot->dispatching && (next == 0 || slot->deadline < next)) {
            next = slot->deadline;
        }
    }
    if (next == rx_expiry_deadline) {
        return;
    }
    rx_expiry_deadline = next;
    if (next == 0) {
        k_timer_stop(&thingset_can_report_expiry_timer);
    }
    else {
        k_timer_start(&thingset_can_report_expiry_timer,
                      K_MSEC(next > now ? next - now : 1), K_NO_WAIT);
    }
}

static void thingset_can_expire_reports_locked(int64_t now)
{
    for (size_t i = 0; i < ARRAY_SIZE(rx_slots); i++) {
        struct thingset_can_rx_context *slot = &rx_slots[i];
        if (slot->used && !slot->dispatching && now >= slot->deadline) {
            atomic_inc(&slot->owner->report_rx_expired);
            slot->used = false;
        }
    }
}

static void thingset_can_report_expiry(struct k_timer *timer)
{
    k_spinlock_key_t key = k_spin_lock(&rx_slots_lock);
    int64_t now = k_uptime_get();
    rx_expiry_deadline = 0;
    thingset_can_expire_reports_locked(now);
    thingset_can_schedule_report_expiry_locked(now);
    k_spin_unlock(&rx_slots_lock, key);
}
#endif /* CONFIG_THINGSET_CAN_REPORT_RX */

static void thingset_can_addr_claim_tx_cb(const struct device *dev, int error, void *user_data)
{
    struct thingset_can *ts_can = user_data;

    if (error == 0) {
        k_event_post(&ts_can->events, EVENT_ADDRESS_CLAIM_MSG_SENT);
    }
    else {
        LOG_ERR("Address claim failed with %d", error);
    }
}

static void thingset_can_addr_discovery_tx_cb(const struct device *dev, int error, void *user_data)
{
    if (error != 0) {
        LOG_ERR("Address discovery failed with %d", error);
    }
}

static void thingset_can_addr_claim_tx_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct thingset_can *ts_can = CONTAINER_OF(dwork, struct thingset_can, addr_claim_work);

    struct can_frame tx_frame = {
        .id = THINGSET_CAN_TYPE_NETWORK | THINGSET_CAN_PRIO_NETWORK_MGMT
#ifdef CONFIG_THINGSET_CAN_ROUTING_BUSES
              | THINGSET_CAN_TARGET_BUS_SET(ts_can->route)
              | THINGSET_CAN_SOURCE_BUS_SET(ts_can->route)
#else /* CONFIG_THINGSET_CAN_ROUTING_BRIDGES */
              | THINGSET_CAN_BRIDGE_SET(ts_can->route)
#endif
              | THINGSET_CAN_TARGET_SET(THINGSET_CAN_ADDR_BROADCAST)
              | THINGSET_CAN_SOURCE_SET(ts_can->node_addr),
        .flags = CAN_FRAME_IDE,
        .dlc = sizeof(eui64),
    };
    memcpy(tx_frame.data, eui64, sizeof(eui64));

    int err = can_send(ts_can->dev, &tx_frame, K_MSEC(100), thingset_can_addr_claim_tx_cb, ts_can);
    if (err != 0) {
        LOG_ERR("Address claim failed with %d", err);
    }
}

static void thingset_can_addr_discovery_rx_cb(const struct device *dev, struct can_frame *frame,
                                              void *user_data)
{
    struct thingset_can *ts_can = user_data;

    if (!(frame->flags & CAN_FRAME_IDE) || (frame->flags & CAN_FRAME_RTR) || frame->dlc != 0) {
        return;
    }

    LOG_INF("Received address discovery frame with ID %X (rand %.2X)", frame->id,
            THINGSET_CAN_RAND_GET(frame->id));

    thingset_sdk_reschedule_work(&ts_can->addr_claim_work, K_NO_WAIT);
}

static void thingset_can_addr_claim_rx_cb(const struct device *dev, struct can_frame *frame,
                                          void *user_data)
{
    struct thingset_can *ts_can = user_data;
    uint8_t *data = frame->data;

    if (!(frame->flags & CAN_FRAME_IDE) || (frame->flags & CAN_FRAME_RTR) || frame->dlc != 8
        || THINGSET_CAN_SOURCE_GET(frame->id) < THINGSET_CAN_ADDR_MIN
        || THINGSET_CAN_SOURCE_GET(frame->id) > THINGSET_CAN_ADDR_MAX) {
        return;
    }

    LOG_INF("Received address claim from node 0x%.2X with EUI-64 "
            "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x",
            THINGSET_CAN_SOURCE_GET(frame->id), data[0], data[1], data[2], data[3], data[4],
            data[5], data[6], data[7]);

    uint8_t source_addr = THINGSET_CAN_SOURCE_GET(frame->id);

    if (ts_can->node_addr == source_addr && memcmp(data, eui64, sizeof(eui64)) != 0) {
        k_event_post(&ts_can->events, EVENT_ADDRESS_ALREADY_USED);
    }

    if (ts_can->addr_claim_callback != NULL) {
        ts_can->addr_claim_callback(data, source_addr);
    }

    /* Optimization: store in internal database to exclude from potentially available addresses */
}

#ifdef CONFIG_THINGSET_CAN_ITEM_RX
static void thingset_can_item_rx_cb(const struct device *dev, struct can_frame *frame,
                                    void *user_data)
{
    struct thingset_can *ts_can = user_data;
    uint16_t data_id = THINGSET_CAN_DATA_ID_GET(frame->id);
    uint8_t source_addr = THINGSET_CAN_SOURCE_GET(frame->id);
    size_t length = can_dlc_to_bytes(frame->dlc);
    if (!(frame->flags & CAN_FRAME_IDE) || (frame->flags & CAN_FRAME_RTR)
        || frame->dlc > 15 || length > CAN_MAX_DLEN
        || (!(frame->flags & CAN_FRAME_FDF) && length > 8)
        || source_addr < THINGSET_CAN_ADDR_MIN || source_addr > THINGSET_CAN_ADDR_MAX
        || ts_can->item_rx_cb == NULL) {
        return;
    }
    ts_can->item_rx_cb(data_id, frame->data, length, source_addr);
}
#endif /* CONFIG_THINGSET_CAN_ITEM_RX */

#ifdef CONFIG_THINGSET_CAN_REPORT_RX
static void thingset_can_report_rx_cb(const struct device *dev, struct can_frame *frame,
                                      void *user_data)
{
    struct thingset_can *ts_can = user_data;
    uint8_t source = THINGSET_CAN_SOURCE_GET(frame->id);
    uint8_t route = THINGSET_CAN_BRIDGE_GET(frame->id);
    uint8_t message = THINGSET_CAN_MSG_NO_GET(frame->id);
    uint8_t sequence = THINGSET_CAN_SEQ_NO_GET(frame->id);
    uint32_t type = frame->id & THINGSET_CAN_MF_TYPE_MASK;
    bool first = type == THINGSET_CAN_MF_TYPE_FIRST || type == THINGSET_CAN_MF_TYPE_SINGLE;
    bool last = type == THINGSET_CAN_MF_TYPE_LAST || type == THINGSET_CAN_MF_TYPE_SINGLE;
    size_t length = can_dlc_to_bytes(frame->dlc);
    struct thingset_can_rx_context *slot = NULL;
    struct thingset_can_rx_context *available = NULL;

    if (!(frame->flags & CAN_FRAME_IDE) || (frame->flags & CAN_FRAME_RTR)
        || !THINGSET_CAN_MF_REPORT(frame->id) || frame->dlc > 15 || length == 0
        || length > CAN_MAX_DLEN || (!(frame->flags & CAN_FRAME_FDF) && length > 8)
        || route != ts_can->route
        || source < THINGSET_CAN_ADDR_MIN || source > THINGSET_CAN_ADDR_MAX)
    {
        atomic_inc(&ts_can->report_rx_dropped);
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&rx_slots_lock);
    int64_t now = k_uptime_get();
    thingset_can_expire_reports_locked(now);
    for (size_t i = 0; i < ARRAY_SIZE(rx_slots); i++) {
        if (!rx_slots[i].used) {
            available = &rx_slots[i];
        }
        else if (!rx_slots[i].dispatching && rx_slots[i].owner == ts_can
                 && rx_slots[i].src_addr == source && rx_slots[i].route == route) {
            slot = &rx_slots[i];
        }
    }
    if (first && sequence == 0) {
        if (slot == NULL) {
            slot = available;
        }
        if (slot != NULL) {
            /* A new FIRST replaces an abandoned message from this source. */
            slot->owner = ts_can;
            slot->src_addr = source;
            slot->route = route;
            slot->msg = message;
            slot->seq = 0;
            slot->len = 0;
            slot->used = true;
            slot->dispatching = false;
        }
    }
    if (slot == NULL || (first && sequence != 0) || slot->msg != message
        || slot->seq != sequence)
    {
        if (slot != NULL) {
            slot->used = false;
        }
        atomic_inc(&ts_can->report_rx_dropped);
        thingset_can_schedule_report_expiry_locked(now);
        k_spin_unlock(&rx_slots_lock, key);
        return;
    }
    if (length > sizeof(slot->data) - slot->len) {
        slot->used = false;
        atomic_inc(&ts_can->report_rx_overflow);
        thingset_can_schedule_report_expiry_locked(now);
        k_spin_unlock(&rx_slots_lock, key);
        return;
    }
    memcpy(slot->data + slot->len, frame->data, length);
    slot->len += length;
    slot->seq = (sequence + 1) & 0xF;
    slot->deadline = now + CONFIG_THINGSET_CAN_REPORT_RX_TIMEOUT;
    slot->dispatching = last;
    thingset_can_schedule_report_expiry_locked(now);
    k_spin_unlock(&rx_slots_lock, key);

    if (last) {
        if (ts_can->report_rx_cb != NULL) {
            ts_can->report_rx_cb(slot->data, slot->len, source);
        }
        key = k_spin_lock(&rx_slots_lock);
        slot->used = false;
        slot->dispatching = false;
        k_spin_unlock(&rx_slots_lock, key);
        /* No slot access after release. The callback must have copied its data. */
    }
}
#endif /* CONFIG_THINGSET_CAN_REPORT_RX */

static void thingset_can_report_tx_cb(const struct device *dev, int error, void *user_data)
{
    struct thingset_can *ts_can = user_data;
    k_spinlock_key_t key = k_spin_lock(&ts_can->report_state_lock);
    ts_can->report_tx_error = error;
    ts_can->report_tx_pending = false;
    k_sem_give(&ts_can->report_tx_sem);
    k_spin_unlock(&ts_can->report_state_lock, key);
}

int thingset_can_send_raw_report_inst(struct thingset_can *ts_can, const uint8_t *data,
                                     size_t length, k_timeout_t timeout)
{
    if (ts_can == NULL || data == NULL || length == 0) {
        return -EINVAL;
    }
    if (length > CONFIG_THINGSET_CAN_REPORT_MAX_SIZE) {
        return -EMSGSIZE;
    }
    if (K_TIMEOUT_EQ(timeout, K_FOREVER) || K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!device_is_ready(ts_can->dev)) {
        return -ENODEV;
    }
    if (!atomic_get(&ts_can->ready)) {
        return -EAGAIN;
    }
    k_timepoint_t end = sys_timepoint_calc(timeout);
    int ret = k_mutex_lock(&ts_can->report_lock, sys_timepoint_timeout(end));
    if (ret != 0) {
        return -ETIMEDOUT;
    }
    k_spinlock_key_t key = k_spin_lock(&ts_can->report_state_lock);
    if (ts_can->report_tx_pending) {
        k_spin_unlock(&ts_can->report_state_lock, key);
        k_mutex_unlock(&ts_can->report_lock);
        return -EBUSY;
    }
    k_spin_unlock(&ts_can->report_state_lock, key);

    uint8_t message = ts_can->msg_no++;
    size_t pos = 0;
    uint8_t sequence = 0;
    while (pos < length) {
        if (sys_timepoint_expired(end)) {
            ret = -ETIMEDOUT;
            break;
        }
        size_t chunk = MIN(length - pos, CAN_MAX_DLEN);
        bool last = pos + chunk == length;
        uint32_t type = pos == 0
                            ? (last ? THINGSET_CAN_MF_TYPE_SINGLE : THINGSET_CAN_MF_TYPE_FIRST)
                            : (last ? THINGSET_CAN_MF_TYPE_LAST : THINGSET_CAN_MF_TYPE_CONSEC);
        struct can_frame frame = {
            .id = THINGSET_CAN_PRIO_REPORT_LOW | THINGSET_CAN_TYPE_MF_REPORT
                  | THINGSET_CAN_MSG_NO_SET(message) | type | THINGSET_CAN_SEQ_NO_SET(sequence)
                  | THINGSET_CAN_SOURCE_SET(ts_can->node_addr)
#ifdef CONFIG_THINGSET_CAN_ROUTING_BUSES
                  | THINGSET_CAN_SOURCE_BUS_SET(ts_can->route),
#else
                  | THINGSET_CAN_BRIDGE_SET(ts_can->route),
#endif
            .flags = CAN_FRAME_IDE | (IS_ENABLED(CONFIG_CAN_FD_MODE) ? CAN_FRAME_FDF : 0)
                     | (IS_ENABLED(CONFIG_THINGSET_CAN_FD_BRS) ? CAN_FRAME_BRS : 0),
            .dlc = can_bytes_to_dlc(chunk),
        };
        /* The zero-initialized frame pads the final CAN FD DLC only. */
        memcpy(frame.data, data + pos, chunk);
        key = k_spin_lock(&ts_can->report_state_lock);
        k_sem_reset(&ts_can->report_tx_sem);
        ts_can->report_tx_pending = true;
        ts_can->report_tx_error = 0;
        k_spin_unlock(&ts_can->report_state_lock, key);
        ret = can_send(ts_can->dev, &frame, sys_timepoint_timeout(end),
                       thingset_can_report_tx_cb, ts_can);
        if (ret != 0) {
            key = k_spin_lock(&ts_can->report_state_lock);
            ts_can->report_tx_pending = false;
            k_spin_unlock(&ts_can->report_state_lock, key);
            break;
        }
        if (k_sem_take(&ts_can->report_tx_sem, sys_timepoint_timeout(end)) != 0) {
            /* Leave pending set until the real callback: never reuse its completion. */
            ret = -ETIMEDOUT;
            break;
        }
        key = k_spin_lock(&ts_can->report_state_lock);
        ret = ts_can->report_tx_error;
        k_spin_unlock(&ts_can->report_state_lock, key);
        if (ret != 0) {
            break;
        }
        pos += chunk;
        sequence++;
        if (pos < length) {
            k_timeout_t remaining = sys_timepoint_timeout(end);
            k_sleep(K_TICKS(MIN(remaining.ticks,
                               K_MSEC(CONFIG_THINGSET_CAN_FRAME_SEPARATION_TIME).ticks)));
        }
    }
    k_mutex_unlock(&ts_can->report_lock);
    return ret;
}

int thingset_can_send_report_inst(struct thingset_can *ts_can, const char *path,
                                  enum thingset_data_format format)
{
    struct shared_buffer *buffer = thingset_sdk_shared_buffer();
    k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_THINGSET_CAN_REPORT_TIMEOUT));
    if (k_sem_take(&buffer->lock, sys_timepoint_timeout(end)) != 0) {
        return -ETIMEDOUT;
    }
    int length = thingset_report_path(&ts, buffer->data, buffer->size, path, format);
    int ret = length <= 0 ? (length < 0 ? length : -ENODATA)
                         : thingset_can_send_raw_report_inst(ts_can, buffer->data, length,
                                                             sys_timepoint_timeout(end));
    k_sem_give(&buffer->lock);
    return ret;
}

#ifdef CONFIG_THINGSET_SUBSET_LIVE_METRICS
static void thingset_can_live_reporting_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct thingset_can *ts_can = CONTAINER_OF(dwork, struct thingset_can, live_reporting_work);

    if (live_reporting_enable) {
        thingset_can_send_report_inst(ts_can, TS_NAME_SUBSET_LIVE, THINGSET_BIN_IDS_VALUES);
    }

    ts_can->next_live_report_time += 1000 * live_reporting_period;
    if (ts_can->next_live_report_time <= k_uptime_get()) {
        /* ensure proper initialization of next_live_report_time */
        ts_can->next_live_report_time = k_uptime_get() + 1000 * live_reporting_period;
    }

    thingset_sdk_reschedule_work(dwork, K_TIMEOUT_ABS_MS(ts_can->next_live_report_time));
}
#endif /* CONFIG_THINGSET_SUBSET_LIVE_METRICS */

#ifdef CONFIG_THINGSET_CAN_CONTROL_REPORTING
static void thingset_can_item_tx_cb(const struct device *dev, int error, void *user_data)
{
    /* Do nothing: Single-frame reports are fire and forget. */
}

static void thingset_can_control_reporting_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct thingset_can *ts_can = CONTAINER_OF(dwork, struct thingset_can, control_reporting_work);
    int data_len = 0;
    int err;

    struct can_frame frame = {
        .flags = CAN_FRAME_IDE,
    };
    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();

    struct thingset_data_object *obj = NULL;
    while (ts_can->control_enable
           && (obj = thingset_iterate_subsets(&ts, CONFIG_THINGSET_CAN_CONTROL_SUBSET, obj))
                  != NULL)
    {
        k_sem_take(&sbuf->lock, K_FOREVER);
        data_len = thingset_export_item(&ts, sbuf->data, sbuf->size, obj, THINGSET_BIN_VALUES_ONLY);
        if (data_len > CAN_MAX_DLEN) {
            LOG_WRN("Value of data item %x exceeds single CAN frame payload size", obj->id);
            k_sem_give(&sbuf->lock);
        }
        else if (data_len > 0) {
            memcpy(frame.data, sbuf->data, data_len);
            k_sem_give(&sbuf->lock);
            frame.id = THINGSET_CAN_TYPE_SF_REPORT | THINGSET_CAN_PRIO_CONTROL_LOW
                       | THINGSET_CAN_DATA_ID_SET(obj->id)
                       | THINGSET_CAN_SOURCE_SET(ts_can->node_addr);
#ifdef CONFIG_CAN_FD_MODE
            frame.flags |= CAN_FRAME_FDF;
#endif
            frame.dlc = can_bytes_to_dlc(data_len);
            err = can_send(ts_can->dev, &frame, K_MSEC(CONFIG_THINGSET_CAN_REPORT_SEND_TIMEOUT),
                           thingset_can_item_tx_cb, NULL);
            if (err != 0) {
                LOG_DBG("Error sending CAN frame with ID %x", frame.id);
            }
#ifdef CONFIG_CAN_FD_MODE
            frame.flags &= ~CAN_FRAME_FDF;
#endif
        }
        else {
            k_sem_give(&sbuf->lock);
        }
        obj++; /* continue with object behind current one */
    }

    ts_can->next_control_report_time += ts_can->control_period;
    if (ts_can->next_control_report_time <= k_uptime_get()) {
        /* ensure proper initialization of next_control_report_time */
        ts_can->next_control_report_time = k_uptime_get() + ts_can->control_period;
    }

    thingset_sdk_reschedule_work(dwork, K_TIMEOUT_ABS_MS(ts_can->next_control_report_time));
}
#endif

/* The timer, RX path and TX error path compete for one terminal callback.
 * Retain the transaction semaphore until the callback returns: its bytes and
 * argument cannot be overwritten by a request launched from another thread. */
static bool thingset_can_finish_request(struct thingset_can_request_response *rr,
                                       uint32_t generation, uint32_t can_id, uint8_t *data,
                                       size_t length, int send_error, int receive_error)
{
    k_spinlock_key_t key = k_spin_lock(&rr->lock);
    if (rr->callback == NULL || rr->generation != generation || rr->can_id != can_id) {
        k_spin_unlock(&rr->lock, key);
        return false;
    }
    thingset_can_reqresp_callback_t callback = rr->callback;
    void *arg = rr->cb_arg;
    rr->callback = NULL;
    rr->cb_arg = NULL;
    rr->can_id = 0;
    k_timer_stop(&rr->timer);
    k_spin_unlock(&rr->lock, key);
    callback(data, length, send_error, receive_error, THINGSET_CAN_SOURCE_GET(can_id), arg);
    k_sem_give(&rr->sem);
    return true;
}

/* ISO-TP status values are not errno values. */
static int thingset_can_isotp_error(int error)
{
    switch (error) {
    case ISOTP_N_OK:
        return 0;
    case ISOTP_N_TIMEOUT_A:
    case ISOTP_N_TIMEOUT_BS:
    case ISOTP_N_TIMEOUT_CR:
        return -ETIMEDOUT;
    case ISOTP_N_BUFFER_OVERFLW:
        return -EMSGSIZE;
    case ISOTP_NO_NET_BUF_LEFT:
    case ISOTP_NO_CTX_LEFT:
        return -ENOBUFS;
    default:
        return -EIO;
    }
}

static struct isotp_fast_addr thingset_can_get_tx_addr(const struct isotp_fast_addr *rx_addr)
{
    return (struct isotp_fast_addr){
        .ext_id = (rx_addr->ext_id & 0x1F000000)
#ifdef CONFIG_THINGSET_CAN_ROUTING_BUSES
                  | THINGSET_CAN_TARGET_BUS_SET(THINGSET_CAN_SOURCE_BUS_GET(rx_addr->ext_id))
                  | THINGSET_CAN_SOURCE_BUS_SET(THINGSET_CAN_TARGET_BUS_GET(rx_addr->ext_id))
#else /* CONFIG_THINGSET_CAN_ROUTING_BRIDGES */
                  | THINGSET_CAN_BRIDGE_SET(THINGSET_CAN_BRIDGE_GET(rx_addr->ext_id))
#endif
                  | THINGSET_CAN_SOURCE_SET(THINGSET_CAN_TARGET_GET(rx_addr->ext_id))
                  | THINGSET_CAN_TARGET_SET(THINGSET_CAN_SOURCE_GET(rx_addr->ext_id)),
    };
}

static void thingset_can_reqresp_timeout_handler(struct k_timer *timer)
{
    struct thingset_can_request_response *rr =
        CONTAINER_OF(timer, struct thingset_can_request_response, timer);
    k_spinlock_key_t key = k_spin_lock(&rr->lock);
    uint32_t generation = rr->generation;
    uint32_t can_id = rr->can_id;
    /* A timer callback already running when a previous request completed must
     * not expire a subsequently installed request. */
    bool expired = rr->callback != NULL && k_uptime_get() >= rr->deadline;
    k_spin_unlock(&rr->lock, key);
    if (expired) {
        thingset_can_finish_request(rr, generation, can_id, NULL, 0, 0, -ETIMEDOUT);
    }
}

int thingset_can_send_inst(struct thingset_can *ts_can, uint8_t *tx_buf, size_t tx_len,
                           uint8_t target_addr, uint8_t route,
                           thingset_can_reqresp_callback_t callback, void *callback_arg,
                           k_timeout_t timeout)
{
    if (ts_can == NULL || tx_buf == NULL || tx_len == 0
        || target_addr < THINGSET_CAN_ADDR_MIN || target_addr > THINGSET_CAN_ADDR_MAX
        || (IS_ENABLED(CONFIG_THINGSET_CAN_ROUTING_BUSES) && route > 15)) {
        return -EINVAL;
    }
    if (tx_len > CONFIG_THINGSET_CAN_TX_BUF_SIZE) {
        return -EMSGSIZE;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!device_is_ready(ts_can->dev)) {
        return -ENODEV;
    }
    if (!atomic_get(&ts_can->ready)) {
        return -EAGAIN;
    }
    if (callback != NULL && (K_TIMEOUT_EQ(timeout, K_FOREVER)
                             || K_TIMEOUT_EQ(timeout, K_NO_WAIT))) {
        return -EINVAL;
    }
    struct isotp_fast_addr tx_addr = {
        .ext_id = THINGSET_CAN_TYPE_REQRESP | THINGSET_CAN_PRIO_REQRESP
#ifdef CONFIG_THINGSET_CAN_ROUTING_BUSES
                  | THINGSET_CAN_SOURCE_BUS_SET(ts_can->route) | THINGSET_CAN_TARGET_BUS_SET(route)
#else
                  | THINGSET_CAN_BRIDGE_SET(route)
#endif
                  | THINGSET_CAN_SOURCE_SET(ts_can->node_addr)
                  | THINGSET_CAN_TARGET_SET(target_addr),
    };
    struct thingset_can_request_response *rr = &ts_can->request_response;
    struct thingset_can_tx_context *tx = callback != NULL ? &ts_can->client_tx
                                                        : &ts_can->server_tx;
    k_timepoint_t end = sys_timepoint_calc(timeout);
    if (callback != NULL && k_sem_take(&rr->sem, sys_timepoint_timeout(end)) != 0) {
        return -ETIMEDOUT;
    }
    if (k_sem_take(&tx->sem, sys_timepoint_timeout(end)) != 0) {
        if (callback != NULL) {
            k_sem_give(&rr->sem);
        }
        return -EBUSY;
    }
    memcpy(tx->data, tx_buf, tx_len);
    k_spinlock_key_t key = k_spin_lock(&tx->lock);
    tx->submitting = true;
    tx->callback_seen = false;
    tx->completed = false;
    tx->released = false;
    k_spin_unlock(&tx->lock, key);
    if (callback != NULL) {
        key = k_spin_lock(&rr->lock);
        tx->generation = ++rr->generation;
        rr->can_id = thingset_can_get_tx_addr(&tx_addr).ext_id;
        rr->callback = callback;
        rr->cb_arg = callback_arg;
        k_timeout_t remaining = sys_timepoint_timeout(end);
        rr->deadline = k_uptime_get() + k_ticks_to_ms_ceil64(remaining.ticks);
        k_timer_start(&rr->timer, remaining, K_NO_WAIT);
        k_spin_unlock(&rr->lock, key);
    }
    int result = isotp_fast_send(&ts_can->ctx, tx->data, tx_len, tx_addr, tx);
    int error = thingset_can_isotp_error(result);
    if (error != 0 && callback != NULL) {
        thingset_can_finish_request(rr, tx->generation, thingset_can_get_tx_addr(&tx_addr).ext_id,
                                   NULL, 0, error, 0);
    }
    key = k_spin_lock(&tx->lock);
    tx->submitting = false;
    /* Immediate failure schedules no asynchronous callback. A synchronous SF
     * callback may already have completed, but must not release storage until
     * isotp_fast_send itself has returned. */
    tx->completed |= result != ISOTP_N_OK;
    bool release = tx->completed && !tx->released;
    tx->released |= release;
    k_spin_unlock(&tx->lock, key);
    if (release) {
        k_sem_give(&tx->sem);
    }
    return error;
}

static void thingset_can_reqresp_recv_callback(struct net_buf *buffer, int rem_len,
                                               struct isotp_fast_addr addr, void *arg)
{
    struct thingset_can *ts_can = arg;
    struct thingset_can_request_response *rr = &ts_can->request_response;
    k_spinlock_key_t key = k_spin_lock(&rr->lock);
    uint32_t generation = rr->generation;
    bool response = rr->callback != NULL && rr->can_id == addr.ext_id;
    k_spin_unlock(&rr->lock, key);
    if (rem_len != 0 || buffer == NULL) {
        if (response) {
            thingset_can_finish_request(rr, generation, addr.ext_id, NULL, 0, 0,
                                       rem_len < 0 ? thingset_can_isotp_error(rem_len) : -EMSGSIZE);
        }
        return;
    }
    size_t length = net_buf_frags_len(buffer);
    if (length == 0 || length > sizeof(ts_can->rx_buffer)) {
        if (response) {
            thingset_can_finish_request(rr, generation, addr.ext_id, NULL, 0, 0, -EMSGSIZE);
        }
        return;
    }
    size_t copied = net_buf_linearize(ts_can->rx_buffer, sizeof(ts_can->rx_buffer), buffer, 0,
                                     length);
    if (copied != length) {
        if (response) {
            thingset_can_finish_request(rr, generation, addr.ext_id, NULL, 0, 0, -EMSGSIZE);
        }
        return;
    }
    if (response) {
        thingset_can_finish_request(rr, generation, addr.ext_id, ts_can->rx_buffer, length, 0, 0);
        return;
    }
    /* Binary response codes are >= 0x80. Never process an unsolicited/late
     * response as a command, even when another peer is currently being polled. */
    if (ts_can->rx_buffer[0] >= 0x80 || ts_can->rx_buffer[0] == ':') {
        return;
    }
    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();
    if (k_sem_take(&sbuf->lock, K_MSEC(CONFIG_THINGSET_CAN_REPORT_TIMEOUT)) != 0) {
        return;
    }
    key = k_spin_lock(&ts_can->request_context_lock);
    ts_can->request_thread = k_current_get();
    ts_can->request_source = THINGSET_CAN_SOURCE_GET(addr.ext_id);
    ts_can->request_route = IS_ENABLED(CONFIG_THINGSET_CAN_ROUTING_BUSES)
                                ? THINGSET_CAN_SOURCE_BUS_GET(addr.ext_id)
                                : THINGSET_CAN_BRIDGE_GET(addr.ext_id);
    k_spin_unlock(&ts_can->request_context_lock, key);
    int tx_len = thingset_process_message(&ts, ts_can->rx_buffer, length, sbuf->data, sbuf->size);
    key = k_spin_lock(&ts_can->request_context_lock);
    ts_can->request_thread = NULL;
    k_spin_unlock(&ts_can->request_context_lock, key);
    if (tx_len > 0 && tx_len <= sbuf->size) {
        uint8_t target = THINGSET_CAN_SOURCE_GET(addr.ext_id);
        uint8_t route = IS_ENABLED(CONFIG_THINGSET_CAN_ROUTING_BUSES)
                            ? THINGSET_CAN_SOURCE_BUS_GET(addr.ext_id)
                            : THINGSET_CAN_BRIDGE_GET(addr.ext_id);
        /* send_inst copies into the distinct server buffer before returning. */
        int error = thingset_can_send_inst(ts_can, sbuf->data, tx_len, target, route,
                                           NULL, NULL, K_NO_WAIT);
        if (error != 0) {
            LOG_WRN("Unable to send ThingSet response: %d", error);
        }
    }
    /* Only the code which acquired the shared buffer releases it. */
    k_sem_give(&sbuf->lock);
}

static void thingset_can_reqresp_recv_error_callback(int8_t error, struct isotp_fast_addr addr,
                                                     void *arg)
{
    struct thingset_can *ts_can = arg;
    struct thingset_can_request_response *rr = &ts_can->request_response;
    k_spinlock_key_t key = k_spin_lock(&rr->lock);
    uint32_t generation = rr->generation;
    k_spin_unlock(&rr->lock, key);
    thingset_can_finish_request(rr, generation, addr.ext_id, NULL, 0, 0,
                               thingset_can_isotp_error(error));
}

static void thingset_can_reqresp_sent_callback(int result, void *arg)
{
    struct thingset_can_tx_context *tx = arg;
    struct thingset_can_request_response *rr = &tx->owner->request_response;
    k_spinlock_key_t key = k_spin_lock(&tx->lock);
    if (tx->callback_seen) {
        k_spin_unlock(&tx->lock, key);
        return;
    }
    tx->callback_seen = true;
    uint32_t generation = tx->generation;
    k_spin_unlock(&tx->lock, key);
    if (tx->client && result != ISOTP_N_OK) {
        key = k_spin_lock(&rr->lock);
        uint32_t can_id = rr->can_id;
        k_spin_unlock(&rr->lock, key);
        thingset_can_finish_request(rr, generation, can_id, NULL, 0,
                                   thingset_can_isotp_error(result), 0);
    }
    /* Successful TX is not a response. Keep the request and its timer alive. */
    key = k_spin_lock(&tx->lock);
    tx->completed = true;
    bool release = !tx->submitting && !tx->released;
    tx->released |= release;
    k_spin_unlock(&tx->lock, key);
    if (release) {
        k_sem_give(&tx->sem);
    }
}

static void thingset_can_timeout_timer_expired(struct k_timer *timer)
{
    struct thingset_can *ts_can = CONTAINER_OF(timer, struct thingset_can, timeout_timer);
    k_event_set(&ts_can->events, EVENT_ADDRESS_CLAIM_TIMED_OUT);
}

static void thingset_can_publish_startup(struct thingset_can *ts_can, bool started,
                                        bool ready, int error)
{
    k_spinlock_key_t key = k_spin_lock(&ts_can->state_lock);
    bool changed = atomic_get(&ts_can->driver_started) != started
                   || atomic_get(&ts_can->ready) != ready
                   || atomic_get(&ts_can->init_error) != error;
    atomic_set(&ts_can->driver_started, started);
    atomic_set(&ts_can->ready, ready);
    atomic_set(&ts_can->init_error, error);
    thingset_can_state_callback_t callback = changed ? ts_can->state_callback : NULL;
    void *arg = ts_can->state_callback_arg;
    k_spin_unlock(&ts_can->state_lock, key);
    if (callback != NULL) {
        callback(arg);
    }
}

static int thingset_can_startup_failed(struct thingset_can *ts_can, int error)
{
    thingset_can_publish_startup(ts_can, atomic_get(&ts_can->driver_started), false, error);
    return error;
}

void thingset_can_set_state_callback_inst(struct thingset_can *ts_can,
                                          thingset_can_state_callback_t callback, void *arg)
{
    k_spinlock_key_t key = k_spin_lock(&ts_can->state_lock);
    ts_can->state_callback = callback;
    ts_can->state_callback_arg = arg;
    k_spin_unlock(&ts_can->state_lock, key);
    if (callback != NULL) {
        callback(arg);
    }
}

int thingset_can_init_inst(struct thingset_can *ts_can, const struct device *can_dev,
                           uint8_t bus_number, k_timeout_t timeout)
{
    struct can_frame tx_frame = {
        .flags = CAN_FRAME_IDE,
    };
    int filter_id;
    int err;

    thingset_can_publish_startup(ts_can, false, false, 0);
    if (!device_is_ready(can_dev)) {
        LOG_ERR("CAN device not ready");
        return thingset_can_startup_failed(ts_can, -ENODEV);
    }

    k_sem_init(&ts_can->request_response.sem, 1, 1);
    k_timer_init(&ts_can->request_response.timer, thingset_can_reqresp_timeout_handler, NULL);
    k_mutex_init(&ts_can->report_lock);
    ts_can->client_tx.owner = ts_can;
    ts_can->client_tx.client = true;
    ts_can->server_tx.owner = ts_can;
    ts_can->server_tx.client = false;
    k_sem_init(&ts_can->client_tx.sem, 1, 1);
    k_sem_init(&ts_can->server_tx.sem, 1, 1);
    k_sem_init(&ts_can->report_tx_sem, 0, 1);
    k_timer_init(&ts_can->timeout_timer, thingset_can_timeout_timer_expired, NULL);

#ifdef CONFIG_THINGSET_SUBSET_LIVE_METRICS
    k_work_init_delayable(&ts_can->live_reporting_work, thingset_can_live_reporting_handler);
#endif
#ifdef CONFIG_THINGSET_CAN_CONTROL_REPORTING
    ts_can->control_enable = IS_ENABLED(CONFIG_THINGSET_CAN_CONTROL_REPORTING_ENABLE_PRESET);
    ts_can->control_period = CONFIG_THINGSET_CAN_CONTROL_REPORTING_PERIOD;
    k_work_init_delayable(&ts_can->control_reporting_work, thingset_can_control_reporting_handler);
#endif
    k_work_init_delayable(&ts_can->addr_claim_work, thingset_can_addr_claim_tx_handler);

    ts_can->dev = can_dev;
    ts_can->route = bus_number;

    /* set initial address (will be changed if already used on the bus) */
    if (ts_can->node_addr < THINGSET_CAN_ADDR_MIN || ts_can->node_addr > THINGSET_CAN_ADDR_MAX) {
        ts_can->node_addr = THINGSET_CAN_ADDR_MIN;
    }

    k_event_init(&ts_can->events);
    k_timer_start(&ts_can->timeout_timer, timeout, K_NO_WAIT);

#ifdef CONFIG_CAN_FD_MODE
    can_mode_t supported_modes;
    err = can_get_capabilities(can_dev, &supported_modes);
    if (err == 0 && (supported_modes & CAN_MODE_FD) != 0) {
        err = can_set_mode(ts_can->dev, CAN_MODE_FD);
        if (err == 0) {
            LOG_DBG("Enabled CAN-FD mode");
        }
        else {
            LOG_ERR("Failed to enable CAN-FD mode");
            err = -ENODEV;
            goto failed;
        }
    }
    else {
        LOG_ERR("CAN device does not support CAN-FD; recompile with CAN_FD_MODE set to false.");
        /* there is no point continuing, as we will still assume a 64-byte payload everywhere */
        err = -ENODEV;
        goto failed;
    }
#endif

    err = can_start(ts_can->dev);
    if (err != 0 && err != -EALREADY) {
        goto failed;
    }

    struct can_filter addr_claim_filter = {
        .id = THINGSET_CAN_TYPE_NETWORK | THINGSET_CAN_TARGET_SET(THINGSET_CAN_ADDR_BROADCAST),
        .mask = THINGSET_CAN_TYPE_MASK | THINGSET_CAN_TARGET_MASK,
        .flags = CAN_FILTER_IDE,
    };

#ifdef CONFIG_THINGSET_CAN_ROUTING_BUSES
    addr_claim_filter.id |=
        THINGSET_CAN_TARGET_BUS_SET(bus_number) | THINGSET_CAN_SOURCE_BUS_SET(bus_number);
    addr_claim_filter.mask |= THINGSET_CAN_TARGET_BUS_MASK | THINGSET_CAN_SOURCE_BUS_MASK;
#elif defined(CONFIG_THINGSET_CAN_ROUTING_BRIDGES)
    addr_claim_filter.id |= THINGSET_CAN_BRIDGE_SET(bus_number);
    addr_claim_filter.mask |= THINGSET_CAN_BRIDGE_MASK;
#endif

    filter_id =
        can_add_rx_filter(ts_can->dev, thingset_can_addr_claim_rx_cb, ts_can, &addr_claim_filter);
    if (filter_id < 0) {
        LOG_ERR("Unable to add addr_claim filter: %d", filter_id);
        err = filter_id;
        goto failed;
    }

    /* Local driver/filter startup needs no ACK peer or claimed address. */
    thingset_can_publish_startup(ts_can, true, false, 0);

    while (1) {
        if (k_event_test(&ts_can->events, EVENT_ADDRESS_CLAIM_TIMED_OUT)) {
            can_remove_rx_filter(ts_can->dev, filter_id);
            err = -ETIMEDOUT;
            goto failed;
        }
        k_event_clear(&ts_can->events, EVENT_ADDRESS_CLAIM_MSG_SENT
                                           | EVENT_ADDRESS_CLAIMING_FINISHED
                                           | EVENT_ADDRESS_ALREADY_USED);

        /* send out address discovery frame */
        uint8_t rand = sys_rand32_get() & 0xFF;
        tx_frame.id = THINGSET_CAN_PRIO_NETWORK_MGMT | THINGSET_CAN_TYPE_NETWORK
                      | THINGSET_CAN_RAND_SET(rand) | THINGSET_CAN_TARGET_SET(ts_can->node_addr)
                      | THINGSET_CAN_SOURCE_SET(THINGSET_CAN_ADDR_ANONYMOUS);
        tx_frame.dlc = 0;
        err =
            can_send(ts_can->dev, &tx_frame, K_MSEC(10), thingset_can_addr_discovery_tx_cb, ts_can);
        if (err != 0) {
            k_sleep(K_MSEC(100));
            continue;
        }

        /* wait 500 ms for address claim message from other node */
        uint32_t event = k_event_wait(&ts_can->events,
                                      EVENT_ADDRESS_ALREADY_USED | EVENT_ADDRESS_CLAIM_TIMED_OUT,
                                      false, K_MSEC(500));
        if (event & EVENT_ADDRESS_ALREADY_USED) {
            /* try again with new random node_addr between 0x01 and 0xFD */
            ts_can->node_addr =
                THINGSET_CAN_ADDR_MIN
                + sys_rand32_get() % (THINGSET_CAN_ADDR_MAX - THINGSET_CAN_ADDR_MIN + 1);
            LOG_WRN("Node addr already in use, trying 0x%.2X", ts_can->node_addr);
        }
        else if (event & EVENT_ADDRESS_CLAIM_TIMED_OUT) {
            LOG_ERR("Address claim timed out");
            err = -ETIMEDOUT;
            goto failed;
        }
        else {
            struct can_bus_err_cnt err_cnt_before;
            can_get_state(ts_can->dev, NULL, &err_cnt_before);

            thingset_sdk_reschedule_work(&ts_can->addr_claim_work, K_NO_WAIT);

            event = k_event_wait(&ts_can->events,
                                 EVENT_ADDRESS_CLAIM_MSG_SENT | EVENT_ADDRESS_CLAIM_TIMED_OUT,
                                 false, K_MSEC(100));
            if (event & EVENT_ADDRESS_CLAIM_TIMED_OUT) {
                LOG_ERR("Address claim timed out");
                err = -ETIMEDOUT;
                goto failed;
            }
            else if (!(event & EVENT_ADDRESS_CLAIM_MSG_SENT)) {
                k_sleep(K_MSEC(100));
                continue;
            }

            struct can_bus_err_cnt err_cnt_after;
            can_get_state(ts_can->dev, NULL, &err_cnt_after);

            if (err_cnt_after.tx_err_cnt <= err_cnt_before.tx_err_cnt) {
                /* address claiming is finished */
                k_event_post(&ts_can->events, EVENT_ADDRESS_CLAIMING_FINISHED);
                k_timer_stop(&ts_can->timeout_timer);
                LOG_INF("Using CAN node address 0x%.2X on %s", ts_can->node_addr,
                        ts_can->dev->name);
                break;
            }

            /* Continue the loop in the very unlikely case of a collision because two nodes with
             * different EUI-64 tried to claim the same node address at exactly the same time.
             */
        }
    }

#if CONFIG_THINGSET_STORAGE
    /* save node address as init value for next boot-up */
    thingset_storage_save_queued(false);
#endif

    struct can_filter addr_discovery_filter = {
        .id = THINGSET_CAN_TYPE_NETWORK | THINGSET_CAN_SOURCE_SET(THINGSET_CAN_ADDR_ANONYMOUS)
              | THINGSET_CAN_TARGET_SET(ts_can->node_addr),
        .mask = THINGSET_CAN_TYPE_MASK | THINGSET_CAN_SOURCE_MASK | THINGSET_CAN_TARGET_MASK,
        .flags = CAN_FILTER_IDE,
    };
    filter_id = can_add_rx_filter(ts_can->dev, thingset_can_addr_discovery_rx_cb, ts_can,
                                  &addr_discovery_filter);
    if (filter_id < 0) {
        LOG_ERR("Unable to add addr_discovery filter: %d", filter_id);
        err = filter_id;
        goto failed;
    }

    struct isotp_fast_addr rx_addr = {
        .ext_id = THINGSET_CAN_TYPE_REQRESP | THINGSET_CAN_PRIO_REQRESP
                  | THINGSET_CAN_TARGET_SET(ts_can->node_addr),
    };
    ts_can->ctx.get_tx_addr_callback = thingset_can_get_tx_addr;
    err = isotp_fast_bind(&ts_can->ctx, can_dev, rx_addr, &fc_opts,
                         thingset_can_reqresp_recv_callback, ts_can,
                         thingset_can_reqresp_recv_error_callback,
                         thingset_can_reqresp_sent_callback);
    if (err != 0) {
        goto failed;
    }
    thingset_can_publish_startup(ts_can, true, true, 0);

#ifdef CONFIG_THINGSET_SUBSET_LIVE_METRICS
    thingset_sdk_reschedule_work(&ts_can->live_reporting_work, K_NO_WAIT);
#endif
#ifdef CONFIG_THINGSET_CAN_CONTROL_REPORTING
    thingset_sdk_reschedule_work(&ts_can->control_reporting_work, K_NO_WAIT);
#endif

    return 0;

failed:
    k_timer_stop(&ts_can->timeout_timer);
    return thingset_can_startup_failed(ts_can, err);
}

void thingset_can_set_addr_claim_rx_callback_inst(struct thingset_can *ts_can,
                                                  thingset_can_addr_claim_rx_callback_t cb)
{
    ts_can->addr_claim_callback = cb;
}

int thingset_can_get_request_source_inst(struct thingset_can *ts_can, uint8_t *source,
                                         uint8_t *route)
{
    if (ts_can == NULL || source == NULL || route == NULL) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -ENOENT;
    }
    k_spinlock_key_t key = k_spin_lock(&ts_can->request_context_lock);
    int error = -ENOENT;
    if (ts_can->request_thread != NULL && ts_can->request_thread == k_current_get()) {
        *source = ts_can->request_source;
        *route = ts_can->request_route;
        error = 0;
    }
    k_spin_unlock(&ts_can->request_context_lock, key);
    return error;
}

static int thingset_can_network_send(struct thingset_can *ts_can, uint8_t target_addr,
                                      bool announce, k_timeout_t timeout)
{
    if (ts_can == NULL || target_addr < THINGSET_CAN_ADDR_MIN
        || target_addr > THINGSET_CAN_ADDR_MAX || K_TIMEOUT_EQ(timeout, K_FOREVER)
        || K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!device_is_ready(ts_can->dev)) {
        return -ENODEV;
    }
    if (!atomic_get(&ts_can->ready)) {
        return -EAGAIN;
    }
    if (ts_can->route != 0) {
        return -ENOTSUP;
    }
    k_timepoint_t end = sys_timepoint_calc(timeout);
    if (k_mutex_lock(&ts_can->report_lock, sys_timepoint_timeout(end)) != 0) {
        return -ETIMEDOUT;
    }
    k_spinlock_key_t key = k_spin_lock(&ts_can->report_state_lock);
    if (ts_can->report_tx_pending) {
        k_spin_unlock(&ts_can->report_state_lock, key);
        k_mutex_unlock(&ts_can->report_lock);
        return -EBUSY;
    }
    k_sem_reset(&ts_can->report_tx_sem);
    ts_can->report_tx_pending = true;
    ts_can->report_tx_error = 0;
    k_spin_unlock(&ts_can->report_state_lock, key);
    struct can_frame frame = {
        .id = THINGSET_CAN_PRIO_NETWORK_MGMT | THINGSET_CAN_TYPE_NETWORK
              | THINGSET_CAN_RAND_SET(sys_rand32_get()) | THINGSET_CAN_TARGET_SET(target_addr)
              | THINGSET_CAN_SOURCE_SET(THINGSET_CAN_ADDR_ANONYMOUS),
        .flags = CAN_FRAME_IDE,
        .dlc = 0,
    };
    if (announce) {
        frame.id = THINGSET_CAN_PRIO_NETWORK_MGMT | THINGSET_CAN_TYPE_NETWORK
                   | THINGSET_CAN_TARGET_SET(THINGSET_CAN_ADDR_BROADCAST)
                   | THINGSET_CAN_SOURCE_SET(ts_can->node_addr);
        frame.dlc = sizeof(eui64);
        memcpy(frame.data, eui64, sizeof(eui64));
    }
    int error = can_send(ts_can->dev, &frame, sys_timepoint_timeout(end),
                         thingset_can_report_tx_cb, ts_can);
    if (error != 0) {
        key = k_spin_lock(&ts_can->report_state_lock);
        ts_can->report_tx_pending = false;
        k_spin_unlock(&ts_can->report_state_lock, key);
    }
    else if (k_sem_take(&ts_can->report_tx_sem, sys_timepoint_timeout(end)) != 0) {
        error = -ETIMEDOUT;
    }
    else {
        key = k_spin_lock(&ts_can->report_state_lock);
        error = ts_can->report_tx_error;
        k_spin_unlock(&ts_can->report_state_lock, key);
    }
    k_mutex_unlock(&ts_can->report_lock);
    return error;
}

int thingset_can_probe_address_inst(struct thingset_can *ts_can, uint8_t target_addr,
                                    k_timeout_t timeout)
{
    return thingset_can_network_send(ts_can, target_addr, false, timeout);
}

int thingset_can_announce_address_inst(struct thingset_can *ts_can, k_timeout_t timeout)
{
    return thingset_can_network_send(ts_can, THINGSET_CAN_ADDR_MIN, true, timeout);
}

#ifdef CONFIG_THINGSET_CAN_REPORT_RX
int thingset_can_set_report_rx_callback_inst(struct thingset_can *ts_can,
                                             thingset_can_report_rx_callback_t rx_cb)
{
    if (!device_is_ready(ts_can->dev)) {
        return -ENODEV;
    }

    if (rx_cb == NULL) {
        return -EINVAL;
    }

    ts_can->report_rx_cb = rx_cb;
    if (ts_can->report_filter_installed) {
        return 0;
    }

    int filter_id =
        can_add_rx_filter(ts_can->dev, thingset_can_report_rx_cb, ts_can, &mf_report_filter);
    if (filter_id < 0) {
        LOG_ERR("Unable to add packetized report filter: %d", filter_id);
        return filter_id;
    }
    ts_can->report_filter_installed = true;

    return 0;
}
#endif /* CONFIG_THINGSET_CAN_REPORT_RX */

#ifdef CONFIG_THINGSET_CAN_ITEM_RX
int thingset_can_set_item_rx_callback_inst(struct thingset_can *ts_can,
                                           thingset_can_item_rx_callback_t rx_cb)
{
    if (!device_is_ready(ts_can->dev)) {
        return -ENODEV;
    }

    if (rx_cb == NULL) {
        return -EINVAL;
    }

    ts_can->item_rx_cb = rx_cb;

    int filter_id =
        can_add_rx_filter(ts_can->dev, thingset_can_item_rx_cb, ts_can, &sf_report_filter);
    if (filter_id < 0) {
        LOG_ERR("Unable to add report filter: %d", filter_id);
        return filter_id;
    }

    return 0;
}
#endif /* CONFIG_THINGSET_CAN_ITEM_RX */

#ifndef CONFIG_THINGSET_CAN_MULTIPLE_INSTANCES

#if DT_NODE_EXISTS(DT_CHOSEN(thingset_can))
#define CAN_DEVICE_NODE DT_CHOSEN(thingset_can)
#else
#define CAN_DEVICE_NODE DT_CHOSEN(zephyr_canbus)
#endif

static const struct device *can_dev = DEVICE_DT_GET(CAN_DEVICE_NODE);

static struct thingset_can ts_can_single = {
    .dev = DEVICE_DT_GET(CAN_DEVICE_NODE),
    .node_addr = 1, /* initialize with valid default address */
};

THINGSET_ADD_ITEM_UINT8(TS_ID_NET, TS_ID_NET_CAN_NODE_ADDR, "pCANNodeAddr",
                        &ts_can_single.node_addr,
                        IS_ENABLED(CONFIG_THINGSET_CAN_ALLOW_ADDRESS_WRITE) ? THINGSET_ANY_RW
                                                                           : THINGSET_ANY_R,
                        TS_SUBSET_NVM);

int thingset_can_send_report(const char *path, enum thingset_data_format format)
{
    return thingset_can_send_report_inst(&ts_can_single, path, format);
}

int thingset_can_send_raw_report(const uint8_t *data, size_t length, k_timeout_t timeout)
{
    return thingset_can_send_raw_report_inst(&ts_can_single, data, length, timeout);
}

int thingset_can_probe_address(uint8_t target_addr, k_timeout_t timeout)
{
    return thingset_can_probe_address_inst(&ts_can_single, target_addr, timeout);
}

int thingset_can_announce_address(k_timeout_t timeout)
{
    return thingset_can_announce_address_inst(&ts_can_single, timeout);
}

void thingset_can_set_addr_claim_rx_callback(thingset_can_addr_claim_rx_callback_t cb)
{
    thingset_can_set_addr_claim_rx_callback_inst(&ts_can_single, cb);
}

int thingset_can_get_request_source(uint8_t *source, uint8_t *route)
{
    return thingset_can_get_request_source_inst(&ts_can_single, source, route);
}

void thingset_can_set_state_callback(thingset_can_state_callback_t callback, void *arg)
{
    thingset_can_set_state_callback_inst(&ts_can_single, callback, arg);
}

int thingset_can_send(uint8_t *tx_buf, size_t tx_len, uint8_t target_addr, uint8_t route,
                      thingset_can_reqresp_callback_t callback, void *callback_arg,
                      k_timeout_t timeout)
{
    return thingset_can_send_inst(&ts_can_single, tx_buf, tx_len, target_addr, route, callback,
                                  callback_arg, timeout);
}

#ifdef CONFIG_THINGSET_CAN_REPORT_RX
int thingset_can_set_report_rx_callback(thingset_can_report_rx_callback_t rx_cb)
{
    return thingset_can_set_report_rx_callback_inst(&ts_can_single, rx_cb);
}
#endif

#ifdef CONFIG_THINGSET_CAN_ITEM_RX
int thingset_can_set_item_rx_callback(thingset_can_item_rx_callback_t rx_cb)
{
    return thingset_can_set_item_rx_callback_inst(&ts_can_single, rx_cb);
}
#endif

struct thingset_can *thingset_can_get_inst()
{
    return &ts_can_single;
}

static void thingset_can_thread()
{
    int err;

    LOG_DBG("Initialising ThingSet CAN");
    err = thingset_can_init_inst(&ts_can_single, can_dev, CONFIG_THINGSET_CAN_DEFAULT_ROUTE,
                                 K_FOREVER);
    if (err != 0) {
        LOG_ERR("Failed to init ThingSet CAN: %d", err);
        return;
    }
}

K_THREAD_DEFINE(thingset_can, CONFIG_THINGSET_CAN_THREAD_STACK_SIZE, thingset_can_thread, NULL,
                NULL, NULL, CONFIG_THINGSET_CAN_THREAD_PRIORITY, 0, 0);

#endif /* !CONFIG_THINGSET_CAN_MULTIPLE_INSTANCES */
