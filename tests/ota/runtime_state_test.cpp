/* Real runtime worker/API with deterministic queues and fake Zephyr boundaries.
 * Worker entry is exercised through initialize_runtime/worker_iteration and
 * focused process/reconcile_step calls for the protocol race regressions.
 * Portable participant/coordinator/protocol sources are linked unchanged.
 * No application OTA health or maintenance callback is linked. */
#define CONFIG_OWNTECH_OTA_LEAD 1
#define CONFIG_OWNTECH_OTA_USABLE_SLOT_SIZE 768
#define CONFIG_OWNTECH_OTA_HARDWARE_ID 1
#define CONFIG_OWNTECH_OTA_LAYOUT_ID 2
#define CONFIG_OWNTECH_OTA_BOOTLOADER_ID 3
#include "ota_lead_runtime.cpp"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

uint8_t eui64[8]={0,0,0,0,0,0,0,1};
static thingset_can_context fake_can={1,1,1,0};
static thingset_can_state_callback_t fake_can_callback;
static void *fake_can_callback_arg;
static uint64_t fake_now;
static bool fake_confirmed=true,fake_inhibited=true,fake_role,fake_recovery,fake_maintenance;
static bool fake_flushed;
static int fake_safety_result,fake_network_result,fake_hash_result,fake_journal_result;
static int fake_safety_enter_result;
static unsigned fake_safety_checks,fake_safety_entries;
static unsigned fake_network_inits;
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
static unsigned fake_identity_reads,fake_receive_calls;
static int fake_receive_timeout;
static void (*fake_wait_event)();
static void (*fake_sleep_event)();
static int fake_event_result;
static void pump();
static int local_flush(void *);
extern "C" int strncmp(const char *a,const char *b,size_t n){
    while(n--){if(*a!=*b)return (unsigned char)*a-(unsigned char)*b;if(!*a)return 0;++a;++b;}return 0;
}

int64_t k_uptime_get(){return (int64_t)fake_now;}
void k_sleep(int ms){fake_now+=ms;if(fake_sleep_event){auto event=fake_sleep_event;fake_sleep_event=nullptr;event();}}
void runtime_test_msgq_wait(k_msgq *q,int timeout){
    ++fake_receive_calls;fake_receive_timeout=timeout;
    if(!q->count && timeout!=K_NO_WAIT){
        /* Inject an API producer exactly between wait selection and dequeue.
         * An empty forever wait otherwise represents a suspended worker. */
        if(fake_wait_event){auto event=fake_wait_event;fake_wait_event=nullptr;event();}
        else if(timeout>0)fake_now+=(unsigned)timeout;
    }
}
int k_sem_take(k_sem *s,int){if(!s->count)pump();if(!s->count)return -1;--s->count;return 0;}
thingset_can_context *thingset_can_get_inst(){return &fake_can;}
void thingset_can_set_state_callback(thingset_can_state_callback_t callback,void *arg){
    fake_can_callback=callback;fake_can_callback_arg=arg;if(callback)callback(arg);
}
static void can_changed(){if(fake_can_callback)fake_can_callback(fake_can_callback_arg);}
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
        else fake_peers[i].status.state=OTA_RECOVERY_REQUIRED;
    }
    return 0;
}
extern "C" int ota_safety_check(){++fake_safety_checks;return fake_safety_result;}
extern "C" bool ota_safety_inhibited(){return fake_inhibited;}
extern "C" void ota_safety_restore(bool value){fake_inhibited=value;}
extern "C" int ota_safety_enter(){fake_inhibited=true;++fake_safety_entries;return fake_safety_enter_result;}
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
    ++fake_identity_reads;
    id->protocol_version=OTA_PROTOCOL_VERSION;id->image_class=OTA_IMAGE_RECEIVER;id->usable_slot_size=768;id->usable_image_size=768;
    id->hardware_id=1;id->layout_id=2;id->bootloader_id=3;
    id->active_confirmed=fake_confirmed;id->slot_available=fake_owner==OTA_SLOT_NONE&&!fake_recovery;
}
int ota_storage_active_hash(uint8_t hash[32]){if(fake_hash_result)return fake_hash_result;memcpy(hash,fake_active_hash,32);return 0;}
int ota_storage_get_journal(ota_storage_journal *j){*j=fake_journal;return fake_journal_result;}
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
void ota_storage_hooks(ota_participant_hooks *h){*h={nullptr,local_prepare,local_append,local_flush,local_validate,local_journal,local_reboot,local_close,nullptr};}
extern "C" int ota_network_init(){++fake_network_inits;return fake_network_result;}
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
extern "C" int ota_network_release(uint8_t,const uint8_t identity[8],const uint8_t hash[32],uint64_t){
    unsigned i=identity[7]-2;if(i>=2||memcmp(hash,fake_peers[i].active_mcuboot_image_hash,32))return OTA_ERR_IDENTITY;
    ++fake_release_requests;fake_peers[i].status.state=OTA_SUCCEEDED;return 0;
}
static void pump(){Work w{};if(!k_msgq_get(&ota_queue,&w,K_NO_WAIT))process(w);}
static ota_manifest make_manifest(){
    ota_manifest m={};m.campaign_id=42;m.image_size=768;m.image_content_size=768;m.hardware_id=1;m.layout_id=2;m.bootloader_id=3;
    m.protocol_version=OTA_PROTOCOL_VERSION;m.image_class=OTA_IMAGE_RECEIVER;memset(m.artifact_sha256,0xaa,32);memset(m.mcuboot_image_hash,0xbb,32);
    memcpy(m.version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
    memcpy(m.build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));return m;
}
static void reset_runtime(){
    losses=initialized=busy=healthy=local_healthy=can_ready=lead_role=identity_conflict=discovery_requested=stage_end_requested=reconcile_mode=refresh_pending=0;
    participant={};coordinator={};staged_manifest={};local_snapshot={};runtime_snapshot={};diagnostics_snapshot={"BOOT",0,false,false,false,false,false};
    memset(inventory,0,sizeof(inventory));memset(frozen,0,sizeof(frozen));inventory_count=frozen_count=0;
    discovery_active=discovery_done=staged=staging=verifying=reconcile_active=waiting_can=false;
    probe_address=1;discovery_deadline=campaign_id=reconcile_deadline=discovery_token=0;discovery_reserved=false;stage_offset=pass_snapshot=0;
    service_error=0;stage_state=OTA_IDLE;phase_snapshot="BOOT";memset(reconcile_hash,0,32);
    reconcile_manifest={};reconcile_commit=0;source_window={};last_source={};accepted_source={};event_mask=next_event_order=event_campaign=0;
    memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
    accepted_start={};accepted_reconcile={};accepted_prepare_campaign=next_stream_poll=stream_poll_target=0;
    ota_queue.head=ota_queue.count=0;fake_now=0;fake_can={1,1,1,0};
    fake_can_callback=nullptr;fake_can_callback_arg=nullptr;fake_network_inits=0;
    fake_identity_reads=fake_receive_calls=0;fake_receive_timeout=K_NO_WAIT;fake_wait_event=fake_sleep_event=nullptr;fake_event_result=0;
}
static void reset_all(){
    reset_runtime();fake_confirmed=true;fake_inhibited=true;fake_role=fake_recovery=fake_maintenance=false;
    fake_safety_result=fake_safety_enter_result=fake_network_result=fake_hash_result=fake_journal_result=0;
    fake_safety_checks=fake_safety_entries=0;
    fake_confirms=fake_prepares=fake_appends=fake_flushes=fake_reboots=fake_release_requests=0;
    fake_owner=OTA_SLOT_NONE;fake_journal={};fake_manifest={};fake_fleet_manifest={};fake_fleet_count=0;fake_offset=fake_fleet_commit=0;fake_flushed=false;
    memset(fake_active_hash,0xbb,32);memset(fake_lead,0,8);
    for(unsigned i=0;i<2;i++){
        fake_peers[i]={};auto &o=fake_peers[i];o.identity.eui[7]=i+2;o.identity.address=i+2;ota_storage_boot_identity(&o.identity);
        o.healthy=o.confirmed=true;memset(o.active_mcuboot_image_hash,0xbb,32);
        memcpy(o.active_version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
        memcpy(o.active_build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));fake_peer_present[i]=true;
    }
}
static void discover_all(uint64_t token=0){
    (void)ota_service_discover(token);pump();
    for(unsigned i=0;i<300&&discovery_active;i++){fake_now+=5;discovery_step();publish();}
}

static const uint8_t ids[2][8]={{0,0,0,0,0,0,0,2},{0,0,0,0,0,0,0,3}};
static int start_campaign(uint64_t id=42){
    discover_all(id);CHECK(discovery_done&&inventory_count==2&&!ota_service_busy());
    auto m=make_manifest();m.campaign_id=id;CHECK(!ota_service_stage_begin(&m));CHECK(!ota_service_stage_begin(&m));
    auto contradictory=m;contradictory.artifact_sha256[0]^=1;CHECK(ota_service_stage_begin(&contradictory)==OTA_ERR_CONFLICT);
    pump();CHECK(staging);
    CHECK(!ota_service_stage_end());pump();CHECK(staged);
    CHECK(!fake_prepares&&!fake_appends&&!fake_flushes);
    CHECK(!ota_service_start(id,ids,2));CHECK(!ota_service_start(id,ids,2));pump();return 0;
}
static int serve_source(){
    uint64_t id;uint32_t off,n;ota_service_source_request(&id,&off,&n);
    if(n){
        uint8_t data[OTA_MAX_PAYLOAD];memset(data,0x5a,sizeof(data));CHECK(n<=sizeof(data)&&off+n<=768);
        if(last_source.ready) {
            CHECK(!ota_service_source_data(last_source.campaign,last_source.offset,data,last_source.length));
            data[0]^=1;CHECK(ota_service_source_data(last_source.campaign,last_source.offset,data,last_source.length)==OTA_ERR_CONFLICT);data[0]^=1;
        }
        CHECK(ota_service_source_data(id+1,off,data,n)==OTA_ERR_CONFLICT);
        CHECK(ota_service_source_data(id,off+1,data,n)==OTA_ERR_CONFLICT);
        CHECK(!ota_service_source_data(id,off,data,n));CHECK(!ota_service_source_data(id,off,data,n));
        data[0]^=1;CHECK(ota_service_source_data(id,off,data,n)==OTA_ERR_CONFLICT);
    }return 0;
}
static int dedicated_cycles(){
    reset_all();initialize_runtime();CHECK(ota_service_healthy()&&ota_service_is_lead());
    CHECK(ota_service_set_role(false)==OTA_ERR_COMPATIBILITY);
    ota_command cmd{};cmd.manifest=make_manifest();CHECK(ota_runtime_command(&cmd,2)==OTA_ERR_COMPATIBILITY);
    for(uint64_t id=42;id<44;id++){
        int rc=start_campaign(id);if(rc)return rc;
        for(unsigned i=0;i<1000&&coordinator.phase!=OTA_COORD_VALIDATE_BARRIER;i++){
            worker_iteration();CHECK(!service_error);rc=serve_source();if(rc)return rc;
        }
        CHECK(coordinator.phase==OTA_COORD_VALIDATE_BARRIER&&fake_fleet_count==2);
        CHECK(!coordinator.targets[0].is_lead&&!coordinator.targets[1].is_lead);
        CHECK(!ota_service_commit(id));
        for(unsigned i=0;i<1000&&strncmp(ota_service_phase(),"SUCCESS",8);i++){worker_iteration();CHECK(!service_error);}
        CHECK(!strncmp(ota_service_phase(),"SUCCESS",8)&&!ota_service_busy());
        CHECK(!fake_prepares&&!fake_appends&&!fake_flushes&&!fake_reboots);
    }
    CHECK(fake_release_requests==4);unsigned reads=fake_identity_reads;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&reads==fake_identity_reads);return 0;
}
static int failures(){
    reset_all();initialize_runtime();auto m=make_manifest();m.image_class=OTA_IMAGE_LEAD;
    CHECK(ota_service_stage_begin(&m)==OTA_ERR_COMPATIBILITY);
    m=make_manifest();m.image_content_size--;CHECK(ota_service_stage_begin(&m)==OTA_ERR_COMPATIBILITY);
    int rc=start_campaign();if(rc)return rc;
    for(unsigned i=0;i<500&&!source_window.requested;i++)worker_iteration();
    CHECK(source_window.requested&&!source_window.ready);fake_now=source_window.deadline;worker_iteration();
    CHECK(service_error==OTA_ERR_TIMEOUT&&coordinator.phase==OTA_COORD_FAILED);
    CHECK(fake_peers[0].status.state!=OTA_COMMITTED&&!fake_prepares);
    /* Core startup confirms independently of main.cpp and remote peers. Once
     * CAN becomes ready, the same runtime can perform an ordinary campaign. */
    reset_all();fake_confirmed=false;fake_can.ready=0;initialize_runtime();
    CHECK(fake_safety_entries==1&&fake_safety_checks==1&&fake_confirms==1&&ota_service_local_healthy()&&!ota_service_can_ready());
    CHECK(!fake_inhibited&&!ota_service_healthy());
    fake_can.ready=1;can_changed();worker_iteration();CHECK(ota_service_healthy());
    rc=start_campaign();if(rc)return rc;
    /* Independence from application health does not bypass platform safety
     * or persistent maintenance/recovery from a previous interrupted boot. */
    reset_all();fake_confirmed=false;fake_safety_result=-1;initialize_runtime();
    CHECK(fake_safety_checks==1&&!fake_confirms&&!ota_service_healthy()&&fake_inhibited);
    CHECK(ota_service_error()==OTA_ERR_SAFETY&&!ota_service_local_healthy());
    m=make_manifest();CHECK(ota_service_stage_begin(&m)==OTA_ERR_STATE);
    CHECK(ota_service_discover(42)==OTA_ERR_STATE&&!fake_prepares&&!fake_fleet_count);
    reset_all();fake_confirmed=false;fake_safety_enter_result=-1;initialize_runtime();
    CHECK(fake_safety_entries==1&&!fake_confirms&&!ota_service_healthy()&&fake_inhibited);
    CHECK(ota_service_error()==OTA_ERR_SAFETY&&!ota_service_local_healthy());
    reset_all();fake_confirmed=false;fake_recovery=true;initialize_runtime();
    CHECK(!fake_confirms&&fake_inhibited&&!ota_service_healthy()&&ota_service_error()==OTA_ERR_JOURNAL);
    reset_all();fake_confirmed=false;fake_maintenance=true;initialize_runtime();
    CHECK(!fake_confirms&&fake_inhibited&&!ota_service_healthy()&&ota_service_error()==OTA_ERR_JOURNAL);
    reset_all();initialize_runtime();rc=start_campaign();if(rc)return rc;fake_peer_present[1]=false;
    for(unsigned i=0;i<5000&&!service_error;i++){worker_iteration();rc=serve_source();if(rc)return rc;}
    CHECK(service_error&&frozen_count==2&&!fake_prepares&&!fake_reboots);
    return 0;
}
extern "C" int ota_runtime_state_test_run(){int rc=dedicated_cycles();return rc?rc:failures();}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main(){int rc=ota_runtime_state_test_run();if(rc)fprintf(stderr,"runtime_state_test.cpp:%d\n",rc);return rc?1:0;}
#endif
