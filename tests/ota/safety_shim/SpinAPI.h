/* Hardware boundary for the real OTA safety implementation. */
#pragma once
#include <stdint.h>

enum { GPIO_OUTPUT_INACTIVE = 0, GPIO_OUTPUT_ACTIVE = 1 };
typedef uint32_t gpio_flags_t;
struct SafetyTestGpio {
    void configurePin(uint8_t pin, gpio_flags_t flags);
    uint8_t readPin(uint8_t pin);
};
struct SafetyTestSpin { SafetyTestGpio gpio; };
extern SafetyTestSpin spin;
