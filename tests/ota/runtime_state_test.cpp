/* Real runtime worker/API with deterministic queues and fake Zephyr boundaries.
 * Worker entry is exercised through initialize_runtime/process/reconcile_step.
 * Portable participant/coordinator/protocol sources are linked unchanged. */
#include "ota_runtime.cpp"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

uint8_t eui64[8]={0,0,0,0,0,0,0,1};
static thingset_can_context fake_can={1,1};
static uint64_t fake_now;
static bool fake_confirmed=true,fake_inhibited=true,fake_role,fake_recovery,fake_maintenance;
static bool fake_flushed;
static int fake_health_result,fake_network_result;
static unsigned fake_confirms,fake_prepares,fake_appends,fake_flushes,fake_reboots,fake_release_requests;
static ota_slot_owner fake_owner=OTA_SLOT_NONE;
static ota_storage_journal fake_journal;
static ota_manifest fake_manifest,fake_fleet_manifest;
static ota_target fake_fleet[OTA_MAX_TARGETS];
static size_t fake_fleet_count;
static uint32_t fake_fleet_commit;
static uint8_t fake_active_hash[32];
static ota_observation fake_peers[2];
static bool fake_peer_present[2]={true,true};
static uint32_t fake_offset;
static uint8_t fake_lead[8];
static void pump();
static int local_flush(void *);
extern "C" int strncmp(const char *a,const char *b,size_t n){
    while(n--){if(*a!=*b)return (unsigned char)*a-(unsigned char)*b;if(!*a)return 0;++a;++b;}return 0;
}

int64_t k_uptime_get(){return (int64_t)fake_now;}
void k_sleep(int ms){fake_now+=ms;}
int k_sem_take(k_sem *s,int){if(!s->count)pump();if(!s->count)return -1;--s->count;return 0;}
thingset_can_context *thingset_can_get_inst(){return &fake_can;}
bool boot_is_img_confirmed(){return fake_confirmed;}
int boot_write_img_confirmed(){++fake_confirms;fake_confirmed=true;return 0;}
int thingset_can_announce_address(int){return 0;}
int thingset_can_probe_address(uint8_t address,int){
    for(unsigned i=0;i<2;i++)if(fake_peer_present[i]&&fake_peers[i].identity.address==address)
        ota_runtime_claim(fake_peers[i].identity.eui,address);
    return 0;
}
int thingset_can_send_raw_report(const uint8_t *bytes,size_t n,int){
    ota_report report;int rc=ota_report_decode(bytes,n,false,&report);if(rc)return rc;
    for(unsigned i=0;i<2;i++)if(fake_peer_present[i]){
        if(report.type==OTA_REPORT_DATA){fake_peers[i].status.offset=report.offset+report.payload_len;}
        else fake_peers[i].status.state=OTA_REBOOTING;
    }
    return 0;
}
extern "C" int owntech_ota_enter_maintenance(){return 0;}
extern "C" int owntech_ota_check_health(){return fake_health_result;}
extern "C" bool ota_safety_inhibited(){return fake_inhibited;}
extern "C" void ota_safety_restore(bool value){fake_inhibited=value;}
extern "C" int ota_safety_enter(){fake_inhibited=true;return 0;}
extern "C" void ota_feedback_state(ota_state){}
extern "C" void ota_feedback_application_led(int){}
int ota_storage_init(){return 0;}
bool ota_storage_recovery_required(){return fake_recovery;}
bool ota_storage_maintenance(){return fake_maintenance;}
ota_slot_owner ota_storage_owner(){return fake_owner;}
int ota_storage_load_role(bool *lead){*lead=fake_role;return 0;}
int ota_storage_persist_role(bool lead){if(fake_owner!=OTA_SLOT_NONE||fake_recovery)return OTA_ERR_STATE;fake_role=lead;return 0;}
int ota_storage_expect_lead(const uint8_t id[8]){memcpy(fake_lead,id,8);return 0;}
void ota_storage_set_events(uint32_t mask,const uint32_t times[12],const uint8_t order[12]){
    fake_journal.event_mask=mask;memcpy(fake_journal.event_ms,times,48);memcpy(fake_journal.event_order,order,12);
}
void ota_storage_boot_identity(ota_identity *id){
    id->protocol_version=1;id->usable_slot_size=768;id->usable_image_size=600;
    id->hardware_id=1;id->layout_id=2;id->bootloader_id=3;
    id->active_confirmed=fake_confirmed;id->slot_available=fake_owner==OTA_SLOT_NONE&&!fake_recovery;
}
int ota_storage_active_hash(uint8_t hash[32]){memcpy(hash,fake_active_hash,32);return 0;}
int ota_storage_get_journal(ota_storage_journal *j){*j=fake_journal;return 0;}
static int local_journal(void *,const ota_manifest *m,ota_state s,uint32_t commit){
    fake_journal.campaign_id=m->campaign_id;fake_journal.commit_id=commit;fake_journal.image_size=m->image_size;
    fake_journal.state=s;memcpy(fake_journal.lead_eui,fake_lead,8);
    memcpy(fake_journal.artifact_sha256,m->artifact_sha256,32);memcpy(fake_journal.mcuboot_image_hash,m->mcuboot_image_hash,32);
    memcpy(fake_journal.version,m->version,32);memcpy(fake_journal.build_id,m->build_id,32);return 0;
}
int ota_storage_stage_begin(const ota_manifest *m){
    if(fake_owner!=OTA_SLOT_NONE||fake_recovery)return OTA_ERR_STATE;
    fake_manifest=*m;fake_offset=0;fake_flushed=false;fake_owner=OTA_SLOT_USB;fake_maintenance=fake_inhibited=true;++fake_prepares;
    return local_journal(nullptr,m,OTA_PREPARING,0);
}
int ota_storage_stage_append(uint32_t off,const uint8_t *,size_t n){
    if(off!=fake_offset||off+n>fake_manifest.image_size)return OTA_ERR_OFFSET;
    fake_offset+=n;++fake_appends;return 0;
}
int ota_storage_stage_end(const ota_manifest *m){
    if(fake_offset!=m->image_size)return OTA_ERR_INCOMPLETE;
    local_flush(nullptr);return local_journal(nullptr,m,OTA_VALID,0);
}
int ota_storage_read(uint32_t,uint8_t *data,size_t n){memset(data,0x5a,n);return 0;}
void ota_storage_abort(){fake_recovery=true;fake_journal.state=OTA_ABORTED;}
int ota_storage_persist_campaign(const ota_manifest *m,const ota_target *t,size_t n,uint32_t commit,ota_state){
    fake_fleet_manifest=*m;memcpy(fake_fleet,t,n*sizeof(*t));fake_fleet_count=n;fake_fleet_commit=commit;return 0;
}
int ota_storage_load_campaign(ota_manifest *m,ota_target *t,size_t *n,uint32_t *commit){
    if(!fake_fleet_count||*n<fake_fleet_count)return OTA_ERR_JOURNAL;
    *m=fake_fleet_manifest;*n=fake_fleet_count;*commit=fake_fleet_commit;memcpy(t,fake_fleet,*n*sizeof(*t));return 0;
}
int ota_storage_release_maintenance(const uint8_t hash[32]){
    if(memcmp(hash,fake_active_hash,32)||!fake_confirmed||fake_reboots)return OTA_ERR_STATE;
    fake_maintenance=fake_recovery=fake_inhibited=false;fake_owner=OTA_SLOT_NONE;fake_journal.state=OTA_SUCCEEDED;return 0;
}
static int local_prepare(void *,const ota_manifest *m,bool adopt){
    if(adopt){if(fake_owner!=OTA_SLOT_USB)return OTA_ERR_STATE;fake_owner=OTA_SLOT_LEAD;return 0;}
    fake_manifest=*m;fake_owner=OTA_SLOT_PARTICIPANT;++fake_prepares;return 0;
}
static int local_append(void *,uint32_t off,const uint8_t *data,size_t n){return ota_storage_stage_append(off,data,n);}
static int local_flush(void *){if(!fake_flushed){++fake_flushes;fake_flushed=true;}return 0;}
static int local_validate(void *,const ota_manifest *){return 0;}
static int local_reboot(void *,uint32_t,uint32_t){++fake_reboots;return 0;}
static void local_close(void *){fake_recovery=fake_maintenance;}
void ota_storage_hooks(ota_participant_hooks *h){*h={nullptr,local_prepare,local_append,local_flush,local_validate,local_journal,local_reboot,local_close};}
extern "C" int ota_network_init(){return fake_network_result;}
extern "C" int ota_network_command(const ota_target *target,const ota_command *cmd){
    unsigned i=target->identity.eui[7]-2;if(i>=2||!fake_peer_present[i])return OTA_ERR_TIMEOUT;
    auto &s=fake_peers[i].status;s.campaign_id=cmd->manifest.campaign_id;s.image_size=cmd->manifest.image_size;
    switch(cmd->type){
    case OTA_CMD_PREPARE:s.state=OTA_READY;s.offset=0;break;
    case OTA_CMD_BEGIN_PASS:s.state=OTA_PASS_OPEN;s.pass_id=cmd->pass_id;break;
    case OTA_CMD_END_PASS:s.state=OTA_PASS_CLOSED;break;
    case OTA_CMD_FINALIZE:s.state=OTA_VALID;s.flash_complete=s.validated=true;break;
    case OTA_CMD_COMMIT:s.state=OTA_COMMITTED;s.commit_id=cmd->commit_id;break;
    case OTA_CMD_ABORT:s.state=OTA_ABORTED;break;
    }
    return 0;
}
extern "C" int ota_network_status(uint8_t address,ota_observation *o){
    for(unsigned i=0;i<2;i++)if(fake_peer_present[i]&&fake_peers[i].identity.address==address){*o=fake_peers[i];return 0;}
    return OTA_ERR_TIMEOUT;
}
extern "C" int ota_network_release(uint8_t,const uint8_t identity[8],const uint8_t hash[32]){
    unsigned i=identity[7]-2;if(i>=2||memcmp(hash,fake_peers[i].active_mcuboot_image_hash,32))return OTA_ERR_IDENTITY;
    ++fake_release_requests;fake_peers[i].status.state=OTA_SUCCEEDED;return 0;
}
static void pump(){Work w{};if(!k_msgq_get(&ota_queue,&w,K_NO_WAIT))process(w);}
static ota_manifest make_manifest(){
    ota_manifest m={};m.campaign_id=42;m.image_size=768;m.image_content_size=600;m.hardware_id=1;m.layout_id=2;m.bootloader_id=3;
    m.protocol_version=1;memset(m.artifact_sha256,0xaa,32);memset(m.mcuboot_image_hash,0xbb,32);
    memcpy(m.version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
    memcpy(m.build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));return m;
}
static void reset_runtime(){
    losses=initialized=busy=healthy=lead_role=identity_conflict=discovery_requested=stage_end_requested=usb_pending=reconcile_mode=release_pending=0;
    participant={};coordinator={};staged_manifest={};local_snapshot={};runtime_snapshot={};
    memset(inventory,0,sizeof(inventory));memset(frozen,0,sizeof(frozen));inventory_count=frozen_count=0;
    discovery_active=discovery_done=staged=staging=verifying=reconcile_active=false;
    probe_address=1;discovery_deadline=campaign_id=reconcile_deadline=0;stage_offset=pass_snapshot=0;
    service_error=usb_result=0;stage_state=OTA_IDLE;phase_snapshot="BOOT";memset(reconcile_hash,0,32);
    reconcile_manifest={};reconcile_commit=0;storage_hooks={};event_mask=next_event_order=event_campaign=0;
    memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
    accepted_start={};accepted_reconcile={};accepted_prepare_campaign=next_stream_poll=stream_poll_target=0;
    ota_queue.head=ota_queue.count=0;k_sem_reset(&usb_done);fake_now=0;fake_can.ready=1;fake_can.node_addr=1;
}
static void reset_all(){
    reset_runtime();fake_confirmed=true;fake_inhibited=true;fake_role=fake_recovery=fake_maintenance=false;
    fake_health_result=fake_network_result=0;fake_confirms=fake_prepares=fake_appends=fake_flushes=fake_reboots=fake_release_requests=0;
    fake_owner=OTA_SLOT_NONE;fake_journal={};fake_manifest={};fake_fleet_manifest={};fake_fleet_count=0;fake_offset=fake_fleet_commit=0;fake_flushed=false;
    memset(fake_active_hash,0xbb,32);memset(fake_lead,0,8);
    for(unsigned i=0;i<2;i++){
        fake_peers[i]={};auto &o=fake_peers[i];o.identity.eui[7]=i+2;o.identity.address=i+2;ota_storage_boot_identity(&o.identity);
        o.healthy=o.confirmed=true;memset(o.active_mcuboot_image_hash,0xbb,32);
        memcpy(o.active_version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
        memcpy(o.active_build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));fake_peer_present[i]=true;
    }
}
static void discover_all(){
    (void)ota_service_discover();pump();
    for(unsigned i=0;i<300&&discovery_active;i++){fake_now+=5;discovery_step();publish();}
}
static void restore_boot_storage(){fake_owner=OTA_SLOT_NONE;fake_recovery=fake_maintenance=true;fake_reboots=0;fake_inhibited=true;}
static const uint8_t ids[3][8]={{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,2},{0,0,0,0,0,0,0,3}};

static int nominal_test(){
    reset_all();initialize_runtime();CHECK(ota_service_healthy()&&!fake_inhibited);
    CHECK(!ota_service_set_role(true));discover_all();CHECK(discovery_done&&inventory_count==3);
    ota_manifest m=make_manifest();CHECK(!ota_service_stage_begin(&m));pump();CHECK(staging&&stage_state==OTA_READY);
    uint8_t data[256]={};for(uint32_t pos=0;pos<768;pos+=256)CHECK(!ota_service_stage_data(pos,data,256));
    CHECK(!ota_service_stage_end());pump();CHECK(staged&&fake_prepares==1&&fake_flushes==1);
    CHECK(event_order[OTA_EVENT_ERASE_BEGIN]<event_order[OTA_EVENT_ERASE_END]);
    CHECK(event_order[OTA_EVENT_USB_STAGE_BEGIN]<event_order[OTA_EVENT_USB_STAGE_END]);
    CHECK(event_order[OTA_EVENT_FLASH_COMPLETE]<event_order[OTA_EVENT_VERIFY_BEGIN]);
    CHECK(event_order[OTA_EVENT_VERIFY_BEGIN]<event_order[OTA_EVENT_VERIFY_END]);
    CHECK(!ota_service_start(42,ids,3));
    CHECK(ota_service_start(42,ids,3)>=0); /* Retry before first worker execution. */
    pump();if(k_msgq_num_used_get(&ota_queue))pump();CHECK(!service_error);
    for(unsigned i=0;i<500&&coordinator.phase!=OTA_COORD_VALIDATE_BARRIER;i++){
        ++fake_now;CHECK(ota_coordinator_step(&coordinator,fake_now)==OTA_AGAIN);publish();
    }
    CHECK(coordinator.phase==OTA_COORD_VALIDATE_BARRIER&&fake_fleet_count==3&&!fake_reboots);
    CHECK(!ota_service_commit(42));pump();
    for(unsigned i=0;i<100&&!fake_reboots;i++){++fake_now;CHECK(ota_coordinator_step(&coordinator,fake_now)==OTA_AGAIN);publish();}
    CHECK(fake_reboots==1&&fake_journal.state==OTA_REBOOTING);
    uint8_t reboot_order=fake_journal.event_order[OTA_EVENT_REBOOTING];CHECK(reboot_order>0);
    int abort_rc=ota_service_abort(42);
    if(abort_rc>=0)pump();
    CHECK(participant.status.state==OTA_REBOOTING&&fake_journal.state==OTA_REBOOTING&&fake_reboots==1);
    /* Preserve journal/frozen roster, clear all application RAM as at reset. */
    reset_runtime();restore_boot_storage();initialize_runtime();
    CHECK(ota_service_healthy()&&ota_storage_recovery_required()&&fake_inhibited);
    CHECK(event_order[OTA_EVENT_REBOOTING]==reboot_order&&event_order[OTA_EVENT_POSTBOOT_CHECK]>reboot_order);
    for(unsigned i=0;i<2;i++){fake_peers[i].status.state=OTA_RECOVERY_REQUIRED;fake_peers[i].status.error=0;}
    CHECK(!ota_service_reconcile(42,ids,3,m.mcuboot_image_hash));pump();
    for(unsigned i=0;i<300&&discovery_active;i++){fake_now+=5;discovery_step();publish();}
    CHECK(frozen_count==3&&reconcile_active);reconcile_step();publish();
    CHECK(fake_release_requests==2&&ota_service_busy()); /* Acceptance is not success. */
    reconcile_step();publish();CHECK(!ota_service_busy()&&!fake_inhibited&&participant.status.state==OTA_SUCCEEDED);
    CHECK(coordinator.phase==OTA_COORD_IDLE);
    discover_all();++m.campaign_id;CHECK(!ota_service_stage_begin(&m));pump();CHECK(staging&&stage_state==OTA_READY&&fake_prepares==2);
    return 0;
}
static int bootstrap_test(){
    reset_all();fake_confirmed=false;initialize_runtime();CHECK(fake_confirms==1&&ota_service_healthy());
    reset_all();fake_confirmed=false;fake_health_result=OTA_ERR_HEALTH;initialize_runtime();
    CHECK(!fake_confirms&&!ota_service_healthy()&&fake_inhibited);
    reset_all();fake_confirmed=false;fake_journal.campaign_id=42;fake_journal.state=OTA_REBOOTING;
    memset(fake_journal.mcuboot_image_hash,0xcc,32);fake_recovery=fake_maintenance=true;initialize_runtime();
    CHECK(!fake_confirms&&!ota_service_healthy()&&fake_inhibited&&local_snapshot.rolled_back);
    return 0;
}
static int reconcile_roster_test(){
    reset_all();fake_role=true;fake_manifest=make_manifest();fake_fleet_manifest=fake_manifest;fake_fleet_count=3;fake_fleet_commit=77;
    for(unsigned i=0;i<3;i++){memcpy(fake_fleet[i].identity.eui,ids[i],8);fake_fleet[i].is_lead=i==0;}
    local_journal(nullptr,&fake_manifest,OTA_REBOOTING,77);restore_boot_storage();initialize_runtime();
    CHECK(!ota_service_reconcile(42,ids,2,fake_manifest.mcuboot_image_hash));pump();
    CHECK(!reconcile_active&&service_error==OTA_ERR_IDENTITY&&fake_inhibited);
    return 0;
}
static int queued_prepare_test(){
    reset_all();initialize_runtime();
    ota_runtime_claim(ids[1],2);
    ota_command cmd={};cmd.type=OTA_CMD_PREPARE;cmd.manifest=make_manifest();
    memcpy(cmd.lead_eui,ids[1],8);cmd.lead_address=2;
    CHECK(!ota_runtime_command(&cmd,2));CHECK(ota_service_busy());
    CHECK(ota_service_set_role(true)==OTA_ERR_STATE);
    CHECK(ota_runtime_command(&cmd,2)>=0);pump();if(k_msgq_num_used_get(&ota_queue))pump();
    CHECK(fake_prepares==1&&participant.status.state==OTA_READY&&!service_error);
    return 0;
}
static int persisted_lead_participant_test(){
    reset_all();fake_role=true;initialize_runtime();ota_runtime_claim(ids[1],2);
    ota_command cmd={};cmd.type=OTA_CMD_PREPARE;cmd.manifest=make_manifest();
    memcpy(cmd.lead_eui,ids[1],8);cmd.lead_address=2;
    CHECK(!ota_runtime_command(&cmd,2));CHECK(ota_service_busy());
    CHECK(ota_runtime_command(&cmd,2)>=0);pump();if(k_msgq_num_used_get(&ota_queue))pump();
    CHECK(participant.status.state==OTA_READY&&fake_prepares==1);
    cmd.type=OTA_CMD_BEGIN_PASS;cmd.pass_id=1;cmd.start_offset=0;
    CHECK(!ota_runtime_command(&cmd,2));pump();CHECK(participant.status.state==OTA_PASS_OPEN);
    ota_manifest local=make_manifest();++local.campaign_id;CHECK(ota_service_stage_begin(&local)==OTA_ERR_STATE);
    return 0;
}
static int prepare_reconcile(){
    reset_all();fake_role=true;fake_manifest=make_manifest();fake_fleet_manifest=fake_manifest;fake_fleet_count=3;fake_fleet_commit=77;
    for(unsigned i=0;i<3;i++){memcpy(fake_fleet[i].identity.eui,ids[i],8);fake_fleet[i].is_lead=i==0;}
    local_journal(nullptr,&fake_manifest,OTA_REBOOTING,77);restore_boot_storage();initialize_runtime();
    for(unsigned i=0;i<2;i++){
        fake_peers[i].status.campaign_id=42;fake_peers[i].status.commit_id=77;
        fake_peers[i].status.image_size=fake_manifest.image_size;fake_peers[i].status.state=OTA_RECOVERY_REQUIRED;
    }
    CHECK(!ota_service_reconcile(42,ids,3,fake_manifest.mcuboot_image_hash));
    CHECK(ota_service_busy());ota_manifest next=make_manifest();++next.campaign_id;
    CHECK(ota_service_stage_begin(&next)==OTA_ERR_STATE);
    pump();for(unsigned i=0;i<300&&discovery_active;i++){fake_now+=5;discovery_step();publish();}
    CHECK(reconcile_active&&frozen_count==3);return 0;
}
static int reconcile_failure_test(){
    int rc=prepare_reconcile();if(rc)return rc;
    reconcile_step();CHECK(fake_release_requests==2);reconcile_step();CHECK(!fake_inhibited&&!ota_service_busy());
    rc=prepare_reconcile();if(rc)return rc;
    fake_peers[1].status.error=OTA_ERR_HEALTH;reconcile_step();publish();
    CHECK(!fake_release_requests&&fake_inhibited&&ota_service_busy());
    fake_now=reconcile_deadline+1;reconcile_step();CHECK(!reconcile_active&&service_error==OTA_ERR_TIMEOUT);
    rc=prepare_reconcile();if(rc)return rc;
    memset(fake_peers[1].active_build_id,0,32);memcpy(fake_peers[1].active_build_id,"wrong-build",12);
    reconcile_step();CHECK(!fake_release_requests&&fake_inhibited);
    rc=prepare_reconcile();if(rc)return rc;
    atomic_set(&identity_conflict,1);reconcile_step();CHECK(!fake_release_requests&&fake_inhibited);
    return 0;
}
static int release_source_test(){
    for(unsigned persisted_lead=0;persisted_lead<2;++persisted_lead){
        reset_all();fake_role=persisted_lead!=0;fake_manifest=make_manifest();memcpy(fake_lead,ids[1],8);
        local_journal(nullptr,&fake_manifest,OTA_REBOOTING,77);restore_boot_storage();initialize_runtime();
        ota_runtime_claim(ids[1],2);
        CHECK(ota_runtime_release(ids[0],fake_manifest.mcuboot_image_hash,3)!=OTA_OK);
        CHECK(!k_msgq_num_used_get(&ota_queue)&&fake_inhibited);
        CHECK(!ota_runtime_release(ids[0],fake_manifest.mcuboot_image_hash,2));
        CHECK(!ota_runtime_release(ids[0],fake_manifest.mcuboot_image_hash,2));pump();
        CHECK(!fake_inhibited&&participant.status.state==OTA_SUCCEEDED&&participant.status.campaign_id==42);
    }
    return 0;
}
static int stale_queue_after_release_test(){
    reset_all();fake_role=true;fake_manifest=make_manifest();memcpy(fake_lead,ids[1],8);
    local_journal(nullptr,&fake_manifest,OTA_REBOOTING,77);restore_boot_storage();initialize_runtime();
    ota_runtime_claim(ids[1],2);
    /* A delayed previous-campaign PREPARE reserves its control queue but cannot
     * restore the lost writer after reboot. The valid RELEASE follows it. */
    ota_command old={};old.type=OTA_CMD_PREPARE;old.manifest=fake_manifest;
    old.lead_address=2;memcpy(old.lead_eui,ids[1],8);
    CHECK(!ota_runtime_command(&old,2));pump();
    CHECK(participant.status.state==OTA_RECOVERY_REQUIRED&&fake_prepares==0);
    CHECK(!ota_runtime_release(ids[0],fake_manifest.mcuboot_image_hash,2));
    old.type=OTA_CMD_ABORT;CHECK(!ota_runtime_command(&old,2));
    old.type=OTA_CMD_PREPARE;CHECK(!ota_runtime_command(&old,2));
    pump();CHECK(participant.status.state==OTA_SUCCEEDED&&!ota_service_busy());
    ota_manifest next=make_manifest();++next.campaign_id;
    CHECK(!ota_service_stage_begin(&next)); /* Local slot reservation overtakes old queue entries. */
    uint32_t rejected=participant.status.rx_rejected;pump();pump();
    CHECK(participant.status.rx_rejected==rejected+2&&participant.status.state==OTA_SUCCEEDED);
    CHECK(!service_error&&fake_journal.state==OTA_SUCCEEDED&&fake_journal.campaign_id==42);
    pump();CHECK(staging&&stage_state==OTA_READY&&fake_journal.campaign_id==43&&fake_prepares==1);
    CHECK(!service_error&&fake_owner==OTA_SLOT_USB&&ota_service_busy());return 0;
}
extern "C" int ota_runtime_state_test_run(){
    int rc=bootstrap_test();if(rc)return rc;
    rc=nominal_test();if(rc)return rc;rc=reconcile_roster_test();if(rc)return rc;
    rc=queued_prepare_test();if(rc)return rc;rc=persisted_lead_participant_test();if(rc)return rc;
    rc=reconcile_failure_test();if(rc)return rc;rc=release_source_test();if(rc)return rc;
    return stale_queue_after_release_test();
}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main(){int rc=ota_runtime_state_test_run();if(rc)fprintf(stderr,"runtime_state_test.cpp:%d\n",rc);return rc?1:0;}
#endif
