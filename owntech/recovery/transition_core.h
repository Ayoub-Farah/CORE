/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_TRANSITION_CORE_H
#define OWNTECH_OTA_TRANSITION_CORE_H
#include "recovery_core.h"

/* A signed, one-board terminal-v2 policy. No production-runtime dependencies. */
struct OtaTransitionConfig {
    uint8_t eui[8];
    uint8_t original_hash[32];
    uint8_t token[32];
    uint8_t role; /* 1: receiver, 2: dedicated Lead. */
    uint16_t local_length, fleet_length;
    uint8_t local[168], fleet[308];
};

int ota_transition_run(const OtaTransitionConfig &, const OtaRecoveryIO &);
#endif
