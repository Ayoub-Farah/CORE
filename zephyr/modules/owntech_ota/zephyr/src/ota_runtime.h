/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "OtaService.h"
#ifdef CONFIG_OWNTECH_OTA_LEAD
#include "OtaLeadService.h"
#endif
#ifdef __cplusplus
extern "C" {
#endif
int ota_runtime_command(const struct ota_command *, uint8_t source);
void ota_runtime_report(const uint8_t *, size_t, uint8_t source);
void ota_runtime_claim(const uint8_t eui[8], uint8_t address);
int ota_network_init(void);
#ifdef CONFIG_OWNTECH_OTA_LEAD
int ota_network_command(const struct ota_target *, const struct ota_command *);
int ota_network_status(uint8_t address, struct ota_observation *);
int ota_network_release(uint8_t address, const uint8_t identity[8], const uint8_t hash[32], uint64_t campaign);
#endif
int ota_runtime_release(const uint8_t identity[8], const uint8_t hash[32], uint64_t campaign, uint8_t source);
#ifdef __cplusplus
}
#endif
