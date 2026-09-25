/* Actual ota_storage.cpp with fake Zephyr/NVS boundaries, including reset RAM. */
#define CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE 900
#ifndef CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED
#define CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED 1
#endif
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
static unsigned erased, flushed_count, scheduled, checked, nvs_writes, armed;
static unsigned programmed_end;
static bool arm_fail;
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
    memset(flash[1], 0xff, 1024); programmed_end=0; ++erased; return 0;
}
int flash_img_init_id(flash_img_context *w, uint8_t id)
{ *w = {}; return flash_area_open(id, &w->flash_area); }
int flash_img_buffered_write(flash_img_context *w, const uint8_t *data, size_t n, bool final)
{
    if (write_fail || !w->flash_area) return -1;
    while (n) {
        size_t take = 512 - w->pending; if (take > n) take = n;
        memcpy(w->buf + w->pending, data, take); w->pending += take; data += take; n -= take;
        if (w->pending == 512) { memcpy(flash[1] + w->written, w->buf, 512); w->written += 512; w->pending = 0; programmed_end=w->written; }
    }
    if (final) {
        /* stream_flash programs an aligned padded buffer, including FF ECC. */
        if (w->pending) { memset(flash[1]+w->written,0xff,512); programmed_end=w->written+512; }
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
    return hash_fail || id != 1 || check->clen != 200 || memcmp(flash[1], expected_artifact, 200) ? -1 : 0;
}
bool boot_is_img_confirmed() { return confirmed; }
int mcuboot_swap_type() { return swap; }
int boot_request_upgrade(int mode)
{
    local_record record = {}; ota_storage_journal j = {};
    if (mode != BOOT_UPGRADE_TEST || load(JOURNAL_KEY,record,LOCAL_MAGIC) || decode_local(record,j) ||
        j.state != OTA_COMMIT_INTENT || !j.commit_id || !inhibited || writer_open) return -1;
    ++armed; if (arm_fail) return -1;
    swap = BOOT_SWAP_TYPE_TEST; flash[1][1008] = 0x77; return 0;
}
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
    initialized = recovery = maintenance = writer_open = flushed = staged = reboot_queued = journal_corrupt = false;
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
    ota_manifest m = {}; m.campaign_id = 42; m.image_size = 200; m.image_content_size = 200;
    m.hardware_id = 1; m.layout_id = 2; m.bootloader_id = 3; m.protocol_version = OTA_PROTOCOL_VERSION;
    m.image_class = OTA_IMAGE_RECEIVER;
    memcpy(m.version,"1.2.3",6);memcpy(m.build_id,"test-build",11);
    memset(m.mcuboot_image_hash, 0xbb, 32);
    memset(expected_artifact, 0xff, sizeof(expected_artifact));
    memset(expected_artifact, 0, 32); uint8_t *b = expected_artifact;
    b[0]=0x3d;b[1]=0xb8;b[2]=0xf3;b[3]=0x96;b[8]=32;b[10]=16;b[12]=112;
    b[144]=0x08;b[145]=0x69;b[146]=16;b[147]=0;
    b[148]=0xa0;b[149]=0;b[150]=8;b[151]=0;memcpy(b+152,"receiver",8);
    b[160]=0x07;b[161]=0x69;b[162]=40;b[163]=0;
    b[164]=0x10;b[165]=0;b[166]=32;b[167]=0;memcpy(b+168,m.mcuboot_image_hash,32);
    memcpy(flash[0], b, 1024);
    return m;
}
static int upload(const ota_manifest &m)
{
    int rc=ota_storage_stage_begin(&m); if(rc) return rc;
    rc=ota_storage_stage_append(0,expected_artifact,200); if(rc) return rc;
    return ota_storage_stage_end(&m);
}
static int journal_gc_regressions()
{
    reset_nv(); reset_ram(); ota_manifest m=artifact(); CHECK(!init_service());
    uint8_t calibration[1500], readback[1500];
    for(size_t i=0;i<sizeof(calibration);++i) calibration[i]=(uint8_t)(i*37+9);
    CHECK(nvs_storage_write(0x0201,calibration,512)==512);
    CHECK(!upload(m));
    ota_target targets[OTA_MAX_TARGETS]={};
    for(size_t i=0;i<OTA_MAX_TARGETS;++i) targets[i].identity.eui[7]=(uint8_t)(i+1);
    ota_manifest restored={};ota_target restored_targets[OTA_MAX_TARGETS]={};
    size_t count=OTA_MAX_TARGETS;uint32_t commit=0;
    unsigned gc_before=nv_gc_count;
    for(unsigned i=0;i<80;++i) {
        CHECK(!ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,78+i,i%2?OTA_REBOOTING:OTA_COMMITTED));
        CHECK(!save_journal(&m,i%2?OTA_REBOOTING:OTA_COMMITTED,78+i));
        count=OTA_MAX_TARGETS;CHECK(!ota_storage_load_campaign(&restored,restored_targets,&count,&commit));
        CHECK(count==OTA_MAX_TARGETS&&commit==78+i&&equal_manifest(restored,m));
        for(size_t j=0;j<count;++j)
            CHECK(!restored_targets[j].is_lead && !memcmp(restored_targets[j].identity.eui,targets[j].identity.eui,8)&&!restored_targets[j].identity.address);
    }
    CHECK(nv_gc_count>gc_before+10);
    CHECK(nvs_storage_read(0x0201,readback,512)==512&&!memcmp(readback,calibration,512));
    fleet_record compact={};CHECK(!load(FLEET_KEY,compact,FLEET_MAGIC));
    compact.eui[1][0]^=1;CHECK(nvs_storage_write(FLEET_KEY,&compact,sizeof(compact))==sizeof(compact));
    count=OTA_MAX_TARGETS;CHECK(ota_storage_load_campaign(&restored,restored_targets,&count,&commit)==OTA_ERR_JOURNAL);
    memcpy(compact.eui[1],compact.eui[0],8);CHECK(!store(FLEET_KEY,compact));
    CHECK(ota_storage_load_campaign(&restored,restored_targets,&count,&commit)==OTA_ERR_JOURNAL);
    targets[0].is_lead=true;
    CHECK(ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,77,OTA_COMMITTED)==OTA_ERR_IDENTITY);
    /* A receiver cannot inherit even a valid dedicated Lead's fleet record. */
    reset_nv();reset_ram();CHECK(!init_service());targets[0].is_lead=false;
    CHECK(!ota_storage_persist_campaign(&m,targets,OTA_MAX_TARGETS,77,OTA_PREPARING));
    reset_ram();unsigned before_class_check=nvs_writes;
    CHECK(ota_storage_init()==OTA_ERR_JOURNAL&&ota_storage_recovery_required());
    CHECK(nvs_writes==before_class_check&&!load(FLEET_KEY,compact,FLEET_MAGIC));
    reset_nv();reset_ram();CHECK(!init_service());
    CHECK(nvs_storage_write(0x0201,calibration,1500)==1500);
    unsigned erase_before=erased,writes_before=nvs_writes;
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL);
    CHECK(erased==erase_before&&nvs_writes==writes_before);
    CHECK(nvs_storage_read(0x0201,readback,1500)==1500&&!memcmp(readback,calibration,1500));
    /* Legacy bytes remain untouched for explicit recovery, never a v2 campaign. */
    reset_nv();reset_ram();uint8_t old[240]={};write32(old,JOURNAL_MAGIC);
    CHECK(nvs_storage_write(JOURNAL_KEY,old,sizeof(old))==sizeof(old));
    CHECK(ota_storage_init()==OTA_ERR_JOURNAL&&ota_storage_recovery_required());
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE&&erased==erase_before);
    CHECK(ota_storage_release_maintenance(m.mcuboot_image_hash)==OTA_ERR_STATE);
    CHECK(nvs_storage_read(JOURNAL_KEY,readback,sizeof(readback))==sizeof(old)&&!memcmp(old,readback,sizeof(old)));
    return 0;
}
extern "C" int ota_storage_test_run()
{
    reset_nv(); reset_ram(); ota_manifest m=artifact();
    const uint8_t calibration[4]={1,2,3,4}; CHECK(nvs_storage_write(0x0201,calibration,4)==4);
    CHECK(!init_service() && inhibited);
    bool lead=true; CHECK(!ota_storage_load_role(&lead) && !lead);
    ota_identity identity={};ota_storage_boot_identity(&identity);
    CHECK(identity.protocol_version==2&&identity.image_class==OTA_IMAGE_RECEIVER);
    confirmed=false; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased); confirmed=true;
    swap=BOOT_SWAP_TYPE_TEST; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased);
    swap=BOOT_SWAP_TYPE_REVERT; CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE && !erased); swap=BOOT_SWAP_TYPE_NONE;
    ota_manifest bad=m;bad.image_size=1024;
    CHECK(ota_storage_stage_begin(&bad)==OTA_ERR_CAPACITY&&!erased);
    bad=m;bad.image_class=OTA_IMAGE_LEAD;
    CHECK(ota_storage_stage_begin(&bad)==OTA_ERR_COMPATIBILITY&&!erased);
    bad=m;bad.protocol_version=1;
    CHECK(ota_storage_stage_begin(&bad)==OTA_ERR_COMPATIBILITY&&!erased);
    flash[1][1008]=0x77;
    CHECK(!ota_storage_stage_begin(&m) && erased==1 && inhibited&&flash[1][1008]==0xff);
    CHECK(!ota_storage_stage_begin(&m) && erased==1);
    CHECK(!ota_storage_stage_append(0,expected_artifact,100));
    CHECK(!ota_storage_stage_append(0,expected_artifact,100));
    uint8_t wrong[100];memcpy(wrong,expected_artifact,100);wrong[0]^=1;
    CHECK(ota_storage_stage_append(0,wrong,100)==OTA_ERR_CONFLICT);
    CHECK(ota_storage_stage_end(&m)==OTA_ERR_INCOMPLETE && !flushed_count);
    CHECK(!ota_storage_stage_append(100,expected_artifact+100,100));
    CHECK(ota_storage_stage_append(200,expected_artifact,1)==OTA_ERR_OFFSET);
    CHECK(!ota_storage_stage_end(&m) && flushed_count==1 && checked==1);
    CHECK(!ota_storage_stage_end(&m) && flushed_count==1 && checked==1);
    CHECK(!armed && swap==BOOT_SWAP_TYPE_NONE && flash[1][1008]==0xff);
    CHECK(programmed_end==512&&programmed_end<=CONFIG_OWNTECH_OTA_USABLE_IMAGE_SIZE);
    for(size_t i=200;i<1024;++i) CHECK(flash[1][i]==0xff);
    ota_participant_hooks hooks;ota_storage_hooks(&hooks);
    CHECK(hooks.prepare(nullptr,&m,true)==OTA_ERR_COMPATIBILITY);
    CHECK(hooks.arm(nullptr,&m,77)==OTA_ERR_STATE&&!armed);
    reset_ram(); CHECK(!init_service()&&ota_storage_recovery_required()&&!writer_open&&!armed);
    ota_storage_journal restored;CHECK(!ota_storage_get_journal(&restored));
    CHECK(restored.format_version==2&&restored.state==OTA_VALID&&restored.image_size==200&&!restored.event_mask);
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_STATE);
    uint8_t hash[32];CHECK(!ota_storage_active_hash(hash));
    CHECK(!ota_storage_release_maintenance(hash));
    CHECK(!ota_storage_maintenance()&&!inhibited);
    ++m.campaign_id;CHECK(!hooks.prepare(nullptr,&m,false));
    CHECK(!hooks.append(nullptr,0,expected_artifact,200));CHECK(!hooks.flush(nullptr));
    CHECK(!hooks.validate(nullptr,&m));CHECK(!hooks.journal(nullptr,&m,OTA_VALID,0));
    CHECK(!armed&&flash[1][1008]==0xff);
    CHECK(!hooks.journal(nullptr,&m,OTA_COMMIT_INTENT,77));
    flash[1][1008]=0x33;unsigned erase_before_arm=erased;
    CHECK(hooks.arm(nullptr,&m,77)==OTA_ERR_STORAGE&&!armed&&erased==erase_before_arm);
    flash[1][1008]=0xff;
    CHECK(!hooks.arm(nullptr,&m,77)&&armed==1&&swap==BOOT_SWAP_TYPE_TEST);
    CHECK(!hooks.journal(nullptr,&m,OTA_COMMITTED,77));
    ota_storage_abort();CHECK(!ota_storage_get_journal(&restored)&&restored.state==OTA_COMMITTED);
    reset_ram();CHECK(!init_service()&&ota_storage_recovery_required()&&inhibited);
    CHECK(!ota_storage_get_journal(&restored)&&restored.commit_id==77&&restored.state==OTA_COMMITTED);
    CHECK(ota_storage_release_maintenance(hash)==OTA_ERR_STATE);swap=BOOT_SWAP_TYPE_NONE;
    CHECK(!ota_storage_release_maintenance(hash));
    uint8_t existing[4];CHECK(nvs_storage_read(0x0201,existing,4)==4&&!memcmp(existing,calibration,4));
    reset_nv();reset_ram();CHECK(!init_service());
    unsigned erase_before=erased;nvs_fail=true;CHECK(ota_storage_stage_begin(&m)==OTA_ERR_JOURNAL);
    CHECK(erased==erase_before&&inhibited&&ota_storage_recovery_required());nvs_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());CHECK(!ota_storage_stage_begin(&m));write_fail=true;
    CHECK(ota_storage_stage_append(0,expected_artifact,200)==OTA_ERR_STORAGE&&ota_storage_recovery_required());write_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());hash_fail=true;
    CHECK(upload(m)==OTA_ERR_IMAGE&&ota_storage_recovery_required());hash_fail=false;
    reset_nv();reset_ram();CHECK(!init_service());CHECK(!ota_storage_stage_begin(&m));
    CHECK(!ota_storage_stage_append(0,expected_artifact,200));flash[1][1008]=0x77;
    CHECK(ota_storage_stage_end(&m)==OTA_ERR_IMAGE&&ota_storage_recovery_required());
    reset_nv();reset_ram();CHECK(!init_service());m.mcuboot_image_hash[0]^=1;
    CHECK(upload(m)==OTA_ERR_IMAGE&&ota_storage_recovery_required());m.mcuboot_image_hash[0]^=1;
    reset_nv();reset_ram();CHECK(!init_service());expected_artifact[148]=0xa1;
    CHECK(upload(m)==OTA_ERR_COMPATIBILITY&&ota_storage_recovery_required());expected_artifact[148]=0xa0;
    reset_nv();reset_ram();CHECK(!init_service());expected_artifact[152]='x';
    CHECK(upload(m)==OTA_ERR_COMPATIBILITY&&ota_storage_recovery_required());expected_artifact[152]='r';
    reset_nv();reset_ram();CHECK(!init_service());m=artifact();
    expected_artifact[10]=28;expected_artifact[12]=100;
    expected_artifact[132]=0x08;expected_artifact[133]=0x69;expected_artifact[134]=28;expected_artifact[135]=0;
    expected_artifact[136]=0xa0;expected_artifact[137]=0;expected_artifact[138]=8;expected_artifact[139]=0;
    memcpy(expected_artifact+140,"receiver",8); /* Second class TLV remains at148. */
    CHECK(upload(m)==OTA_ERR_FORMAT&&ota_storage_recovery_required());m=artifact();
    return journal_gc_regressions();
}
extern "C" int ota_storage_gate_test_run()
{
    reset_nv(); reset_ram(); ota_manifest m=artifact(); CHECK(!init_service());
    unsigned erase_before=erased, writes_before=nvs_writes;
    CHECK(ota_storage_stage_begin(&m)==OTA_ERR_COMPATIBILITY);
    ota_participant_hooks hooks;ota_storage_hooks(&hooks);
    CHECK(hooks.prepare(nullptr,&m,false)==OTA_ERR_COMPATIBILITY);
    CHECK(hooks.arm(nullptr,&m,77)==OTA_ERR_STATE);
    CHECK(erased==erase_before&&nvs_writes==writes_before&&!armed&&!writer_open);
#if defined(CONFIG_OWNTECH_OTA_LEAD) && CONFIG_OWNTECH_OTA_LEAD
    ota_target target{};target.identity.eui[7]=2;
    CHECK(!ota_storage_persist_campaign(&m,&target,1,77,OTA_PREPARING));
    reset_ram();CHECK(!init_service());
    fleet_record corrupt{};CHECK(!load(FLEET_KEY,corrupt,FLEET_MAGIC));
    corrupt.manifest[157]=0;CHECK(!store(FLEET_KEY,corrupt));
    reset_ram();CHECK(ota_storage_init()==OTA_ERR_JOURNAL);
#endif
    return 0;
}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main(){int rc=ota_storage_test_run();if(rc) fprintf(stderr,"storage_test.cpp:%d\n",rc);return rc?1:0;}
#endif
