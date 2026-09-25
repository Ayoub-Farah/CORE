#pragma once
#include <stdint.h>
struct SafetyTestHrtim { struct { uint32_t OENR; } sCommonRegs; };
extern SafetyTestHrtim safety_test_hrtim;
#define HRTIM1 (&safety_test_hrtim)
void LL_HRTIM_DisableOutput(SafetyTestHrtim *timer, uint32_t outputs);
