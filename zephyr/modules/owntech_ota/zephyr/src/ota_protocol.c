/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_protocol.h"
#include <string.h>

static uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
uint32_t ota_read_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8)); }
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}
uint32_t ota_crc32(const uint8_t *p, size_t n) { return ~crc_update(~0U, p, n); }
size_t ota_report_wire_length(size_t n, bool fd)
{
    if (!fd || !n) return n;
    static const uint8_t dlc[] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    size_t tail = ((n - 1) % 64) + 1, base = n - tail;
    for (size_t i = 0; i < sizeof(dlc); ++i) if (tail <= dlc[i]) return base + dlc[i];
    return 0;
}
int ota_report_encode(const struct ota_report *r, uint8_t *out, size_t cap, size_t *n)
{
    if (!r || !out || !n || !r->payload || !r->campaign_id ||
        r->payload_len == 0 || r->payload_len > OTA_MAX_PAYLOAD ||
        (r->type != OTA_REPORT_DATA && r->type != OTA_REPORT_REBOOT) ||
        (r->type == OTA_REPORT_REBOOT && (r->payload_len != 8 || r->pass_id || r->offset)) ||
        (r->type == OTA_REPORT_DATA && !r->pass_id) || cap < OTA_HEADER_SIZE + r->payload_len)
        return OTA_ERR_ARGUMENT;
    memset(out, 0, OTA_HEADER_SIZE);
    memcpy(out, "OTAC", 4); out[4] = OTA_PROTOCOL_VERSION; out[5] = r->type;
    put16(out + 6, OTA_HEADER_SIZE);
    put32(out + 8, (uint32_t)r->campaign_id); put32(out + 12, r->campaign_id >> 32);
    put32(out + 16, r->pass_id); put32(out + 20, r->offset); put16(out + 24, r->payload_len);
    memcpy(out + OTA_HEADER_SIZE, r->payload, r->payload_len);
    put32(out + 28, ~crc_update(crc_update(~0U, out, 28), r->payload, r->payload_len));
    *n = OTA_HEADER_SIZE + r->payload_len;
    return OTA_OK;
}
int ota_report_decode(const uint8_t *in, size_t n, bool fd, struct ota_report *r)
{
    if (!in || !r || n < OTA_HEADER_SIZE || n > OTA_MAX_REPORT_SIZE ||
        memcmp(in, "OTAC", 4) || in[4] != OTA_PROTOCOL_VERSION ||
        read16(in + 6) != OTA_HEADER_SIZE || read16(in + 26)) return OTA_ERR_FORMAT;
    uint16_t len = read16(in + 24);
    if (!len || len > OTA_MAX_PAYLOAD) return OTA_ERR_FORMAT;
    size_t logical = OTA_HEADER_SIZE + len;
    if (n != ota_report_wire_length(logical, fd)) return OTA_ERR_FORMAT;
    for (size_t i = logical; i < n; ++i) if (in[i]) return OTA_ERR_FORMAT;
    if (~crc_update(crc_update(~0U, in, 28), in + OTA_HEADER_SIZE, len) != ota_read_le32(in + 28))
        return OTA_ERR_CRC;
    r->type = (enum ota_report_type)in[5];
    r->campaign_id = (uint64_t)ota_read_le32(in + 8) | (uint64_t)ota_read_le32(in + 12) << 32;
    r->pass_id = ota_read_le32(in + 16); r->offset = ota_read_le32(in + 20);
    r->payload_len = len; r->payload = in + OTA_HEADER_SIZE;
    if (!r->campaign_id || (r->type != OTA_REPORT_DATA && r->type != OTA_REPORT_REBOOT) ||
        (r->type == OTA_REPORT_DATA && !r->pass_id) ||
        (r->type == OTA_REPORT_REBOOT && (r->pass_id || r->offset || len != 8))) return OTA_ERR_FORMAT;
    return OTA_OK;
}
int ota_report_encode_reboot(uint64_t campaign, uint32_t commit, uint32_t delay,
                             uint8_t *out, size_t cap, size_t *n)
{
    uint8_t payload[8]; put32(payload, commit); put32(payload + 4, delay);
    struct ota_report r = { OTA_REPORT_REBOOT, campaign, 0, 0, 8, payload };
    if (!commit || !delay) return OTA_ERR_ARGUMENT;
    return ota_report_encode(&r, out, cap, n);
}
