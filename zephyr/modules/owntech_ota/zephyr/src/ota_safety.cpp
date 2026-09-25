/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include "SpinAPI.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stm32_ll_hrtim.h>
#include <stm32g4xx_ll_bus.h>

/* Fail closed from reset, before any setup routine or application thread. */
static atomic_t inhibited = ATOMIC_INIT(1);
extern "C" bool ota_safety_inhibited(void) { return atomic_get(&inhibited) != 0; }
extern "C" void ota_safety_restore(bool value) { atomic_set(&inhibited, value); }

extern "C" int ota_safety_check(void)
{
    extern uint8_t dt_leg_count;
    extern uint16_t dt_pin_driver[], dt_pin_capacitor[];
    if (!ota_safety_inhibited() || (HRTIM1->sCommonRegs.OENR & 0xFFFU)) return OTA_ERR_SAFETY;
    for (unsigned i=0;i<dt_leg_count;i++) {
        if (dt_pin_driver[i] && spin.gpio.readPin(dt_pin_driver[i]) != 0) return OTA_ERR_SAFETY;
        if (dt_pin_capacitor[i] && spin.gpio.readPin(dt_pin_capacitor[i]) != 1) return OTA_ERR_SAFETY;
    }
    return OTA_OK;
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
    /* Core owns the hardware stop, even before application initialization.
     * No user callback can delay or veto recovery of a broken application.
     * Critical tasks remain running; low-level output APIs enforce inhibition. */
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_HRTIM1);
    LL_HRTIM_DisableOutput(HRTIM1, 0xFFFU);
    return ota_safety_check();
}
