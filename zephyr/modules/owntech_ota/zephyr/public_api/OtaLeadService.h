/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "OtaService.h"
#ifdef __cplusplus
extern "C" {
#endif
int ota_service_set_role(bool lead);
int ota_service_stage_begin(const struct ota_manifest *);
int ota_service_stage_data(uint32_t offset, const uint8_t *, size_t);
int ota_service_stage_end(void);
int ota_service_start(uint64_t campaign, const uint8_t identities[][8], size_t count);
int ota_service_commit(uint64_t campaign);
int ota_service_abort(uint64_t campaign);
/* A new nonzero token refreshes an idle inventory; repeats poll the same scan.
 * Zero preserves legacy cached discovery. Discovery reserves the idle service. */
int ota_service_discover(uint64_t campaign);
int ota_service_reconcile(uint64_t campaign, const uint8_t identities[][8],
                          size_t count, const uint8_t expected_hash[32]);
int ota_service_target(size_t index, struct ota_observation *, bool *is_lead,
                       uint64_t *last_seen_ms);
size_t ota_service_target_count(void);
int ota_service_source_data(uint64_t campaign, uint32_t offset, const uint8_t *, size_t);
void ota_service_source_request(uint64_t *campaign, uint32_t *offset, uint32_t *length);
#ifdef __cplusplus
}
#endif
