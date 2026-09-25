/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

static atomic_t led_state = ATOMIC_INIT(OTA_IDLE);
static atomic_t app_led = ATOMIC_INIT(0);
static const gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool configured;
static void feedback(struct k_work *);
K_WORK_DELAYABLE_DEFINE(ota_led_work, feedback);

extern "C" void ota_feedback_state(enum ota_state s)
{
    if (atomic_set(&led_state, s) != s) k_work_reschedule(&ota_led_work, K_NO_WAIT);
}
extern "C" void ota_feedback_application_led(int action)
{
    /* IRQ-safe producers do no GPIO initialization or sleeping. */
    if (action == 2) atomic_xor(&app_led, 1); else atomic_set(&app_led, action != 0);
    int s = atomic_get(&led_state);
    if (s == OTA_IDLE || s == OTA_SUCCEEDED) k_work_reschedule(&ota_led_work, K_NO_WAIT);
}
static void feedback(struct k_work *)
{
    if (!configured) {
        if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE)) return;
        configured = true;
    }
    uint32_t t = (uint32_t)k_uptime_get(), period = 0, edge = 0;
    bool on;
    switch (atomic_get(&led_state)) {
    case OTA_PREPARING:
        period = 1000; t %= period;
        on = t < 100 || (t >= 200 && t < 300);
        edge = t < 100 ? 100 : t < 200 ? 200 : t < 300 ? 300 : 1000;
        break;
    case OTA_READY: case OTA_PASS_OPEN: case OTA_PASS_CLOSED: period = 200; break;
    case OTA_VERIFYING: period = 600; break;
    case OTA_VALID: case OTA_COMMITTED: case OTA_REBOOTING: on = true; break;
    case OTA_FAILED: case OTA_ABORTED: case OTA_RECOVERY_REQUIRED:
        period = 2000; t %= period; on = t < 900; edge = on ? 900 : 2000; break;
    default: on = atomic_get(&app_led); break;
    }
    if (period && !edge) {t %= period;on = t < period / 2;edge = on ? period / 2 : period;}
    gpio_pin_set_dt(&led, on);
    /* schedule (not reschedule) preserves an immediate event queued while this
     * handler ran. No timer or thread wakeup remains in a steady state. */
    if (period) k_work_schedule(&ota_led_work, K_MSEC(edge - t));
}
