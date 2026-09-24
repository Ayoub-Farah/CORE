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
constexpr uint32_t JOURNAL_MAGIC = 0x3141544f; /* OTA1 */
constexpr uint32_t FLEET_MAGIC = 0x3241544f; /* OTA2: fixed-width compact fleet */
struct marker { uint32_t magic; uint32_t value; uint32_t crc; };
struct local_record { uint32_t magic; ota_storage_journal journal; uint32_t crc; };
/* Preserve the deployed OTA1 layout for recovery. New records only retain
 * stable EUIs; addresses and availability must be rediscovered after reboot. */
struct legacy_fleet_record {
    uint32_t magic;
    uint32_t length;
    ota_manifest manifest;
    ota_target targets[OTA_MAX_TARGETS];
    uint32_t commit_id;
    uint32_t count;
    enum ota_state state;
    uint32_t crc;
};
struct fleet_record {
    uint32_t magic;
    uint32_t length;
    uint8_t manifest[157];
    uint8_t eui[OTA_MAX_TARGETS][8];
    uint8_t count;
    uint8_t lead_index;
    uint8_t state;
    uint32_t commit_id;
    uint32_t crc;
};
static_assert(sizeof(legacy_fleet_record) == 824 && offsetof(legacy_fleet_record, crc) == 820,
              "OTA1 fleet recovery requires its deployed layout");
static_assert(sizeof(fleet_record) == 304 && offsetof(fleet_record, crc) == 300,
              "OTA2 fleet is a fixed-width record without padding");
K_MUTEX_DEFINE(slot_mutex);
struct lock {
    lock() { k_mutex_lock(&slot_mutex, K_FOREVER); }
    ~lock() { k_mutex_unlock(&slot_mutex); }
};
flash_img_context writer;
ota_manifest current_manifest;
ota_storage_journal current_journal;
ota_slot_owner owner = OTA_SLOT_NONE;
bool initialized, recovery, maintenance, writer_open, flushed, staged, reboot_queued;
uint32_t accepted, reboot_commit;
uint8_t expected_lead_eui[8];
uint32_t event_mask, event_ms[12];
uint8_t event_order[12];
int terminal_error;

uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
void write32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i)); }
void encode_manifest(uint8_t out[157], const ota_manifest &m)
{
    write32(out, (uint32_t)m.campaign_id); write32(out + 4, (uint32_t)(m.campaign_id >> 32));
    write32(out + 8, m.image_size); write32(out + 12, m.image_content_size);
    write32(out + 16, m.hardware_id); write32(out + 20, m.layout_id); write32(out + 24, m.bootloader_id);
    out[28] = m.protocol_version;
    memcpy(out + 29, m.artifact_sha256, 32); memcpy(out + 61, m.mcuboot_image_hash, 32);
    memcpy(out + 93, m.version, 32); memcpy(out + 125, m.build_id, 32);
}
void decode_manifest(ota_manifest &m, const uint8_t in[157])
{
    m = {};
    m.campaign_id = (uint64_t)ota_read_le32(in) | ((uint64_t)ota_read_le32(in + 4) << 32);
    m.image_size = ota_read_le32(in + 8); m.image_content_size = ota_read_le32(in + 12);
    m.hardware_id = ota_read_le32(in + 16); m.layout_id = ota_read_le32(in + 20);
    m.bootloader_id = ota_read_le32(in + 24); m.protocol_version = in[28];
    memcpy(m.artifact_sha256, in + 29, 32); memcpy(m.mcuboot_image_hash, in + 61, 32);
    memcpy(m.version, in + 93, 32); memcpy(m.build_id, in + 125, 32);
}
bool equal_manifest(const ota_manifest &a, const ota_manifest &b)
{
    return a.campaign_id == b.campaign_id && a.image_size == b.image_size &&
        a.image_content_size == b.image_content_size && a.protocol_version == b.protocol_version &&
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
int save_journal(const ota_manifest *m, ota_state state, uint32_t commit)
{
    local_record record = {};
    record.magic = JOURNAL_MAGIC; record.journal.campaign_id = m->campaign_id;
    record.journal.commit_id = commit; record.journal.state = state;
    record.journal.image_size = m->image_size;
    memcpy(record.journal.lead_eui, expected_lead_eui, 8);
    memcpy(record.journal.artifact_sha256, m->artifact_sha256, 32);
    memcpy(record.journal.mcuboot_image_hash, m->mcuboot_image_hash, 32);
    memcpy(record.journal.version, m->version, OTA_IDENTITY_TEXT_SIZE);
    memcpy(record.journal.build_id, m->build_id, OTA_IDENTITY_TEXT_SIZE);
    record.journal.event_mask = event_mask;
    memcpy(record.journal.event_ms, event_ms, sizeof(event_ms));
    memcpy(record.journal.event_order, event_order, sizeof(event_order));
    int rc = store(JOURNAL_KEY, record);
    if (!rc) current_journal = record.journal;
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
    unsigned count = desired == OTA_SLOT_USB ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
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
int image_info(const flash_area *area, uint32_t bound, uint32_t *content_end, uint8_t hash[32])
{
    uint8_t header[32], info[4];
    if (bound < sizeof(header) || bound > area->fa_size || flash_area_read(area, 0, header, sizeof(header)))
        return OTA_ERR_IMAGE;
    if (ota_read_le32(header) != 0x96f3b83dU) return OTA_ERR_FORMAT;
    uint32_t hsize = read16(header + 8), protected_size = read16(header + 10), size = ota_read_le32(header + 12);
    /* This prototype does not implement encrypted/compressed/RAM-load images. */
    if (hsize < 32 || hsize > bound || size > bound - hsize || ota_read_le32(header + 16)) return OTA_ERR_FORMAT;
    uint32_t pos = hsize + size;
    if (protected_size) {
        if (protected_size < 4 || protected_size > bound - pos || flash_area_read(area, pos, info, 4) ||
            read16(info) != 0x6908 || read16(info + 2) != protected_size) return OTA_ERR_FORMAT;
        uint32_t end = pos + protected_size; pos += 4;
        while (pos < end) {
            if (end - pos < 4 || flash_area_read(area, pos, info, 4)) return OTA_ERR_FORMAT;
            uint32_t len = read16(info + 2); pos += 4;
            if (len > end - pos) return OTA_ERR_FORMAT;
            pos += len;
        }
    }
    if (bound - pos < 4 || flash_area_read(area, pos, info, 4) || read16(info) != 0x6907) return OTA_ERR_FORMAT;
    uint32_t total = read16(info + 2);
    if (total < 4 || total > bound - pos) return OTA_ERR_FORMAT;
    uint32_t end = pos + total; pos += 4; bool found = false;
    while (pos < end) {
        if (end - pos < 4 || flash_area_read(area, pos, info, 4)) return OTA_ERR_FORMAT;
        uint32_t len = read16(info + 2); pos += 4;
        if (len > end - pos || info[1]) return OTA_ERR_FORMAT;
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
int validate(const ota_manifest *m)
{
    if (writer_open || !flushed || accepted != m->image_size) return OTA_ERR_INCOMPLETE;
    struct flash_img_check check = {m->artifact_sha256, m->image_size};
    if (flash_img_check(&writer, &check, OTA_SECONDARY)) return OTA_ERR_IMAGE;
    const flash_area *area;
    if (flash_area_open(OTA_SECONDARY, &area)) return OTA_ERR_STORAGE;
    uint8_t hash[32]; uint32_t content = 0;
    int rc = image_info(area, m->image_content_size, &content, hash);
    if (!rc && (content != m->image_content_size || content > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE ||
                memcmp(hash, m->mcuboot_image_hash, 32))) rc = OTA_ERR_IMAGE;
    static const uint8_t magic[16] = {0x77,0xc2,0x95,0xf3,0x60,0xd2,0xef,0x7f,0x35,0x52,0x50,0x0f,0x2c,0xb6,0x79,0x80};
    /* Existing imgtool --pad without --confirm: padding is FF followed by boot
     * magic. Do not edit trailer or infer non-pending state from this check. */
    if (!rc && (m->image_size != area->fa_size || m->image_size < content + sizeof(magic))) rc = OTA_ERR_FORMAT;
    uint8_t block[128];
    for (uint32_t pos = content; !rc && pos < m->image_size - sizeof(magic);) {
        size_t n = m->image_size - sizeof(magic) - pos; if (n > sizeof(block)) n = sizeof(block);
        if (flash_area_read(area, pos, block, n)) { rc = OTA_ERR_STORAGE; break; }
        for (size_t i = 0; i < n; ++i) if (block[i] != 0xff) { rc = OTA_ERR_FORMAT; break; }
        pos += n;
    }
    if (!rc && (flash_area_read(area, m->image_size - sizeof(magic), block, sizeof(magic)) ||
                memcmp(block, magic, sizeof(magic)))) rc = OTA_ERR_FORMAT;
    flash_area_close(area);
    return rc;
}
int begin(const ota_manifest *m, ota_slot_owner desired)
{
    if (!initialized || recovery || owner != OTA_SLOT_NONE || !m || !m->campaign_id) return OTA_ERR_STATE;
    uint8_t lead_nonzero = 0; for (size_t i = 0; i < 8; ++i) lead_nonzero |= expected_lead_eui[i];
    if (!lead_nonzero) return OTA_ERR_IDENTITY;
    if (!m->image_content_size || m->image_content_size > CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE ||
        m->image_content_size > m->image_size ||
        m->image_size != FIXED_PARTITION_SIZE(slot1_partition)) return OTA_ERR_CAPACITY;
    if (m->protocol_version != OTA_PROTOCOL_VERSION || m->hardware_id != CONFIG_OWNTECH_OTA_HARDWARE_ID ||
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
    int rc = flash_area_erase(area, 0, area->fa_size); flash_area_close(area);
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
    if (adopt) {
        if (owner != OTA_SLOT_USB || !staged || writer_open || !equal_manifest(current_manifest, *m))
            return OTA_ERR_CONFLICT;
        owner = OTA_SLOT_LEAD; return OTA_OK;
    }
    return begin(m, OTA_SLOT_PARTICIPANT);
}
int hook_append(void *, uint32_t offset, const uint8_t *data, size_t n)
{ lock guard; return owner == OTA_SLOT_PARTICIPANT ? append(offset, data, n) : OTA_ERR_STATE; }
int hook_flush(void *) { lock guard; return flush(); }
int hook_validate(void *, const ota_manifest *m)
{ lock guard; return equal_manifest(current_manifest, *m) ? validate(m) : OTA_ERR_CONFLICT; }
int hook_journal(void *, const ota_manifest *m, ota_state state, uint32_t commit)
{ lock guard; return save_journal(m, state, commit); }
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
    int jr = load(JOURNAL_KEY, record);
    if (!jr) {
        current_journal = record.journal; memcpy(expected_lead_eui, record.journal.lead_eui, 8);
        event_mask = record.journal.event_mask; memcpy(event_ms, record.journal.event_ms, sizeof(event_ms));
        memcpy(event_order, record.journal.event_order, sizeof(event_order));
    }
    if ((rc && rc != -ENOENT) || (jr && jr != -ENOENT)) {
        maintenance = recovery = true; ota_safety_restore(true); return OTA_ERR_JOURNAL;
    }
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
              hook_journal, hook_reboot, hook_close};
}
void ota_storage_boot_identity(ota_identity *id)
{
    lock guard;
    id->protocol_version = OTA_PROTOCOL_VERSION; id->usable_slot_size = FIXED_PARTITION_SIZE(slot1_partition);
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
    if (current_manifest.campaign_id) (void)save_journal(&current_manifest, OTA_ABORTED, current_journal.commit_id);
}
int ota_storage_persist_campaign(const ota_manifest *m, const ota_target *targets,
                                size_t count, uint32_t commit, ota_state state)
{
    if (!m || !targets || !count || count > OTA_MAX_TARGETS || !commit) return OTA_ERR_ARGUMENT;
    lock guard;
    fleet_record record = {}; record.magic = FLEET_MAGIC; record.length = sizeof(record);
    encode_manifest(record.manifest, *m);
    unsigned leads = 0;
    for (size_t i = 0; i < count; ++i) {
        uint8_t nonzero = 0;
        for (size_t k = 0; k < 8; ++k) nonzero |= targets[i].identity.eui[k];
        if (!nonzero) return OTA_ERR_IDENTITY;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(targets[i].identity.eui, record.eui[j], 8)) return OTA_ERR_IDENTITY;
        memcpy(record.eui[i], targets[i].identity.eui, 8);
        if (targets[i].is_lead) { ++leads; record.lead_index = (uint8_t)i; }
    }
    if (leads != 1) return OTA_ERR_IDENTITY;
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
    if (rc == static_cast<int>(sizeof(legacy_fleet_record)) && header[0] == JOURNAL_MAGIC) {
        legacy_fleet_record record = {}; rc = load(FLEET_KEY, record);
        if (rc) return rc;
        if (record.length != sizeof(record) || !record.count || record.count > OTA_MAX_TARGETS ||
            record.count > *count || !record.commit_id) return OTA_ERR_JOURNAL;
        *m = record.manifest; *count = record.count; *commit = record.commit_id;
        memcpy(targets, record.targets, record.count * sizeof(*targets)); return OTA_OK;
    }
    if (rc != static_cast<int>(sizeof(fleet_record)) || header[0] != FLEET_MAGIC) return OTA_ERR_JOURNAL;
    fleet_record record = {}; rc = load(FLEET_KEY, record, FLEET_MAGIC);
    if (rc) return rc;
    if (record.length != sizeof(record) || !record.count || record.count > OTA_MAX_TARGETS ||
        record.count > *count || !record.commit_id || record.lead_index >= record.count ||
        record.state > OTA_SUCCEEDED) return OTA_ERR_JOURNAL;
    for (size_t i = 0; i < record.count; ++i) {
        uint8_t nonzero = 0;
        for (size_t k = 0; k < 8; ++k) nonzero |= record.eui[i][k];
        if (!nonzero) return OTA_ERR_JOURNAL;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(record.eui[i], record.eui[j], 8)) return OTA_ERR_JOURNAL;
    }
    decode_manifest(*m, record.manifest); *count = record.count; *commit = record.commit_id;
    memset(targets, 0, record.count * sizeof(*targets));
    for (size_t i = 0; i < record.count; ++i) {
        memcpy(targets[i].identity.eui, record.eui[i], 8);
        targets[i].is_lead = i == record.lead_index;
    }
    return OTA_OK;
}
int ota_storage_release_maintenance(const uint8_t expected_hash[32])
{
    lock guard;
    uint8_t active[32];
    if (!expected_hash || writer_open || reboot_queued || !boot_available() ||
        ota_storage_active_hash(active) || memcmp(active, expected_hash, 32)) return OTA_ERR_STATE;
    /* Retain complete campaign provenance and result across a subsequent boot.
     * The independent inhibition marker is cleared only after this succeeds. */
    local_record record = {}; record.magic = JOURNAL_MAGIC; record.journal = current_journal;
    record.journal.state = OTA_SUCCEEDED; record.journal.event_mask = event_mask;
    memcpy(record.journal.event_ms, event_ms, sizeof(event_ms));
    memcpy(record.journal.event_order, event_order, sizeof(event_order));
    if (store(JOURNAL_KEY, record)) return OTA_ERR_JOURNAL;
    if (save_marker(MAINTENANCE_KEY, false)) return OTA_ERR_JOURNAL;
    current_journal = record.journal;
    maintenance = recovery = false; owner = OTA_SLOT_NONE; ota_safety_restore(false);
    return OTA_OK;
}
