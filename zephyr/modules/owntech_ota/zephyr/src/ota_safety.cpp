/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include "ShieldAPI.h"
#include "SpinAPI.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stm32_ll_hrtim.h>

/* Fail closed from reset, before any setup routine or application thread. */
static atomic_t inhibited = ATOMIC_INIT(1);
extern "C" bool ota_safety_inhibited(void) { return atomic_get(&inhibited) != 0; }
extern "C" void ota_safety_restore(bool value) { atomic_set(&inhibited, value); }

extern "C" __weak int owntech_ota_enter_maintenance(void)
{
    return IS_ENABLED(CONFIG_OWNTECH_OTA_BLINK_DEMO) ? 0 : OTA_ERR_SAFETY;
}
extern "C" __weak int owntech_ota_check_health(void)
{
    return IS_ENABLED(CONFIG_OWNTECH_OTA_BLINK_DEMO) ? 0 : OTA_ERR_HEALTH;
}

extern "C" int ota_safety_enter(void)
{
    atomic_set(&inhibited, 1);
    extern uint8_t dt_leg_count;
    extern uint16_t dt_pin_driver[], dt_pin_capacitor[];
    for (unsigned i=0;i<dt_leg_count;i++) {
        if (dt_pin_driver[i]) spin.gpio.configurePin(dt_pin_driver[i],GPIO_OUTPUT_INACTIVE);
        if (dt_pin_capacitor[i]) spin.gpio.configurePin(dt_pin_capacitor[i],GPIO_OUTPUT_ACTIVE);
    }
    /* Do not stop the critical task: it also runs the safety supervision. */
    shield.power.stop(ALL);
    int rc = owntech_ota_enter_maintenance();
    shield.power.stop(ALL);
    /* OENR is the hardware output-enable state, including direct PWM API use. */
    if (rc || (HRTIM1->sCommonRegs.OENR & 0xFFFU)) return OTA_ERR_SAFETY;
    for (unsigned i=0;i<dt_leg_count;i++) {
        if (dt_pin_driver[i] && spin.gpio.readPin(dt_pin_driver[i]) != 0) return OTA_ERR_SAFETY;
        if (dt_pin_capacitor[i] && spin.gpio.readPin(dt_pin_capacitor[i]) != 1) return OTA_ERR_SAFETY;
    }
    return 0;
}
