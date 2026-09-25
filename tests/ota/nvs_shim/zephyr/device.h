#pragma once
#include <stdbool.h>
struct device { const char *name; };
extern const struct device nvs_test_device;
bool device_is_ready(const struct device *);
