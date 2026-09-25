/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_RECOVERY_CORE_H
#define OWNTECH_OTA_RECOVERY_CORE_H
#include <stddef.h>
#include <stdint.h>

/* Deliberately separate from OtaAPI and production runtime/storage code. */
struct OtaRecoveryBoard { uint8_t eui[8]; uint8_t original_hash[32]; };
struct OtaRecoveryConfig {
    uint64_t campaign;
    uint8_t lead_eui[8];
    uint8_t image_hash[32];
    uint32_t image_size;
    size_t board_count;
    OtaRecoveryBoard boards[16];
    /* Explicit host-proven, pre-COMMIT USB staging failure; Lead only. */
    bool staged_lead_only;
    /* Explicit pre-transfer preparation failure; a frozen follower only. */
    bool prepared_follower_only;
    /* Protocol v2 receiver journal, before any commit intention or arm. */
    bool compact_receiver_only;
    uint8_t artifact_hash[32];
};
struct OtaRecoveryIO {
    void *context;
    /* Read returns exact stored length, -2 for absent, another negative on error. */
    int (*read)(void *, uint16_t, uint8_t *, size_t);
    int (*write)(void *, uint16_t, const uint8_t *, size_t);
    int (*erase)(void *, uint16_t);
    int (*identity)(void *, uint8_t eui[8]);
    int (*backup_hash)(void *, uint8_t hash[32]);
    int (*health)(void *);
    bool (*confirmed)(void *);
    int (*confirm)(void *);
};
enum OtaRecoveryResult {
    OTA_RECOVERED = 0, OTA_RECOVERY_ALREADY_DONE = 1,
    OTA_RECOVERY_CONFIG = -10, OTA_RECOVERY_IDENTITY = -11,
    OTA_RECOVERY_BACKUP = -12, OTA_RECOVERY_JOURNAL = -13,
    OTA_RECOVERY_MAINTENANCE = -14, OTA_RECOVERY_FLEET = -15,
    OTA_RECOVERY_MARKER = -16, OTA_RECOVERY_STORAGE = -17,
    OTA_RECOVERY_HEALTH = -18, OTA_RECOVERY_CONFIRM = -19
};

uint32_t ota_recovery_crc32(const uint8_t *, size_t);
int ota_recovery_run(const OtaRecoveryConfig &, const OtaRecoveryIO &);
const char *ota_recovery_result_name(int);
#endif
