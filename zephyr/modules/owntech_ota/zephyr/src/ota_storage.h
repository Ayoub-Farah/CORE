/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_STORAGE_H
#define OWNTECH_OTA_STORAGE_H
#include "OtaAPI.h"
#ifdef __cplusplus
extern "C" {
#endif
enum ota_slot_owner { OTA_SLOT_NONE, OTA_SLOT_USB, OTA_SLOT_PARTICIPANT, OTA_SLOT_LEAD };
/* Existing NVS owner only. Reserved keys 0x0500 (role), 0x0501 (maintenance),
 * 0x0502 (campaign journal), 0x0503 (frozen coordinator journal). */
struct ota_storage_journal {
    uint16_t format_version;
    uint8_t protocol_version;
    uint8_t image_class;
    uint64_t campaign_id;
    uint32_t commit_id;
    uint32_t image_size;
    enum ota_state state;
    uint8_t lead_eui[8];
    uint8_t artifact_sha256[32];
    uint8_t mcuboot_image_hash[32];
    char version[OTA_IDENTITY_TEXT_SIZE];
    char build_id[OTA_IDENTITY_TEXT_SIZE];
    uint32_t event_mask;
    uint32_t event_ms[12];
    uint8_t event_order[12];
};
int ota_storage_init(void);
bool ota_storage_recovery_required(void);
bool ota_storage_maintenance(void);
enum ota_slot_owner ota_storage_owner(void);
int ota_storage_load_role(bool *lead);
int ota_storage_persist_role(bool lead);
/* Bind the prepared campaign to its stable Lead identity before the first
 * stage/prepare. The next journal persists it before any erase. */
int ota_storage_expect_lead(const uint8_t eui[8]);
/* Retain diagnostic event history in RAM; v2 local records omit fleet history. */
void ota_storage_set_events(uint32_t mask, const uint32_t event_ms[12], const uint8_t order[12]);
void ota_storage_hooks(struct ota_participant_hooks *hooks);
void ota_storage_boot_identity(struct ota_identity *identity);
int ota_storage_active_hash(uint8_t hash[32]);
int ota_storage_get_journal(struct ota_storage_journal *journal);
int ota_storage_stage_begin(const struct ota_manifest *manifest);
int ota_storage_stage_append(uint32_t offset, const uint8_t *data, size_t length);
int ota_storage_stage_end(const struct ota_manifest *manifest);
int ota_storage_read(uint32_t offset, uint8_t *data, size_t length);
void ota_storage_abort(void);
int ota_storage_persist_campaign(const struct ota_manifest *, const struct ota_target *,
                                size_t count, uint32_t commit_id, enum ota_state);
/* V2 restores stable receiver EUIs; refresh other fields through discovery.
 * Legacy OTA1/OTA2 records require explicit recovery, never implicit adoption. */
int ota_storage_load_campaign(struct ota_manifest *, struct ota_target *, size_t *count,
                             uint32_t *commit_id);
/* Only after every frozen identity passed postboot validation. Never clears a
 * pending image, failed local health or an unconfirmed running image. */
int ota_storage_release_maintenance(const uint8_t expected_active_hash[32]);
#ifdef __cplusplus
}
#endif
#endif
