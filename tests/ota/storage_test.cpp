/* Actual ota_storage.cpp with fake Zephyr/NVS boundaries, including reset RAM. */
#define CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE 900
#define CONFIG_OWNTECH_OTA_HARDWARE_ID 1
#define CONFIG_OWNTECH_OTA_LAYOUT_ID 2
#define CONFIG_OWNTECH_OTA_BOOTLOADER_ID 3
#include "ota_storage.cpp"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

static uint8_t flash[2][1024], expected_artifact[1024];
static flash_area areas[] = {{0,1024},{1,1024}};
/* Two 2048-byte sectors, one reserved for GC. Entries append data+ATE; GC
 * copies every latest live value BEFORE appending a replacement, exactly the
 * capacity constraint of Zephyr NVS. The old key remains live on ENOSPC. */
struct nv { uint16_t key; size_t len; unsigned sequence; uint8_t data[1536]; };
static nv records[2][64];
static unsigned nv_sector, nv_sequence, nv_used[2], nv_gc_count;
static bool nv_gc_marker;
static unsigned erased, flushed_count, scheduled, checked, nvs_writes;
static bool confirmed = true, inhibited = true, safety_fail, write_fail, nvs_fail, hash_fail;
static int swap = BOOT_SWAP_TYPE_NONE;
int flash_area_open(unsigned id, const flash_area **p) { if (id > 1) return -1; *p = &areas[id]; return 0; }
void flash_area_close(const flash_area *) {}
int flash_area_read(const flash_area *a, uint32_t off, void *out, size_t n)
{ if (off > 1024 || n > 1024 - off) return -1; memcpy(out, flash[a->id] + off, n); return 0; }
int flash_area_erase(const flash_area *a, uint32_t off, size_t n)
{
    marker m = {}; if (!inhibited || load(MAINTENANCE_KEY, m) || m.value != 1) return -1;
    if (a->id != 1 || off || n != 1024) return -1;
    memset(flash[1], 0xff, 1024); ++erased; return 0;
}
int flash_img_init_id(flash_img_context *w, uint8_t id)
{ *w = {}; return flash_area_open(id, &w->flash_area); }
int flash_img_buffered_write(flash_img_context *w, const uint8_t *data, size_t n, bool final)
{
    if (write_fail || !w->flash_area) return -1;
    while (n) {
        size_t take = 512 - w->pending; if (take > n) take = n;
        memcpy(w->buf + w->pending, data, take); w->pending += take; data += take; n -= take;
        if (w->pending == 512) { memcpy(flash[1] + w->written, w->buf, 512); w->written += 512; w->pending = 0; }
    }
    if (final) {
        memcpy(flash[1] + w->written, w->buf, w->pending); w->written += w->pending; w->pending = 0;
        w->flash_area = nullptr; ++flushed_count;
    }
    return 0;
}
size_t flash_img_bytes_written(flash_img_context *w) { return w->written; }
int flash_img_check(flash_img_context *, const struct flash_img_check *check, uint8_t id)
{
    ++checked;
    /* Model the SHA contract by checking all exact artifact bytes. Hash
     * implementation is Zephyr's existing SHA256, not a new OTA primitive. */
    return hash_fail || id != 1 || check->clen != 1024 || memcmp(flash[1], expected_artifact, 1024) ? -1 : 0;
}
bool boot_is_img_confirmed() { return confirmed; }
int mcuboot_swap_type() { return swap; }
int k_work_schedule(k_work_delayable *, uint32_t) { ++scheduled; return 0; }
void sys_reboot(int) {}
extern "C" int ota_safety_enter() { inhibited = true; return safety_fail ? -1 : 0; }
extern "C" bool ota_safety_inhibited() { return inhibited; }
extern "C" void ota_safety_restore(bool v) { inhibited = v; }
int nvs_storage_write(uint16_t key, const void *data, size_t n)
{
    if (nvs_fail || n > sizeof(records[0][0].data)) return -EIO;
    nv *latest = nullptr;
    for (unsigned i = 0; i < 64; ++i) if (records[nv_sector][i].key == key &&
            (!latest || records[nv_sector][i].sequence > latest->sequence)) latest = &records[nv_sector][i];
    if (latest && latest->len == n && !memcmp(latest->data, data, n)) return 0;
    unsigned required = ((n + 7) & ~7U) + 8;
    if (nv_used[nv_sector] + required > 2032) {
        unsigned next = nv_sector ^ 1, count = 0;
        memset(records[next], 0, sizeof(records[next])); nv_used[next] = 8; /* GC-done ATE */
        for (unsigned i = 0; i < 64; ++i) {
            const nv &candidate = records[nv_sector][i];
            if (!candidate.key) continue;
            bool obsolete = false;
            for (unsigned j = 0; j < 64; ++j)
                if (records[nv_sector][j].key == candidate.key &&
                    records[nv_sector][j].sequence > candidate.sequence) obsolete = true;
            if (obsolete) continue;
            records[next][count++] = candidate;
            nv_used[next] += ((candidate.len + 7) & ~7U) + 8;
        }
        memset(records[nv_sector], 0, sizeof(records[nv_sector])); nv_used[nv_sector] = 0;
        nv_sector = next; nv_gc_marker = true; ++nv_gc_count;
    }
    if (nv_used[nv_sector] + required > 2032) return -ENOSPC;
    for (unsigned i = 0; i < 64; ++i) if (!records[nv_sector][i].key) {
        nv &entry = records[nv_sector][i]; entry.key = key; entry.len = n; entry.sequence = ++nv_sequence;
        memcpy(entry.data, data, n); nv_used[nv_sector] += required; ++nvs_writes; return (int)n;
    }
    return -ENOSPC;
}
int nvs_storage_read(uint16_t key, void *data, size_t n)
{
    const nv *latest = nullptr;
    for (unsigned i = 0; i < 64; ++i) if (records[nv_sector][i].key == key &&
            (!latest || records[nv_sector][i].sequence > latest->sequence)) latest = &records[nv_sector][i];
    if (latest) {
        if (n > latest->len) n = latest->len;
        memcpy(data, latest->data, n);
        return (int)latest->len;
    }
    return -ENOENT;
}
int32_t nvs_storage_get_free_space()
{
    int32_t free = 2032 - (nv_gc_marker ? 8 : 0);
    for (unsigned i = 0; i < 64; ++i) {
        const nv &candidate = records[nv_sector][i];
        if (!candidate.key) continue;
        bool obsolete = false;
        for (unsigned j = 0; j < 64; ++j)
            if (records[nv_sector][j].key == candidate.key &&
                records[nv_sector][j].sequence > candidate.sequence) obsolete = true;
        if (!obsolete) free -= ((candidate.len + 7) & ~7U) + 8;
    }
    return free;
}
static void reset_nv()
{
    memset(records, 0, sizeof(records)); nv_sector = nv_sequence = nv_gc_count = 0;
    nv_used[0] = nv_used[1] = 0; nv_gc_marker = false;
}
static void reset_ram()
{
    writer = {}; current_manifest = {}; current_journal = {}; owner = OTA_SLOT_NONE;
    initialized = recovery = maintenance = writer_open = flushed = staged = reboot_queued = false;
    accepted = reboot_commit = 0; terminal_error = 0; inhibited = true;
    memset(expected_lead_eui,0,8);
    event_mask=0;memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
}
static int init_service()
{
    int rc=ota_storage_init();if(rc) return rc;
    const uint8_t lead_eui[8]={0,0,0,0,0,0,0,1};
    return ota_storage_expect_lead(lead_eui);
}
static ota_manifest artifact()
{
    ota_manifest m = {}; m.campaign_id = 42; m.image_size = 1024; m.image_content_size = 200;
    m.hardware_id = 1; m.layout_id = 2; m.bootloader_id = 3; m.protocol_version = 1;
    memcpy(m.version,"1.2.3",6);memcpy(m.build_id,"test-build",11);
    memset(m.mcuboot_image_hash, 0xbb, 32);
    memset(expected_artifact, 0xff, sizeof(expected_artifact));
    memset(expected_artifact, 0, 32); uint8_t *b = expected_artifact;
    b[0]=0x3d;b[1]=0xb8;b[2]=0xf3;b[3]=0x96;b[8]=32;b[12]=128;
    b[160]=0x07;b[161]=0x69;b[162]=40;b[163]=0;
    b[164]=0x10;b[165]=0;b[166]=32;b[167]=0;memcpy(b+168,m.mcuboot_image_hash,32);
    const uint8_t magic[16]={0x77,0xc2,0x95,0xf3,0x60,0xd2,0xef,0x7f,0x35,0x52,0x50,0x0f,0x2c,0xb6,0x79,0x80};
    memcpy(b+1008,magic,16); memcpy(flash[0], b, 1024);
    return m;
}
static int upload(const ota_manifest &m)
{
    int rc=ota_storage_stage_begin(&m); if(rc) return rc;
    for (uint32_t pos=0;pos<1024;pos+=256) { rc=ota_storage_stage_append(pos,expected_artifact+pos,256); if(rc) return rc; }
    return ota_storage_stage_end(&m);
}
static int journal_gc_regressions()
{
    reset_nv(); reset_ram(); ota_manifest m=artifact(); m.campaign_id=0xfedcba9876543210ULL;
    CHECK(!init_service());
    uint8_t calibration[1200], readback[1200];
    for(size_t i=0;i<sizeof(calibration);++i) calibration[i]=(uint8_t)(i*37+9);
    const uint16_t version=1;
    CHECK(nvs_storage_write(0x0100,&version,sizeof(version))==sizeof(version));
    CHECK(nvs_storage_write(0x0201,calibration,512)==512);
    CHECK(!ota_storage_persist_role(true));
    CHECK(!upload(m));
    ota_target targets[OTA_MAX_TARGETS]={}; targets[0].is_lead=true;
    for(size_t i=0;i<OTA_MAX_TARGETS;++i) {
        targets[i].identity.eui[0]=0xa5;targets[i].identity.eui[7]=(uint8_t)(i+1);
        targets[i].identity.address=(uint8_t)(i+5);
    }
    /* Reproduce the hardware failure: the original growth-only reservation
     * admits 824 B, VALID is durable, but COMMITTED needs another 832 B while
     * GC must preserve that VALID record. The prior value survives ENOSPC. */
    CHECK(nvs_storage_get_free_space()>=832);
    legacy_fleet_record legacy={};legacy.magic=JOURNAL_MAGIC;legacy.length=sizeof(legacy);
    legacy.manifest=m;memcpy(legacy.targets,targets,sizeof(targets));
    legacy.count=2;legacy.commit_id=77;legacy.state=OTA_VALID;
    CHECK(!store(FLEET_KEY,legacy));
    legacy.state=OTA_COMMITTED;
    legacy.crc=ota_crc32(reinterpret_cast<const uint8_t *>(&legacy),offsetof(legacy_fleet_record,crc));
    CHECK(nvs_storage_write(FLEET_KEY,&legacy,sizeof(legacy))==-ENOSPC);
    CHECK(store(FLEET_KEY,legacy)==OTA_ERR_JOURNAL);
    legacy_fleet_record previous={};CHECK(!load(FLEET_KEY,previous)&&previous.state==OTA_VALID);
    ota_manifest restored={};ota_target restored_targets[OTA_MAX_TARGETS]={};
    size_t count=OTA_MAX_TARGETS;uint32_t commit=0;
    CHECK(!ota_storage_load_campaign(&restored,restored_targets,&count,&commit));
    CHECK(count==2&&commit==77&&equal_manifest(restored,m));
    CHECK(restored_targets[0].is_lead&&restored_targets[1].identity.address==6);
    /* Migration may only use currently free bytes; it never counts the 520 B
     * shrink before the compact replacement itself has been committed. */
    CHECK(!reserve_journal_space(OTA_SLOT_USB));
    CHECK(!ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,77,OTA_COMMITTED));
    uint8_t first=0;CHECK(nvs_storage_read(FLEET_KEY,&first,1)==304);
    unsigned gc_before=nv_gc_count;
    for(unsigned i=0;i<80;++i) {
        CHECK(!ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,78+i,
              i%2?OTA_REBOOTING:OTA_COMMITTED));
        CHECK(!save_journal(&m,i%2?OTA_REBOOTING:OTA_COMMITTED,78+i));
        count=OTA_MAX_TARGETS;CHECK(!ota_storage_load_campaign(&restored,restored_targets,&count,&commit));
        CHECK(count==OTA_MAX_TARGETS&&commit==78+i&&equal_manifest(restored,m));
        CHECK(restored_targets[0].is_lead&&!restored_targets[1].is_lead);
        for(size_t j=0;j<count;++j)
            CHECK(!memcmp(restored_targets[j].identity.eui,targets[j].identity.eui,8)&&
                  !restored_targets[j].identity.address);
    }
    CHECK(nv_gc_count>gc_before+10);
    CHECK(nvs_storage_read(0x0201,readback,512)==512&&!memcmp(readback,calibration,512));
    CHECK(nvs_storage_read(0x0100,&commit,sizeof(commit))==2&&(commit&0xffff)==version);
    /* A valid CRC cannot hide a malformed compact roster; damaged bytes also
     * fail loading. These are read-only failures, without migration writes. */
    fleet_record compact={};CHECK(!load(FLEET_KEY,compact,FLEET_MAGIC));
    compact.eui[1][0]^=1;CHECK(nvs_storage_write(FLEET_KEY,&compact,sizeof(compact))==sizeof(compact));
    count=OTA_MAX_TARGETS;CHECK(ota_storage_load_campaign(&restored,restored_targets,&count,&commit)==OTA_ERR_JOURNAL);
    memcpy(compact.eui[1],compact.eui[0],8);CHECK(!store(FLEET_KEY,compact));
    CHECK(ota_storage_load_campaign(&restored,restored_targets,&count,&commit)==OTA_ERR_JOURNAL);
    /* No version key yet: the 16 B implicit first-write allocation must also
     * fit. Free space equals growth+overwrite+GC, so omitting VERSION would
     * admit this campaign incorrectly. Rejection must precede any mutation. */
    reset_nv();reset_ram();CHECK(!init_service());
    CHECK(nvs_storage_write(0x0201,calibration,1120)==1120);
    unsigned erase_before=erased, writes_before=nvs_writes;
    CHECK(nvs_storage_get_free_space()==24+248+312+312+8);
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL);
    CHECK(erased==erase_before&&nvs_writes==writes_before);
    /* Enough final live-data space, but no room to replace the largest
     * journal. Reject before marker, erase or calibration changes. */
    reset_nv();reset_ram();CHECK(!init_service());
    CHECK(nvs_storage_write(0x0201,calibration,1200)==1200);
    erase_before=erased;writes_before=nvs_writes;
    CHECK(nvs_storage_get_free_space()>24+248+312);
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL);
    CHECK(erased==erase_before&&nvs_writes==writes_before);
    CHECK(nvs_storage_read(MAINTENANCE_KEY,&first,1)==-ENOENT);
    CHECK(nvs_storage_read(0x0201,readback,sizeof(readback))==sizeof(readback)&&
          !memcmp(readback,calibration,sizeof(readback)));
    /* Migration must likewise fail closed when old OTA1 data cannot coexist
     * with OTA2; successful replacement must not be assumed by preflight. */
    reset_nv();reset_ram();CHECK(!init_service());
    CHECK(nvs_storage_write(0x0201,calibration,800)==800);
    CHECK(!save_marker(MAINTENANCE_KEY,false));CHECK(!save_journal(&m,OTA_SUCCEEDED,77));
    legacy.state=OTA_SUCCEEDED;CHECK(!store(FLEET_KEY,legacy));
    erase_before=erased;writes_before=nvs_writes;
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL&&erased==erase_before&&nvs_writes==writes_before);
    CHECK(nvs_storage_read(FLEET_KEY,&first,1)==sizeof(legacy));
    CHECK(nvs_storage_read(0x0201,readback,800)==800&&!memcmp(readback,calibration,800));
    return 0;
}
extern "C" int ota_storage_test_run()
{
    reset_nv(); reset_ram(); ota_manifest m=artifact();
    const uint8_t calibration[4]={1,2,3,4}; CHECK(nvs_storage_write(0x0201,calibration,4)==4);
    CHECK(!init_service() && inhibited); /* health has not released RAM gate */
    bool lead=true; CHECK(!ota_storage_load_role(&lead) && !lead); CHECK(!ota_storage_persist_role(true));
    confirmed=false; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased); confirmed=true;
    swap=BOOT_SWAP_TYPE_TEST; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased);
    swap=BOOT_SWAP_TYPE_REVERT; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased); swap=BOOT_SWAP_TYPE_NONE;
    CHECK(!ota_storage_stage_begin(&m) && erased==1 && inhibited);
    CHECK(!ota_storage_stage_begin(&m) && erased==1);
    CHECK(ota_storage_persist_role(false)==OTA_ERR_STATE);
    CHECK(!ota_storage_stage_append(0,expected_artifact,256));
    CHECK(!ota_storage_stage_append(0,expected_artifact,256)); /* buffered retry */
    uint8_t bad[256];memcpy(bad,expected_artifact,256);bad[0]^=1;
    CHECK(ota_storage_stage_append(0,bad,256)==OTA_ERR_CONFLICT);
    CHECK(!ota_storage_stage_append(256,expected_artifact+256,256));
    CHECK(!ota_storage_stage_append(0,expected_artifact,256)); /* committed retry */
    CHECK(ota_storage_stage_append(0,bad,256)==OTA_ERR_CONFLICT);
    CHECK(ota_storage_stage_end(&m)==OTA_ERR_INCOMPLETE && !flushed_count);
    CHECK(!ota_storage_stage_append(512,expected_artifact+512,256));
    CHECK(!ota_storage_stage_append(768,expected_artifact+768,256));
    CHECK(!ota_storage_stage_end(&m) && flushed_count==1 && checked==1);
    CHECK(!ota_storage_stage_end(&m) && flushed_count==1 && checked==1);
    uint8_t readback[1024];CHECK(!ota_storage_read(0,readback,1024) && !memcmp(readback,expected_artifact,1024));
    /* Active image already equals target, but stage still erased and wrote all bytes. */
    CHECK(!memcmp(flash[0],flash[1],1024) && erased==1);
    swap=BOOT_SWAP_TYPE_TEST; ota_participant_hooks hooks;ota_storage_hooks(&hooks);
    CHECK(!hooks.prepare(nullptr,&m,true) && ota_storage_owner()==OTA_SLOT_LEAD && erased==1);
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE);
    CHECK(!hooks.validate(nullptr,&m));
    uint32_t timestamps[12]={100,200};uint8_t order[12]={1,2};
    ota_storage_set_events(3,timestamps,order);
    CHECK(!hooks.journal(nullptr,&m,OTA_REBOOTING,77));
    CHECK(!hooks.schedule_reboot(nullptr,77,1000));CHECK(!hooks.schedule_reboot(nullptr,77,2000)&&scheduled==1);
    CHECK(hooks.schedule_reboot(nullptr,78,1000)==OTA_ERR_CONFLICT);
    ota_target targets[OTA_MAX_TARGETS]={}; targets[0].is_lead=true;
    for (size_t i=0;i<OTA_MAX_TARGETS;++i) targets[i].identity.eui[7]=(uint8_t)(i+1);
    CHECK(!ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,77,OTA_REBOOTING));
    CHECK(sizeof(fleet_record)==304); ota_manifest loaded;size_t count=OTA_MAX_TARGETS;uint32_t commit;
    CHECK(!ota_storage_load_campaign(&loaded,targets,&count,&commit)&&count==OTA_MAX_TARGETS&&commit==77);
    CHECK(equal_manifest(loaded,m) && targets[0].is_lead && !targets[1].is_lead);
    for(size_t i=0;i<count;++i) CHECK(targets[i].identity.eui[7]==i+1 && !targets[i].identity.address);
    uint8_t existing[4];CHECK(nvs_storage_read(0x0201,existing,4)==4&&!memcmp(existing,calibration,4));
    reset_ram();CHECK(!init_service()&&ota_storage_recovery_required()&&inhibited);
    ota_storage_journal restored;CHECK(!ota_storage_get_journal(&restored));
    CHECK(restored.campaign_id==m.campaign_id&&restored.image_size==m.image_size&&restored.event_mask==3);
    CHECK(restored.event_ms[1]==200&&restored.event_order[1]==2&&!memcmp(restored.version,m.version,32));
    CHECK(!memcmp(restored.build_id,m.build_id,32));
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE);
    CHECK(ota_storage_release_maintenance(m.mcuboot_image_hash)==OTA_ERR_STATE);swap=BOOT_SWAP_TYPE_NONE;
    uint8_t hash[32];CHECK(!ota_storage_active_hash(hash)&&!memcmp(hash,m.mcuboot_image_hash,32));
    CHECK(!ota_storage_release_maintenance(hash)&&!ota_storage_maintenance()&&!inhibited);
    CHECK(!ota_storage_get_journal(&restored)&&restored.state==OTA_SUCCEEDED&&restored.campaign_id==m.campaign_id);
    CHECK(!ota_storage_load_role(&lead)&&lead);
    ++m.campaign_id;CHECK(!upload(m)&&erased==2);ota_storage_abort();CHECK(ota_storage_recovery_required());

    /* Power-safe preconditions precede erase, and journal failure fails closed. */
    reset_nv();reset_ram();CHECK(!init_service());
    unsigned erase_before=erased;nvs_fail=true;CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL);
    CHECK(erased==erase_before&&inhibited&&ota_storage_recovery_required());nvs_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());
    CHECK(!ota_storage_stage_begin(&m));write_fail=true;
    CHECK(ota_storage_stage_append(0,expected_artifact,256)==OTA_ERR_STORAGE&&ota_storage_recovery_required());write_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());hash_fail=true;
    CHECK(upload(m)==OTA_ERR_IMAGE&&ota_storage_recovery_required());hash_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());expected_artifact[1008]^=1;
    CHECK(upload(m)==OTA_ERR_FORMAT&&ota_storage_recovery_required());expected_artifact[1008]^=1;
    reset_nv();reset_ram();CHECK(!init_service());m.mcuboot_image_hash[0]^=1;
    CHECK(upload(m)==OTA_ERR_IMAGE&&ota_storage_recovery_required());m.mcuboot_image_hash[0]^=1;
    reset_nv();reset_ram();CHECK(!init_service());
    uint8_t metadata[1400]={};
    CHECK(nvs_storage_write(0x0301,metadata,sizeof(metadata))==sizeof(metadata));erase_before=erased;
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL&&erased==erase_before);
    return journal_gc_regressions();
}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main(){int rc=ota_storage_test_run();if(rc) fprintf(stderr,"storage_test.cpp:%d\n",rc);return rc?1:0;}
#endif
