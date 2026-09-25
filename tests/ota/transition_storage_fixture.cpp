/* Python-generated policy + actual ota_storage.cpp + actual transition core.
 * Reuse the storage test's in-memory NVS/flash boundaries, not its test runner. */
#include "owntech_ota_recovery_config.h"
#if OWNTECH_OTA_TRANSITION_ROLE == 2
#define CONFIG_OWNTECH_OTA_LEAD 1
#endif
#define main ota_storage_unused_main
#include "storage_test.cpp"
#undef main
#include "transition_core.h"

namespace {
const OtaTransitionConfig transition_config = {
    OWNTECH_OTA_TRANSITION_EUI_BYTES, OWNTECH_OTA_TRANSITION_ORIGINAL_HASH_BYTES,
    OWNTECH_OTA_TRANSITION_TOKEN_BYTES, OWNTECH_OTA_TRANSITION_ROLE,
    OWNTECH_OTA_TRANSITION_LOCAL_LENGTH, OWNTECH_OTA_TRANSITION_FLEET_LENGTH,
    OWNTECH_OTA_TRANSITION_LOCAL_BYTES, OWNTECH_OTA_TRANSITION_FLEET_BYTES
};
bool helper_confirmed;
unsigned transition_erases;
int transition_read(void *,uint16_t key,uint8_t *data,size_t n)
{ return nvs_storage_read(key,data,n); }
int transition_write(void *,uint16_t key,const uint8_t *data,size_t n)
{ return nvs_storage_write(key,data,n); }
int transition_erase(void *,uint16_t key)
{
    /* Model deletion of all prior versions; real NVS supplies tombstones.
     * GC/power-cut mechanics remain covered by their dedicated tests. */
    for(auto &sector: records) for(auto &record: sector) if(record.key==key) record.key=0;
    ++transition_erases; return 0;
}
int transition_identity(void *,uint8_t eui[8])
{ memcpy(eui,transition_config.eui,8); return 0; }
int transition_backup(void *,uint8_t hash[32])
{ return ota_storage_active_hash(hash); }
int transition_health(void *) { return 0; }
bool transition_confirmed(void *) { return helper_confirmed; }
int transition_confirm(void *) { helper_confirmed=true; return 0; }
}

extern "C" int ota_transition_storage_fixture_run()
{
    reset_nv(); reset_ram(); artifact(); confirmed=true; swap=BOOT_SWAP_TYPE_NONE;
    helper_confirmed=false; transition_erases=0;
    const auto &config=transition_config;
    uint8_t actual[308];
    if(config.role==1) {
        local_record encoded={}; memcpy(&encoded,config.local,sizeof(encoded));
        ota_storage_journal journal={}; CHECK(!decode_local(encoded,journal));
        ota_manifest m={}; m.campaign_id=journal.campaign_id; m.image_size=journal.image_size;
        m.image_content_size=m.image_size; m.protocol_version=journal.protocol_version;
        m.image_class=journal.image_class; memcpy(m.artifact_sha256,journal.artifact_sha256,32);
        memcpy(m.mcuboot_image_hash,journal.mcuboot_image_hash,32);
        memcpy(m.version,journal.version,32); memcpy(m.build_id,journal.build_id,32);
        memcpy(expected_lead_eui,journal.lead_eui,8);
        CHECK(!save_journal(&m,OTA_SUCCEEDED,journal.commit_id));
        CHECK(nvs_storage_read(JOURNAL_KEY,actual,sizeof(actual))==168 && !memcmp(actual,config.local,168));
    } else {
        ota_manifest m={}; decode_manifest(m,config.fleet+8);
        ota_target targets[OTA_MAX_TARGETS]={}; const unsigned count=config.fleet[294];
        for(unsigned i=0;i<count;++i) memcpy(targets[i].identity.eui,config.fleet+166+i*8,8);
        CHECK(!ota_storage_persist_campaign(&m,targets,count,ota_read_le32(config.fleet+300),OTA_SUCCEEDED));
        CHECK(nvs_storage_read(FLEET_KEY,actual,sizeof(actual))==308 && !memcmp(actual,config.fleet,308));
    }
    CHECK(!save_marker(MAINTENANCE_KEY,false));
    CHECK(!save_marker(ROLE_KEY,config.role==2));
    const uint8_t calibration[]={2,4,6,8};
    CHECK(nvs_storage_write(0x201,calibration,sizeof(calibration))==sizeof(calibration));
    /* The flash shim models MCUboot's active digest TLV. Cryptographic image
     * inspection is exercised before generation by the Python test. */
    memcpy(flash[0]+168,config.original_hash,32);
    reset_ram(); CHECK(!ota_storage_init() && !ota_storage_recovery_required());
    uint8_t hash[32]; CHECK(!ota_storage_active_hash(hash) && !memcmp(hash,config.original_hash,32));
    ota_storage_journal before={}; CHECK(!ota_storage_get_journal(&before));
    CHECK((config.role==1)==(before.campaign_id!=0));
    const OtaRecoveryIO io={nullptr,transition_read,transition_write,transition_erase,
        transition_identity,transition_backup,transition_health,transition_confirmed,transition_confirm};
    const unsigned flash_erases=erased;
    CHECK(ota_transition_run(config,io)==OTA_RECOVERED && helper_confirmed && transition_erases==4);
    CHECK(erased==flash_erases);
    CHECK(nvs_storage_read(0x201,actual,sizeof(actual))==sizeof(calibration) && !memcmp(actual,calibration,sizeof(calibration)));
    bool lead=false; CHECK(!ota_storage_load_role(&lead) && lead==(config.role==2));

    /* A restored application with a new execution hash must see fresh state,
     * never the former campaign's expected digest or a recovery condition. */
    flash[0][168]^=0x80; uint8_t new_hash[32]; memcpy(new_hash,flash[0]+168,32);
    reset_ram(); CHECK(!ota_storage_init() && !ota_storage_recovery_required() && !ota_storage_maintenance());
    ota_storage_journal after={}; CHECK(!ota_storage_get_journal(&after));
    CHECK(!after.campaign_id && !after.commit_id && after.state==OTA_IDLE);
    ota_manifest previous={}; ota_target targets[OTA_MAX_TARGETS]={}; size_t count=OTA_MAX_TARGETS; uint32_t commit=0;
    CHECK(ota_storage_load_campaign(&previous,targets,&count,&commit)==-ENOENT);
    CHECK(!ota_storage_active_hash(hash) && !memcmp(hash,new_hash,32) && memcmp(hash,config.original_hash,32));
    CHECK(!ota_storage_release_maintenance(new_hash) && !ota_storage_recovery_required());
    ota_identity identity={}; ota_storage_boot_identity(&identity);
    CHECK(identity.slot_available && identity.active_confirmed);
    CHECK(!ota_storage_get_journal(&after) && !after.campaign_id && !after.commit_id);
    CHECK(nvs_storage_read(JOURNAL_KEY,actual,sizeof(actual))==-ENOENT);
    return 0;
}
#ifndef OWNTECH_FREESTANDING_TEST
int main() { return ota_transition_storage_fixture_run(); }
#endif
