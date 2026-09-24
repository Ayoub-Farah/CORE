/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_PROTOCOL_H
#define OWNTECH_OTA_PROTOCOL_H

#include "OtaAPI.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ota_report_type { OTA_REPORT_DATA = 1, OTA_REPORT_REBOOT = 2 };

struct ota_report {
    enum ota_report_type type;
    uint64_t campaign_id;
    uint32_t pass_id;
    uint32_t offset;
    uint16_t payload_len;
    const uint8_t *payload; /* Borrows the decoded input, only for this call. */
};

uint32_t ota_crc32(const uint8_t *, size_t);
/* FD rounds ONLY the last <=64-byte frame to a legal DLC. */
size_t ota_report_wire_length(size_t logical_length, bool can_fd);
int ota_report_encode(const struct ota_report *, uint8_t *output, size_t capacity,
                       size_t *logical_length);
int ota_report_decode(const uint8_t *, size_t length, bool can_fd, struct ota_report *);
int ota_report_encode_reboot(uint64_t campaign_id, uint32_t commit_id,
        uint32_t delay_ms, uint8_t *output, size_t capacity, size_t *logical_length);
uint32_t ota_read_le32(const uint8_t *);

#ifdef __cplusplus
}
#endif
#endif
