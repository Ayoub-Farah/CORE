/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

static atomic_t led_state = ATOMIC_INIT(OTA_IDLE);
static atomic_t app_led = ATOMIC_INIT(0);
static const gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

extern "C" void ota_feedback_state(enum ota_state s) { atomic_set(&led_state, s); }
extern "C" void ota_feedback_application_led(int action)
{
    if (action == 2) atomic_xor(&app_led, 1); else atomic_set(&app_led, action != 0);
}

static void feedback(void *, void *, void *)
{
    if (!gpio_is_ready_dt(&led) || gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE)) return;
    for (;;) {
        uint32_t t = (uint32_t)k_uptime_get();
        int s = atomic_get(&led_state);
        bool on;
        switch (s) {
        case OTA_PREPARING: on = t % 1000 < 100 || (t % 1000 >= 200 && t % 1000 < 300); break;
        case OTA_READY: case OTA_PASS_OPEN: case OTA_PASS_CLOSED: on = t % 200 < 100; break;
        case OTA_VERIFYING: on = t % 600 < 300; break;
        case OTA_VALID: case OTA_COMMITTED: case OTA_REBOOTING: on = true; break;
        case OTA_FAILED: case OTA_ABORTED: case OTA_RECOVERY_REQUIRED:
            on = t % 2000 < 900; break;
        default:
            on = IS_ENABLED(CONFIG_OWNTECH_OTA_BLINK_DEMO)
                ? (t / CONFIG_OWNTECH_OTA_BLINK_HALF_PERIOD_MS) % 2 : atomic_get(&app_led);
        }
        gpio_pin_set_dt(&led, on);
        k_sleep(K_MSEC(25));
    }
}
K_THREAD_DEFINE(ota_feedback_thread, 768, feedback, nullptr, nullptr, nullptr, 10, 0, 0);
