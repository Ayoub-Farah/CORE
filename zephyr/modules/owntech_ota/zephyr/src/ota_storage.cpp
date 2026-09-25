/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_storage.h"
#include "ota_protocol.h"
#include "nvs_storage.h"
#include <zephyr/kernel.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

extern "C" int ota_safety_enter(void);
extern "C" bool ota_safety_inhibited(void);
extern "C" void ota_safety_restore(bool);

#ifndef CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE
#define CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE CONFIG_OWNTECH_OTA_USABLE_SLOT_SIZE
#endif
#define OTA_SECONDARY FIXED_PARTITION_ID(slot1_partition)
#define OTA_PRIMARY FIXED_PARTITION_ID(slot0_partition)

namespace {
constexpr uint16_t ROLE_KEY = 0x0500, MAINTENANCE_KEY = 0x0501, JOURNAL_KEY = 0x0502, FLEET_KEY = 0x0503;
constexpr uint32_t JOURNAL_MAGIC = 0x3141544f; /* OTA1 maintenance/role markers remain compatible. */
constexpr uint32_t LOCAL_MAGIC = 0x324c544f; /* OTL2: fixed-width compact local journal. */
constexpr uint32_t FLEET_MAGIC = 0x3341544f; /* OTA3: protocol v2, receiver targets only. */
struct marker { uint32_t magic; uint32_t value; uint32_t crc; };
struct local_record {
    uint32_t magic;
    uint16_t version, length;
    uint8_t payload[156];
    uint32_t crc;
};
struct fleet_record {
    uint32_t magic;
    uint32_t length;
    uint8_t manifest[158];
    uint8_t eui[OTA_MAX_TARGETS][8];
    uint8_t count;
    uint8_t reserved;
    uint8_t state;
    uint32_t commit_id;
    uint32_t crc;
};
static_assert(sizeof(local_record) == 168 && offsetof(local_record, crc) == 164,
              "Local journal must not depend on ARM enum/alignment flags");
static_assert(sizeof(fleet_record) == 308 && offsetof(fleet_record, crc) == 304,
              "Fleet journal must not depend on ARM enum/alignment flags");
K_MUTEX_DEFINE(slot_mutex);
struct lock {
    lock() { k_mutex_lock(&slot_mutex, K_FOREVER); }
    ~lock() { k_mutex_unlock(&slot_mutex); }
};
flash_img_context writer;
ota_manifest current_manifest;
ota_storage_journal current_journal;
ota_slot_owner owner = OTA_SLOT_NONE;
bool initialized, recovery, maintenance, writer_open, flushed, staged, reboot_queued, journal_corrupt;
uint32_t accepted, reboot_commit;
uint8_t expected_lead_eui[8];
uint32_t event_mask, event_ms[12];
uint8_t event_order[12];
int terminal_error;

uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
void write32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i)); }
void encode_manifest(uint8_t out[158], const ota_manifest &m)
{
    write32(out, (uint32_t)m.campaign_id); write32(out + 4, (uint32_t)(m.campaign_id >> 32));
    write32(out + 8, m.image_size); write32(out + 12, m.image_content_size);
    write32(out + 16, m.hardware_id); write32(out + 20, m.layout_id); write32(out + 24, m.bootloader_id);
    out[28] = m.protocol_version;
    memcpy(out + 29, m.artifact_sha256, 32); memcpy(out + 61, m.mcuboot_image_hash, 32);
    memcpy(out + 93, m.version, 32); memcpy(out + 125, m.build_id, 32);
    out[157] = m.image_class;
}
void decode_manifest(ota_manifest &m, const uint8_t in[158])
{
    m = {};
    m.campaign_id = (uint64_t)ota_read_le32(in) | ((uint64_t)ota_read_le32(in + 4) << 32);
    m.image_size = ota_read_le32(in + 8); m.image_content_size = ota_read_le32(in + 12);
    m.hardware_id = ota_read_le32(in + 16); m.layout_id = ota_read_le32(in + 20);
    m.bootloader_id = ota_read_le32(in + 24); m.protocol_version = in[28];
    memcpy(m.artifact_sha256, in + 29, 32); memcpy(m.mcuboot_image_hash, in + 61, 32);
    memcpy(m.version, in + 93, 32); memcpy(m.build_id, in + 125, 32);
    m.image_class = in[157];
}
bool valid_fleet(const fleet_record &record)
{
    if (record.length != sizeof(record) || !record.count || record.count > OTA_MAX_TARGETS ||
        !record.commit_id || record.reserved || record.state > OTA_SUCCEEDED) return false;
    ota_manifest m; decode_manifest(m, record.manifest);
    if (m.protocol_version != OTA_PROTOCOL_VERSION || m.image_class != OTA_IMAGE_RECEIVER ||
        !m.campaign_id || !m.image_size || m.image_size != m.image_content_size ||
        m.image_size > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE) return false;
    for (size_t i = 0; i < record.count; ++i) {
        uint8_t nonzero = 0; for (size_t k = 0; k < 8; ++k) nonzero |= record.eui[i][k];
        if (!nonzero) return false;
        for (size_t j = 0; j < i; ++j) if (!memcmp(record.eui[i], record.eui[j], 8)) return false;
    }
    return true;
}
bool equal_manifest(const ota_manifest &a, const ota_manifest &b)
{
    return a.campaign_id == b.campaign_id && a.image_size == b.image_size &&
        a.image_content_size == b.image_content_size && a.protocol_version == b.protocol_version &&
        a.image_class == b.image_class &&
        a.hardware_id == b.hardware_id && a.layout_id == b.layout_id && a.bootloader_id == b.bootloader_id &&
        !memcmp(a.artifact_sha256, b.artifact_sha256, 32) && !memcmp(a.mcuboot_image_hash, b.mcuboot_image_hash, 32) &&
        !memcmp(a.version, b.version, sizeof(a.version)) && !memcmp(a.build_id, b.build_id, sizeof(a.build_id));
}
template<typename T> int store(uint16_t key, T &record)
{
    record.crc = ota_crc32(reinterpret_cast<const uint8_t *>(&record), offsetof(T, crc));
    int rc = nvs_storage_write(key, &record, sizeof(record));
    if (rc < 0) return OTA_ERR_JOURNAL;
    T check = {};
    rc = nvs_storage_read(key, &check, sizeof(check));
    return rc == static_cast<int>(sizeof(check)) && !memcmp(&check, &record, sizeof(check)) ? OTA_OK : OTA_ERR_JOURNAL;
}
template<typename T> int load(uint16_t key, T &record, uint32_t magic = JOURNAL_MAGIC)
{
    int rc = nvs_storage_read(key, &record, sizeof(record));
    if (rc == -ENOENT) return rc;
    if (rc != static_cast<int>(sizeof(record)) || record.magic != magic || record.crc !=
        ota_crc32(reinterpret_cast<const uint8_t *>(&record), offsetof(T, crc))) return OTA_ERR_JOURNAL;
    return OTA_OK;
}
int save_marker(uint16_t key, bool value)
{ marker m = {JOURNAL_MAGIC, value ? 1U : 0U, 0}; return store(key, m); }
void encode_local(local_record &record, const ota_storage_journal &j)
{
    record = {}; record.magic = LOCAL_MAGIC; record.version = 2; record.length = sizeof(record);
    uint8_t *p = record.payload;
    write32(p, (uint32_t)j.campaign_id); write32(p + 4, (uint32_t)(j.campaign_id >> 32));
    write32(p + 8, j.commit_id); write32(p + 12, j.image_size);
    p[16] = (uint8_t)j.state; p[17] = j.protocol_version; p[18] = j.image_class;
    memcpy(p + 20, j.lead_eui, 8); memcpy(p + 28, j.artifact_sha256, 32);
    memcpy(p + 60, j.mcuboot_image_hash, 32);
    memcpy(p + 92, j.version, 32); memcpy(p + 124, j.build_id, 32);
}
int decode_local(const local_record &record, ota_storage_journal &j)
{
    const uint8_t *p = record.payload;
    if (record.version != 2 || record.length != sizeof(record) || p[16] > OTA_COMMIT_INTENT ||
        p[17] != OTA_PROTOCOL_VERSION || p[18] != OTA_IMAGE_RECEIVER || p[19]) return OTA_ERR_JOURNAL;
    j = {}; j.format_version = record.version;
    j.campaign_id = (uint64_t)ota_read_le32(p) | ((uint64_t)ota_read_le32(p + 4) << 32);
    j.commit_id = ota_read_le32(p + 8); j.image_size = ota_read_le32(p + 12);
    j.state = (ota_state)p[16]; j.protocol_version = p[17]; j.image_class = p[18];
    memcpy(j.lead_eui, p + 20, 8); memcpy(j.artifact_sha256, p + 28, 32);
    memcpy(j.mcuboot_image_hash, p + 60, 32);
    memcpy(j.version, p + 92, 32); memcpy(j.build_id, p + 124, 32);
    uint8_t nonzero = 0; for (size_t i = 0; i < 8; ++i) nonzero |= j.lead_eui[i];
    return j.campaign_id && j.image_size && j.image_size <= CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE &&
        nonzero ? OTA_OK : OTA_ERR_JOURNAL;
}
int save_journal(const ota_manifest *m, ota_state state, uint32_t commit)
{
    ota_storage_journal next = {};
    next.format_version = 2; next.protocol_version = m->protocol_version; next.image_class = m->image_class;
    next.campaign_id = m->campaign_id; next.commit_id = commit; next.state = state; next.image_size = m->image_size;
    memcpy(next.lead_eui, expected_lead_eui, 8);
    memcpy(next.artifact_sha256, m->artifact_sha256, 32);
    memcpy(next.mcuboot_image_hash, m->mcuboot_image_hash, 32);
    memcpy(next.version, m->version, OTA_IDENTITY_TEXT_SIZE);
    memcpy(next.build_id, m->build_id, OTA_IDENTITY_TEXT_SIZE);
    next.event_mask = event_mask; memcpy(next.event_ms, event_ms, sizeof(event_ms));
    memcpy(next.event_order, event_order, sizeof(event_order));
    local_record record; encode_local(record, next);
    int rc = store(JOURNAL_KEY, record);
    if (!rc) current_journal = next;
    return rc;
}
bool boot_available() { return boot_is_img_confirmed() && mcuboot_swap_type() == BOOT_SWAP_TYPE_NONE; }
int reserve_journal_space(ota_slot_owner desired)
{
    /* NVS has one spare erase sector: the advertised 4KiB is not all usable
     * live data. Reserve the worst-case frozen fleet without deleting or
     * relocating calibration/metadata keys. NVS copies the old live value into
     * the GC sector before appending its replacement: reserve both, not only
     * final live-data growth. Do not credit shrinking an OTA1 fleet until its
     * compact replacement is durable. Reserve 8 B for a future GC marker and
     * 16 B for the NVS version key that the first write may create implicitly.
     * Other application NVS writers must remain stopped during maintenance. */
    size_t needed = 8 + 16, overwrite = 0;
    const uint16_t keys[] = {MAINTENANCE_KEY, JOURNAL_KEY, FLEET_KEY};
    const size_t sizes[] = {sizeof(marker), sizeof(local_record), sizeof(fleet_record)};
    unsigned first = desired == OTA_SLOT_LEAD ? 2 : 0;
    unsigned count = desired == OTA_SLOT_PARTICIPANT ? 2 : 3;
    for (unsigned i = first; i < count; ++i) {
        uint8_t first;
        int old_size = nvs_storage_read(keys[i], &first, sizeof(first));
        if (old_size < 0 && old_size != -ENOENT) return OTA_ERR_JOURNAL;
        size_t old = old_size < 0 ? 0 : (((size_t)old_size + 7) & ~(size_t)7) + 8;
        size_t next = ((sizes[i] + 7) & ~(size_t)7) + 8;
        if (next > old) needed += next - old;
        if (next > overwrite) overwrite = next;
    }
    needed += overwrite;
    int32_t free = nvs_storage_get_free_space();
    return free < 0 || (size_t)free < needed ? OTA_ERR_JOURNAL : OTA_OK;
}
void close_writer()
{
    if (writer_open && writer.flash_area) flash_area_close(writer.flash_area);
    writer.flash_area = nullptr; writer_open = false;
}
int storage_failure(int rc)
{
    terminal_error = rc; recovery = true; close_writer();
    (void)save_journal(&current_manifest, OTA_FAILED, current_journal.commit_id);
    return rc;
}
/* Structural validation is deliberately separate from signature authority.
 * MCUboot checks the image signature with its installed key at boot. */
int image_info(const flash_area *area, uint32_t bound, uint32_t *content_end, uint8_t hash[32],
               uint8_t expected_class = 0)
{
    uint8_t header[32], info[4];
    if (bound < sizeof(header) || bound > area->fa_size || flash_area_read(area, 0, header, sizeof(header)))
        return OTA_ERR_IMAGE;
    if (ota_read_le32(header) != 0x96f3b83dU) return OTA_ERR_FORMAT;
    uint32_t hsize = read16(header + 8), protected_size = read16(header + 10), size = ota_read_le32(header + 12);
    /* This prototype does not implement encrypted/compressed/RAM-load images. */
    if (hsize < 32 || hsize > bound || size > bound - hsize || ota_read_le32(header + 16)) return OTA_ERR_FORMAT;
    uint32_t pos = hsize + size;
    bool class_found = false;
    if (protected_size) {
        if (protected_size < 4 || protected_size > bound - pos || flash_area_read(area, pos, info, 4) ||
            read16(info) != 0x6908 || read16(info + 2) != protected_size) return OTA_ERR_FORMAT;
        uint32_t end = pos + protected_size; pos += 4;
        while (pos < end) {
            if (end - pos < 4 || flash_area_read(area, pos, info, 4)) return OTA_ERR_FORMAT;
            uint32_t len = read16(info + 2); pos += 4;
            if (len > end - pos || info[1]) return OTA_ERR_FORMAT;
            if (info[0] == 0xa0) {
                uint8_t value[8];
                if (class_found || (len != 8 && len != 4) || flash_area_read(area, pos, value, len))
                    return OTA_ERR_FORMAT;
                uint8_t actual = len == 8 && !memcmp(value, "receiver", 8) ? OTA_IMAGE_RECEIVER :
                    len == 4 && !memcmp(value, "lead", 4) ? OTA_IMAGE_LEAD : 0;
                if (!actual || (expected_class && expected_class != actual)) return OTA_ERR_COMPATIBILITY;
                class_found = true;
            }
            pos += len;
        }
    }
    if (expected_class && !class_found) return OTA_ERR_COMPATIBILITY;
    if (bound - pos < 4 || flash_area_read(area, pos, info, 4) || read16(info) != 0x6907) return OTA_ERR_FORMAT;
    uint32_t total = read16(info + 2);
    if (total < 4 || total > bound - pos) return OTA_ERR_FORMAT;
    uint32_t end = pos + total; pos += 4; bool found = false;
    while (pos < end) {
        if (end - pos < 4 || flash_area_read(area, pos, info, 4)) return OTA_ERR_FORMAT;
        uint32_t len = read16(info + 2); pos += 4;
        if (len > end - pos || info[1]) return OTA_ERR_FORMAT;
        if (info[0] == 0xa0) return OTA_ERR_FORMAT; /* Class must be covered by the signature. */
        if (info[0] == 0x10) {
            if (len != 32 || found || flash_area_read(area, pos, hash, 32)) return OTA_ERR_FORMAT;
            found = true;
        }
        pos += len;
    }
    if (!found) return OTA_ERR_FORMAT;
    *content_end = end;
    return OTA_OK;
}
int erased_range(const flash_area *area, uint32_t offset)
{
    uint8_t block[128];
    while (offset < area->fa_size) {
        size_t n = area->fa_size - offset; if (n > sizeof(block)) n = sizeof(block);
        if (flash_area_read(area, offset, block, n)) return OTA_ERR_STORAGE;
        for (size_t i = 0; i < n; ++i) if (block[i] != 0xff) return OTA_ERR_IMAGE;
        offset += n;
    }
    return OTA_OK;
}
int validate(const ota_manifest *m)
{
    if (writer_open || !flushed || accepted != m->image_size) return OTA_ERR_INCOMPLETE;
    struct flash_img_check check = {m->artifact_sha256, m->image_size};
    if (flash_img_check(&writer, &check, OTA_SECONDARY)) return OTA_ERR_IMAGE;
    const flash_area *area;
    if (flash_area_open(OTA_SECONDARY, &area)) return OTA_ERR_STORAGE;
    uint8_t hash[32]; uint32_t content = 0;
    int rc = image_info(area, m->image_content_size, &content, hash, m->image_class);
    if (!rc && (content != m->image_content_size || content > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE ||
                memcmp(hash, m->mcuboot_image_hash, 32))) rc = OTA_ERR_IMAGE;
    if (!rc && m->image_size != content) rc = OTA_ERR_FORMAT;
    /* flash_img may pad its final write buffer, but the useful bound is below
     * every trailer word. begin/append never program those erased ECC cells. */
    if (!rc) rc = erased_range(area, m->image_size);
    flash_area_close(area);
    return rc;
}
int begin(const ota_manifest *m, ota_slot_owner desired)
{
    if (!initialized || recovery || owner != OTA_SLOT_NONE || !m || !m->campaign_id) return OTA_ERR_STATE;
#if defined(CONFIG_OWNTECH_OTA_LEAD) && CONFIG_OWNTECH_OTA_LEAD
    /* A dedicated Lead must never stage receiver firmware into a boot slot. */
    return OTA_ERR_COMPATIBILITY;
#endif
#if !defined(CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED) || !CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED
    /* Installed bootloader geometry/algorithm must be qualified explicitly. */
    return OTA_ERR_COMPATIBILITY;
#endif
    uint8_t lead_nonzero = 0; for (size_t i = 0; i < 8; ++i) lead_nonzero |= expected_lead_eui[i];
    if (!lead_nonzero) return OTA_ERR_IDENTITY;
    if (!m->image_content_size || m->image_content_size > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE ||
        m->image_content_size != m->image_size ||
        m->image_size > FIXED_PARTITION_SIZE(slot1_partition) ||
        ((m->image_size + sizeof(writer.buf) - 1) / sizeof(writer.buf)) * sizeof(writer.buf) >
            CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE) return OTA_ERR_CAPACITY;
    if (m->image_class != OTA_IMAGE_RECEIVER || m->protocol_version != OTA_PROTOCOL_VERSION ||
        m->hardware_id != CONFIG_OWNTECH_OTA_HARDWARE_ID ||
        m->layout_id != CONFIG_OWNTECH_OTA_LAYOUT_ID || m->bootloader_id != CONFIG_OWNTECH_OTA_BOOTLOADER_ID)
        return OTA_ERR_COMPATIBILITY;
    /* This check is inside the same mutex as ownership and the first erase. */
    if (!boot_available()) return OTA_ERR_STATE;
    if (reserve_journal_space(desired)) return OTA_ERR_JOURNAL;
    ota_safety_restore(true); maintenance = true;
    if (ota_safety_enter() || !ota_safety_inhibited()) return OTA_ERR_SAFETY;
    if (save_marker(MAINTENANCE_KEY, true)) { recovery = true; return OTA_ERR_JOURNAL; }
    current_manifest = *m; owner = desired; accepted = 0; flushed = staged = false; terminal_error = 0;
    if (save_journal(m, OTA_PREPARING, 0)) return storage_failure(OTA_ERR_JOURNAL);
    /* No progressive erase is needed: erasing here makes the safety ordering
     * explicit, and flash_img only writes the exact sequential artifact. */
    const flash_area *area;
    if (flash_area_open(OTA_SECONDARY, &area)) return storage_failure(OTA_ERR_STORAGE);
    int rc = flash_area_erase(area, 0, area->fa_size);
    if (!rc) rc = erased_range(area, 0);
    flash_area_close(area);
    if (rc || flash_img_init_id(&writer, OTA_SECONDARY)) return storage_failure(OTA_ERR_STORAGE);
    writer_open = true;
    return OTA_OK;
}
int append(uint32_t offset, const uint8_t *data, size_t n)
{
    if (terminal_error) return terminal_error;
    if (!writer_open || !data || !n || n > OTA_MAX_PAYLOAD || offset > current_manifest.image_size ||
        n > current_manifest.image_size - offset) return OTA_ERR_OFFSET;
    /* SMP retries must contain the same bytes, including a tail buffered by
     * flash_img. A same-offset different payload must never be acknowledged. */
    if (offset < accepted) {
        if (n > accepted - offset) return OTA_ERR_OFFSET;
        size_t written = flash_img_bytes_written(&writer), committed = 0;
        uint8_t check[OTA_MAX_PAYLOAD];
        if (offset < written) {
            committed = written - offset; if (committed > n) committed = n;
            if (flash_area_read(writer.flash_area, offset, check, committed)) return OTA_ERR_STORAGE;
            if (memcmp(check, data, committed)) return OTA_ERR_CONFLICT;
        }
        if (committed < n) {
            size_t buffered_offset = offset + committed - written;
            if (buffered_offset + n - committed > sizeof(writer.buf) ||
                memcmp(writer.buf + buffered_offset, data + committed, n - committed)) return OTA_ERR_CONFLICT;
        }
        return OTA_OK;
    }
    if (offset != accepted) return OTA_ERR_OFFSET;
    if (flash_img_buffered_write(&writer, data, n, false)) return storage_failure(OTA_ERR_STORAGE);
    accepted += n; return OTA_OK;
}
int flush()
{
    if (terminal_error) return terminal_error;
    if (flushed) return OTA_OK;
    if (!writer_open || accepted != current_manifest.image_size) return OTA_ERR_INCOMPLETE;
    uint8_t dummy = 0;
    int rc = flash_img_buffered_write(&writer, &dummy, 0, true);
    writer_open = false; flushed = true;
    if (rc) return storage_failure(OTA_ERR_STORAGE);
    return OTA_OK;
}
void reboot_work(struct k_work *) { sys_reboot(SYS_REBOOT_COLD); }
K_WORK_DELAYABLE_DEFINE(reboot_timer, reboot_work);
int hook_prepare(void *, const ota_manifest *m, bool adopt)
{
    lock guard;
    if (adopt) return OTA_ERR_COMPATIBILITY;
    return begin(m, OTA_SLOT_PARTICIPANT);
}
int hook_append(void *, uint32_t offset, const uint8_t *data, size_t n)
{ lock guard; return owner == OTA_SLOT_PARTICIPANT ? append(offset, data, n) : OTA_ERR_STATE; }
int hook_flush(void *) { lock guard; return flush(); }
int hook_validate(void *, const ota_manifest *m)
{ lock guard; return equal_manifest(current_manifest, *m) ? validate(m) : OTA_ERR_CONFLICT; }
int hook_journal(void *, const ota_manifest *m, ota_state state, uint32_t commit)
{ lock guard; return save_journal(m, state, commit); }
int hook_arm(void *, const ota_manifest *m, uint32_t commit)
{
    lock guard;
    if (owner != OTA_SLOT_PARTICIPANT || writer_open || !flushed || !maintenance ||
        !ota_safety_inhibited() || !equal_manifest(current_manifest, *m) ||
        current_journal.state != OTA_COMMIT_INTENT || !commit || current_journal.commit_id != commit ||
        !boot_available()) return OTA_ERR_STATE;
    /* Persisted intention precedes the first trailer write. boot_request_upgrade
     * owns trailer geometry; a partial write remains maintenance/recovery. */
    const flash_area *area;
    if (flash_area_open(OTA_SECONDARY, &area)) return OTA_ERR_STORAGE;
    int rc = erased_range(area, m->image_size); flash_area_close(area);
    /* MCUboot's boot_set_next may erase a secondary with bad magic. Refuse a
     * changed trailer here instead of invoking that destructive recovery path. */
    if (rc) return OTA_ERR_STORAGE;
    if (boot_request_upgrade(BOOT_UPGRADE_TEST) || mcuboot_swap_type() != BOOT_SWAP_TYPE_TEST)
        return OTA_ERR_STORAGE;
    return OTA_OK;
}
int hook_reboot(void *, uint32_t commit, uint32_t delay)
{
    lock guard;
    if (reboot_queued) return commit == reboot_commit ? OTA_OK : OTA_ERR_CONFLICT;
    if (!commit || !delay || delay > 120000 || current_journal.commit_id != commit ||
        current_journal.state != OTA_REBOOTING) return OTA_ERR_STATE;
    reboot_commit = commit; reboot_queued = true;
    int rc = k_work_schedule(&reboot_timer, K_MSEC(delay));
    if (rc < 0) { reboot_queued = false; return OTA_ERR_STATE; }
    return OTA_OK;
}
void hook_close(void *) { lock guard; close_writer(); recovery = maintenance; }
} // namespace

int ota_storage_init(void)
{
    lock guard;
    if (initialized) return recovery ? OTA_ERR_STATE : OTA_OK;
    initialized = true;
    marker m = {}; int rc = load(MAINTENANCE_KEY, m);
    maintenance = rc == -ENOENT ? false : (rc != OTA_OK || m.value != 0);
    recovery = maintenance;
    /* Safety starts inhibited at reset. A healthy application explicitly
     * releases that RAM gate later, even when no persisted campaign exists. */
    if (maintenance) ota_safety_restore(true);
    local_record record = {};
    int jr = load(JOURNAL_KEY, record, LOCAL_MAGIC);
    if (!jr) {
        jr = decode_local(record, current_journal);
        if (!jr) {
            memcpy(expected_lead_eui, current_journal.lead_eui, 8);
            if (current_journal.state != OTA_SUCCEEDED) maintenance = recovery = true;
        }
    }
    uint32_t fleet_header[2] = {};
    int fleet_size = nvs_storage_read(FLEET_KEY, fleet_header, sizeof(fleet_header));
#if defined(CONFIG_OWNTECH_OTA_LEAD) && CONFIG_OWNTECH_OTA_LEAD
    bool invalid_fleet = fleet_size >= 0 && fleet_header[0] != FLEET_MAGIC;
    if (fleet_size >= 0 && !invalid_fleet) {
        fleet_record fleet = {};
        invalid_fleet = load(FLEET_KEY, fleet, FLEET_MAGIC) || !valid_fleet(fleet);
    }
#else
    /* A receiver never owns a fleet journal. Preserve a stale Lead/legacy
     * record for explicit recovery, without allocating or decoding its roster. */
    bool invalid_fleet = fleet_size >= 0;
#endif
    /* No implicit ABI migration: preserve all old/corrupt bytes for the
     * explicit recovery utility. They can never identify a v2 campaign. */
    if ((rc && rc != -ENOENT) || (jr && jr != -ENOENT) || invalid_fleet ||
        (fleet_size < 0 && fleet_size != -ENOENT)) {
        maintenance = recovery = journal_corrupt = true; ota_safety_restore(true); return OTA_ERR_JOURNAL;
    }
    if (maintenance) ota_safety_restore(true);
    return OTA_OK;
}
bool ota_storage_recovery_required(void) { lock guard; return recovery; }
bool ota_storage_maintenance(void) { lock guard; return maintenance; }
ota_slot_owner ota_storage_owner(void) { lock guard; return owner; }
int ota_storage_load_role(bool *lead)
{
    if (!lead) return OTA_ERR_ARGUMENT;
    lock guard; marker m = {}; int rc = load(ROLE_KEY, m);
    if (rc == -ENOENT) { *lead = false; return OTA_OK; }
    if (rc || m.value > 1) return OTA_ERR_JOURNAL;
    *lead = m.value == 1; return OTA_OK;
}
int ota_storage_persist_role(bool lead)
{
    lock guard;
    if (owner != OTA_SLOT_NONE || maintenance || recovery) return OTA_ERR_STATE;
    return save_marker(ROLE_KEY, lead);
}
int ota_storage_expect_lead(const uint8_t eui[8])
{
    if (!eui) return OTA_ERR_ARGUMENT;
    uint8_t nonzero = 0; for (size_t i = 0; i < 8; ++i) nonzero |= eui[i];
    if (!nonzero) return OTA_ERR_IDENTITY;
    lock guard;
    if ((owner != OTA_SLOT_NONE || recovery) && memcmp(eui, expected_lead_eui, 8)) return OTA_ERR_CONFLICT;
    memcpy(expected_lead_eui, eui, 8); return OTA_OK;
}
void ota_storage_set_events(uint32_t mask, const uint32_t timestamps[12], const uint8_t order[12])
{
    lock guard;
    event_mask = mask;
    if (timestamps) memcpy(event_ms, timestamps, sizeof(event_ms));
    else memset(event_ms, 0, sizeof(event_ms));
    if (order) memcpy(event_order, order, sizeof(event_order));
    else memset(event_order, 0, sizeof(event_order));
}
void ota_storage_hooks(ota_participant_hooks *hooks)
{
    *hooks = {nullptr, hook_prepare, hook_append, hook_flush, hook_validate,
              hook_journal, hook_reboot, hook_close, hook_arm};
}
void ota_storage_boot_identity(ota_identity *id)
{
    lock guard;
    id->protocol_version = OTA_PROTOCOL_VERSION; id->usable_slot_size = FIXED_PARTITION_SIZE(slot1_partition);
#if defined(CONFIG_OWNTECH_OTA_LEAD) && CONFIG_OWNTECH_OTA_LEAD
    id->image_class = OTA_IMAGE_LEAD;
#else
    id->image_class = OTA_IMAGE_RECEIVER;
#endif
    id->usable_image_size = CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE;
    id->hardware_id = CONFIG_OWNTECH_OTA_HARDWARE_ID; id->layout_id = CONFIG_OWNTECH_OTA_LAYOUT_ID;
    id->bootloader_id = CONFIG_OWNTECH_OTA_BOOTLOADER_ID;
    id->active_confirmed = boot_is_img_confirmed();
    id->slot_available = owner == OTA_SLOT_NONE && !recovery && boot_available();
}
int ota_storage_active_hash(uint8_t hash[32])
{
    lock guard; const flash_area *area;
    if (!hash || flash_area_open(OTA_PRIMARY, &area)) return OTA_ERR_STORAGE;
    uint32_t end; int rc = image_info(area, area->fa_size, &end, hash); flash_area_close(area); return rc;
}
int ota_storage_get_journal(ota_storage_journal *journal)
{ if (!journal) return OTA_ERR_ARGUMENT; lock guard; *journal = current_journal; return OTA_OK; }
int ota_storage_stage_begin(const ota_manifest *m)
{
    lock guard;
    if (m && owner == OTA_SLOT_USB && equal_manifest(current_manifest, *m))
        return terminal_error ? terminal_error : OTA_OK;
    return begin(m, OTA_SLOT_USB);
}
int ota_storage_stage_append(uint32_t offset, const uint8_t *data, size_t n)
{ lock guard; return owner == OTA_SLOT_USB ? append(offset, data, n) : OTA_ERR_STATE; }
int ota_storage_stage_end(const ota_manifest *m)
{
    lock guard;
    if (!m || owner != OTA_SLOT_USB || !equal_manifest(current_manifest, *m)) return OTA_ERR_CONFLICT;
    if (staged) return OTA_OK;
    int rc = flush(); if (rc) return rc;
    rc = validate(m); if (rc) return storage_failure(rc);
    rc = save_journal(m, OTA_VALID, 0); if (rc) return storage_failure(rc);
    staged = true; return OTA_OK;
}
int ota_storage_read(uint32_t offset, uint8_t *data, size_t n)
{
    lock guard;
    if (!staged || writer_open || (owner != OTA_SLOT_USB && owner != OTA_SLOT_LEAD) ||
        !data || offset > current_manifest.image_size || n > current_manifest.image_size - offset) return OTA_ERR_STATE;
    const flash_area *area; if (flash_area_open(OTA_SECONDARY, &area)) return OTA_ERR_STORAGE;
    int rc = flash_area_read(area, offset, data, n); flash_area_close(area);
    return rc ? OTA_ERR_STORAGE : OTA_OK;
}
void ota_storage_abort(void)
{
    lock guard; close_writer(); recovery = maintenance;
    /* Preserve commit evidence if any trailer write may have happened. */
    if (current_manifest.campaign_id && !current_journal.commit_id)
        (void)save_journal(&current_manifest, OTA_ABORTED, 0);
}
int ota_storage_persist_campaign(const ota_manifest *m, const ota_target *targets,
                                size_t count, uint32_t commit, ota_state state)
{
    if (!m || !targets || !count || count > OTA_MAX_TARGETS || !commit) return OTA_ERR_ARGUMENT;
    lock guard;
    if (m->protocol_version != OTA_PROTOCOL_VERSION || m->image_class != OTA_IMAGE_RECEIVER ||
        !m->campaign_id || !m->image_size || m->image_size != m->image_content_size ||
        m->image_size > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE) return OTA_ERR_COMPATIBILITY;
    if (reserve_journal_space(OTA_SLOT_LEAD)) return OTA_ERR_JOURNAL;
    fleet_record record = {}; record.magic = FLEET_MAGIC; record.length = sizeof(record);
    encode_manifest(record.manifest, *m);
    for (size_t i = 0; i < count; ++i) {
        if (targets[i].is_lead) return OTA_ERR_IDENTITY;
        uint8_t nonzero = 0;
        for (size_t k = 0; k < 8; ++k) nonzero |= targets[i].identity.eui[k];
        if (!nonzero) return OTA_ERR_IDENTITY;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(targets[i].identity.eui, record.eui[j], 8)) return OTA_ERR_IDENTITY;
        memcpy(record.eui[i], targets[i].identity.eui, 8);
    }
    record.count = (uint8_t)count; record.commit_id = commit; record.state = (uint8_t)state;
    return store(FLEET_KEY, record);
}
int ota_storage_load_campaign(ota_manifest *m, ota_target *targets, size_t *count, uint32_t *commit)
{
    if (!m || !targets || !count || !commit) return OTA_ERR_ARGUMENT;
    lock guard;
    uint32_t header[2] = {};
    int rc = nvs_storage_read(FLEET_KEY, header, sizeof(header));
    if (rc < 0) return rc == -ENOENT ? rc : OTA_ERR_JOURNAL;
    if (rc != static_cast<int>(sizeof(fleet_record)) || header[0] != FLEET_MAGIC) return OTA_ERR_JOURNAL;
    fleet_record record = {}; rc = load(FLEET_KEY, record, FLEET_MAGIC);
    if (rc) return rc;
    if (!valid_fleet(record) || record.count > *count) return OTA_ERR_JOURNAL;
    decode_manifest(*m, record.manifest); *count = record.count; *commit = record.commit_id;
    memset(targets, 0, record.count * sizeof(*targets));
    for (size_t i = 0; i < record.count; ++i) {
        memcpy(targets[i].identity.eui, record.eui[i], 8);
        targets[i].identity.image_class = OTA_IMAGE_RECEIVER;
    }
    return OTA_OK;
}
int ota_storage_release_maintenance(const uint8_t expected_hash[32])
{
    lock guard;
    uint8_t active[32];
    if (!expected_hash || journal_corrupt || writer_open || reboot_queued || !boot_available() ||
        ota_storage_active_hash(active) || memcmp(active, expected_hash, 32)) return OTA_ERR_STATE;
    /* Retain complete campaign provenance and result across a subsequent boot.
     * The independent inhibition marker is cleared only after this succeeds. */
    ota_storage_journal next = current_journal; next.state = OTA_SUCCEEDED;
    local_record record; encode_local(record, next);
    if (current_journal.campaign_id && store(JOURNAL_KEY, record)) return OTA_ERR_JOURNAL;
    if (save_marker(MAINTENANCE_KEY, false)) return OTA_ERR_JOURNAL;
    current_journal = next;
    maintenance = recovery = false; owner = OTA_SLOT_NONE; ota_safety_restore(false);
    return OTA_OK;
}
