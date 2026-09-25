/* Exercise production ota_safety.cpp with only GPIO/HRTIM hardware replaced.
 * No main application, SYNC source, or application task object is linked. */
#include "OtaService.h"
#include "SpinAPI.h"
#include <stm32_ll_hrtim.h>
#include <stm32g4xx_ll_bus.h>
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
#endif
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

SafetyTestSpin spin;
SafetyTestHrtim safety_test_hrtim;
extern "C" {
uint8_t dt_leg_count = 3;
uint16_t dt_pin_driver[] = {1, 0, 3};
uint16_t dt_pin_capacitor[] = {2, 4, 0};
}
static int pins[8], configured[8], disable_calls, read_calls, unsafe_actions;
static bool clock_enabled;
static uint16_t stuck_pin;
static int stuck_value;
static uint32_t stuck_pwm;
static int legacy_calls;

#ifdef SAFETY_TEST_LEGACY_CALLBACKS
/* An old application may still define these names. They must have no influence
 * over the system service, even when they always reject health/maintenance. */
extern "C" int owntech_ota_enter_maintenance()
{ ++legacy_calls; return OTA_ERR_SAFETY; }
extern "C" int owntech_ota_check_health()
{ ++legacy_calls; return OTA_ERR_HEALTH; }
#endif

void SafetyTestGpio::configurePin(uint8_t pin, gpio_flags_t flags)
{
    if (!ota_safety_inhibited() || pin == 0 || pin >= 8) {
        ++unsafe_actions;
        return;
    }
    ++configured[pin];
    pins[pin] = flags == GPIO_OUTPUT_ACTIVE ? 1 : 0;
}
uint8_t SafetyTestGpio::readPin(uint8_t pin)
{
    ++read_calls;
    if (pin == 0 || pin >= 8) { ++unsafe_actions; return 255; }
    return static_cast<uint8_t>(pin == stuck_pin ? stuck_value : pins[pin]);
}
void LL_APB2_GRP1_EnableClock(uint32_t peripheral)
{
    if (!ota_safety_inhibited() || peripheral != LL_APB2_GRP1_PERIPH_HRTIM1)
        ++unsafe_actions;
    clock_enabled = true;
}
void LL_HRTIM_DisableOutput(SafetyTestHrtim *timer, uint32_t outputs)
{
    if (!ota_safety_inhibited() || !clock_enabled || timer != HRTIM1 || outputs != 0xFFFU)
        ++unsafe_actions;
    ++disable_calls;
    safety_test_hrtim.sCommonRegs.OENR = stuck_pwm;
}

static void reset_hardware()
{
    for (unsigned i = 0; i < 8; ++i) { pins[i] = 0; configured[i] = 0; }
    pins[1] = pins[3] = 1; /* Driver enables initially active. */
    safety_test_hrtim.sCommonRegs.OENR = 0xFFF;
    stuck_pin = 0; stuck_value = 0; stuck_pwm = 0;
    clock_enabled = false;
    disable_calls = read_calls = unsafe_actions = legacy_calls = 0;
    ota_safety_restore(false);
}

static int test_safety()
{
    /* Static initialization inhibits power before any application has run. */
    CHECK(ota_safety_inhibited());

    reset_hardware();
    CHECK(ota_safety_check() == OTA_ERR_SAFETY);
    CHECK(!disable_calls && !read_calls); /* A check does not alter hardware. */
    CHECK(ota_safety_enter() == 0);
    CHECK(ota_safety_inhibited() && disable_calls > 0 && clock_enabled);
    CHECK(!safety_test_hrtim.sCommonRegs.OENR && !pins[1] && !pins[3]);
    CHECK(pins[2] == 1 && pins[4] == 1);
    CHECK(configured[1] && configured[2] && configured[3] && configured[4]);
    CHECK(!configured[0] && !configured[5] && !unsafe_actions && !legacy_calls);
    int previous_disables = disable_calls;
    int previous_configures = configured[1] + configured[2] + configured[3] + configured[4];
    CHECK(ota_safety_check() == 0);
    CHECK(disable_calls == previous_disables);
    CHECK(configured[1] + configured[2] + configured[3] + configured[4] == previous_configures);

    /* Safe GPIO/PWM alone cannot authorize flash after inhibition is released. */
    ota_safety_restore(false);
    CHECK(ota_safety_check() == OTA_ERR_SAFETY);
    CHECK(!ota_safety_inhibited());
    CHECK(ota_safety_enter() == 0 && ota_safety_inhibited());
    CHECK(ota_safety_enter() == 0); /* Re-entry is safe and independent of tasks. */
    CHECK(!unsafe_actions && !legacy_calls);

    /* Every physical PWM output is checked, including direct PWM API use. */
    for (unsigned bit = 0; bit < 12; ++bit) {
        reset_hardware();
        stuck_pwm = 1U << bit;
        CHECK(ota_safety_enter() == OTA_ERR_SAFETY);
        CHECK(ota_safety_inhibited() && !unsafe_actions && !legacy_calls);
        stuck_pwm = 0;
        CHECK(ota_safety_enter() == 0);
    }

    /* GPIO command success is insufficient: reject each unsafe readback and
     * negative driver errors for driver-enable and capacitor-disable pins. */
    const uint16_t tested_pins[] = {1, 3, 2, 4};
    const int unsafe_values[] = {1, 1, 0, 0};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned failure = 0; failure < 2; ++failure) {
            reset_hardware();
            stuck_pin = tested_pins[i];
            stuck_value = failure ? -1 : unsafe_values[i];
            CHECK(ota_safety_enter() == OTA_ERR_SAFETY);
            CHECK(ota_safety_inhibited() && !unsafe_actions && !legacy_calls);
            stuck_pin = 0;
            CHECK(ota_safety_enter() == 0);
        }
    }

    /* Read-only revalidation detects hardware changes after a successful stop. */
    reset_hardware();
    CHECK(ota_safety_enter() == 0);
    previous_disables = disable_calls;
    pins[3] = 1;
    CHECK(ota_safety_check() == OTA_ERR_SAFETY && pins[3] == 1);
    pins[3] = 0; pins[4] = 0;
    CHECK(ota_safety_check() == OTA_ERR_SAFETY && pins[4] == 0);
    pins[4] = 1; safety_test_hrtim.sCommonRegs.OENR = 0x800;
    CHECK(ota_safety_check() == OTA_ERR_SAFETY);
    CHECK(disable_calls == previous_disables && !unsafe_actions && !legacy_calls);
    CHECK(ota_safety_enter() == 0);
    return 0;
}

extern "C" int ota_safety_test_run() { return test_safety(); }
#ifndef OWNTECH_FREESTANDING_TEST
int main()
{
    int result = ota_safety_test_run();
    if (result) printf("%d\n", result);
    return result != 0;
}
#endif
