/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_SERVICE_H
#define OWNTECH_OTA_SERVICE_H
#include "OtaAPI.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Weak defaults fail closed. The application supplies maintenance and health
 * checks appropriate to its behavior; see the example hooks in src/main.cpp. */
int owntech_ota_enter_maintenance(void);
int owntech_ota_check_health(void);
bool ota_safety_inhibited(void);
void ota_safety_restore(bool inhibit);
int ota_safety_enter(void);
bool ota_service_busy(void);
bool ota_service_healthy(void);
bool ota_service_is_lead(void);
void ota_feedback_state(enum ota_state state);
void ota_feedback_application_led(int action); /* 0 off, 1 on, 2 toggle */
int ota_service_set_role(bool lead);
int ota_service_stage_begin(const struct ota_manifest *);
int ota_service_stage_data(uint32_t offset, const uint8_t *, size_t);
int ota_service_stage_end(void);
int ota_service_start(uint64_t campaign, const uint8_t identities[][8], size_t count);
int ota_service_commit(uint64_t campaign);
int ota_service_abort(uint64_t campaign);
int ota_service_discover(void);
int ota_service_reconcile(uint64_t campaign, const uint8_t identities[][8],
                          size_t count, const uint8_t expected_hash[32]);
/* Snapshot functions are safe from USB/ThingSet threads. */
void ota_service_local(struct ota_observation *);
int ota_service_target(size_t index, struct ota_observation *, bool *is_lead,
                       uint64_t *last_seen_ms);
size_t ota_service_target_count(void);
const char *ota_service_phase(void);
const char *ota_state_name(enum ota_state);
uint32_t ota_service_pass(void);
uint64_t ota_service_campaign(void);
uint32_t ota_service_stage_offset(void);
int ota_service_error(void);
#ifdef __cplusplus
}
#endif
#endif
