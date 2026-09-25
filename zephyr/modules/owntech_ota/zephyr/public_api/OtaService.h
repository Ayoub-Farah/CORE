/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_SERVICE_H
#define OWNTECH_OTA_SERVICE_H
#include "OtaAPI.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The service starts independently of main(). No application health or
 * maintenance callback is required or invoked. OTA health describes the
 * update service, not readiness of the application's control algorithm. */
bool ota_safety_inhibited(void);
void ota_safety_restore(bool inhibit);
int ota_safety_enter(void);
/* Core-owned hardware readback; never invokes application code. */
int ota_safety_check(void);
bool ota_service_busy(void);
bool ota_service_healthy(void);
/* Local validation/confirmation may complete before a CAN peer is present.
 * healthy remains the admission gate for CAN fleet operations. */
bool ota_service_local_healthy(void);
bool ota_service_can_ready(void);
struct ota_service_diagnostics {
    const char *phase;
    int error;
    bool local_healthy, healthy, can_ready, busy, is_lead;
};
bool ota_service_is_lead(void);
void ota_feedback_state(enum ota_state state);
void ota_feedback_application_led(int action); /* 0 off, 1 on, 2 toggle */
/* Read-only status request from the existing CDC line-rate callback. */
void ota_console_request_status(void);
/* Snapshot functions are safe from USB/ThingSet threads. */
void ota_service_local(struct ota_observation *);
/* Identity and boot/network state from the same worker publication. */
void ota_service_snapshot(struct ota_observation *, struct ota_service_diagnostics *);
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
