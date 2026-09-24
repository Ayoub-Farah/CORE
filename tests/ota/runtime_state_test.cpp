/* Real runtime worker/API with deterministic queues and fake Zephyr boundaries.
 * Worker entry is exercised through initialize_runtime/worker_iteration and
 * focused process/reconcile_step calls for the protocol race regressions.
 * Portable participant/coordinator/protocol sources are linked unchanged. */
#include "ota_runtime.cpp"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

uint8_t eui64[8]={0,0,0,0,0,0,0,1};
static thingset_can_context fake_can={1,1,1,0};
static thingset_can_state_callback_t fake_can_callback;
static void *fake_can_callback_arg;
static uint64_t fake_now;
static bool fake_confirmed=true,fake_inhibited=true,fake_role,fake_recovery,fake_maintenance;
static bool fake_flushed;
static int fake_health_result,fake_network_result,fake_hash_result,fake_journal_result;
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
    ++fake_identity_reads;
    id->protocol_version=1;id->usable_slot_size=768;id->usable_image_size=600;
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
void ota_storage_hooks(ota_participant_hooks *h){*h={nullptr,local_prepare,local_append,local_flush,local_validate,local_journal,local_reboot,local_close};}
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
    losses=initialized=busy=healthy=local_healthy=can_ready=lead_role=identity_conflict=discovery_requested=stage_end_requested=usb_pending=reconcile_mode=release_pending=refresh_pending=0;
    participant={};coordinator={};staged_manifest={};local_snapshot={};runtime_snapshot={};diagnostics_snapshot={"BOOT",0,false,false,false,false,false};
    memset(inventory,0,sizeof(inventory));memset(frozen,0,sizeof(frozen));inventory_count=frozen_count=0;
    discovery_active=discovery_done=staged=staging=verifying=reconcile_active=waiting_can=false;
    probe_address=1;discovery_deadline=campaign_id=reconcile_deadline=discovery_token=0;discovery_reserved=false;stage_offset=pass_snapshot=0;
    service_error=usb_result=0;stage_state=OTA_IDLE;phase_snapshot="BOOT";memset(reconcile_hash,0,32);
    reconcile_manifest={};reconcile_commit=0;storage_hooks={};event_mask=next_event_order=event_campaign=0;
    memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
    accepted_start={};accepted_reconcile={};accepted_prepare_campaign=next_stream_poll=stream_poll_target=0;
    ota_queue.head=ota_queue.count=0;k_sem_reset(&usb_done);fake_now=0;fake_can={1,1,1,0};
    fake_can_callback=nullptr;fake_can_callback_arg=nullptr;fake_network_inits=0;
    fake_identity_reads=fake_receive_calls=0;fake_receive_timeout=K_NO_WAIT;fake_wait_event=fake_sleep_event=nullptr;fake_event_result=0;
}
static void reset_all(){
    reset_runtime();fake_confirmed=true;fake_inhibited=true;fake_role=fake_recovery=fake_maintenance=false;
    fake_health_result=fake_network_result=fake_hash_result=fake_journal_result=0;fake_confirms=fake_prepares=fake_appends=fake_flushes=fake_reboots=fake_release_requests=0;
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
static void restore_boot_storage(){fake_owner=OTA_SLOT_NONE;fake_recovery=fake_maintenance=true;fake_reboots=0;fake_inhibited=true;}
static const uint8_t ids[3][8]={{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,2},{0,0,0,0,0,0,0,3}};
static const uint8_t followers_first[3][8]={{0,0,0,0,0,0,0,2},{0,0,0,0,0,0,0,3},{0,0,0,0,0,0,0,1}};
static int check_campaign_snapshot(uint64_t campaign){
    ota_observation local{};ota_service_local(&local);
    CHECK(local.status.campaign_id==campaign&&local.status.image_size==768);
    CHECK(local.event_mask&&local.event_mask==event_mask);
    unsigned count=0,orders=0;
    for(unsigned i=0;i<OTA_EVENT_COUNT;i++){
        if(local.event_mask&(1U<<i)){
            unsigned order=local.event_order[i];CHECK(order&&order<=OTA_EVENT_COUNT&&!(orders&(1U<<order)));
            orders|=1U<<order;++count;
        }else CHECK(!local.event_ms[i]&&!local.event_order[i]);
    }
    CHECK(orders==((1U<<(count+1))-2));
    bool found=false;
    for(size_t i=0;i<ota_service_target_count();i++){
        ota_observation row{};bool lead=false;uint64_t last=0;
        CHECK(!ota_service_target(i,&row,&lead,&last));
        if(lead){found=true;CHECK(!memcmp(&row,&local,sizeof(row)));}
    }
    CHECK(found);return 0;
}

static int nominal_test(){
    reset_all();initialize_runtime();CHECK(ota_service_healthy()&&!fake_inhibited);
    CHECK(!ota_service_set_role(true));discover_all();CHECK(discovery_done&&inventory_count==3);
    ota_manifest m=make_manifest();CHECK(!ota_service_stage_begin(&m));
    ota_observation queued{};ota_service_diagnostics queued_diag{};ota_service_snapshot(&queued,&queued_diag);
    CHECK(!strncmp(queued_diag.phase,"ERASE_BEGIN",12)&&queued_diag.busy&&queued.status.campaign_id==42);
    worker_iteration();CHECK(staging&&stage_state==OTA_READY);
    unsigned idle_reads=fake_identity_reads;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==idle_reads&&ota_service_busy());
    uint8_t data[256]={};for(uint32_t pos=0;pos<768;pos+=256)CHECK(!ota_service_stage_data(pos,data,256));
    CHECK(!ota_service_stage_end());pump();CHECK(staged&&fake_prepares==1&&fake_flushes==1);
    CHECK(event_order[OTA_EVENT_ERASE_BEGIN]<event_order[OTA_EVENT_ERASE_END]);
    CHECK(event_order[OTA_EVENT_USB_STAGE_BEGIN]<event_order[OTA_EVENT_USB_STAGE_END]);
    CHECK(event_order[OTA_EVENT_FLASH_COMPLETE]<event_order[OTA_EVENT_VERIFY_BEGIN]);
    CHECK(event_order[OTA_EVENT_VERIFY_BEGIN]<event_order[OTA_EVENT_VERIFY_END]);
    CHECK(!ota_service_start(42,followers_first,3));
    CHECK(ota_service_start(42,followers_first,3)>=0); /* Retry before first worker execution. */
    pump();CHECK(!staged&&participant.status.campaign_id==0);
    int rc=check_campaign_snapshot(42);if(rc)return rc;
    CHECK(local_snapshot.status.state==OTA_VALID&&local_snapshot.status.validated&&local_snapshot.status.offset==768);
    worker_iteration();CHECK(!service_error);
    CHECK(participant.status.campaign_id==0);rc=check_campaign_snapshot(42);if(rc)return rc;
    for(unsigned i=0;i<500&&coordinator.phase!=OTA_COORD_VALIDATE_BARRIER;i++){
        worker_iteration();CHECK(fake_receive_timeout==K_MSEC(5)&&!service_error);
        rc=check_campaign_snapshot(42);if(rc)return rc;
    }
    CHECK(coordinator.phase==OTA_COORD_VALIDATE_BARRIER&&fake_fleet_count==3&&!fake_reboots);
    worker_iteration();CHECK(fake_receive_timeout==K_MSEC(5)); /* Commit barrier still has a campaign deadline. */
    CHECK(!ota_service_commit(42));worker_iteration();
    for(unsigned i=0;i<100&&!fake_reboots;i++){worker_iteration();CHECK(!service_error);}
    CHECK(fake_reboots==1&&fake_journal.state==OTA_REBOOTING);
    uint8_t reboot_order=fake_journal.event_order[OTA_EVENT_REBOOTING];CHECK(reboot_order>0);
    int abort_rc=ota_service_abort(42);
    if(abort_rc>=0)pump();
    CHECK(participant.status.state==OTA_REBOOTING&&fake_journal.state==OTA_REBOOTING&&fake_reboots==1);
    /* Preserve journal/frozen roster, clear all application RAM as at reset. */
    reset_runtime();restore_boot_storage();initialize_runtime();
    CHECK(ota_service_healthy()&&ota_storage_recovery_required()&&fake_inhibited);
    idle_reads=fake_identity_reads;worker_iteration();CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==idle_reads);
    CHECK(event_order[OTA_EVENT_REBOOTING]==reboot_order&&event_order[OTA_EVENT_POSTBOOT_CHECK]>reboot_order);
    for(unsigned i=0;i<2;i++){fake_peers[i].status.state=OTA_RECOVERY_REQUIRED;fake_peers[i].status.error=0;}
    CHECK(!ota_service_reconcile(42,ids,3,m.mcuboot_image_hash));worker_iteration();
    for(unsigned i=0;i<300&&discovery_active;i++){worker_iteration();CHECK(fake_receive_timeout==K_MSEC(5));}
    CHECK(frozen_count==3&&reconcile_active);worker_iteration();
    CHECK(fake_release_requests==2&&ota_service_busy()); /* Acceptance is not success. */
    worker_iteration();CHECK(!ota_service_busy()&&!fake_inhibited&&participant.status.state==OTA_SUCCEEDED);
    CHECK(coordinator.phase==OTA_COORD_IDLE);
    CHECK(!strncmp(ota_service_phase(),"SUCCESS",8));idle_reads=fake_identity_reads;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==idle_reads&&fake_release_requests==2);
    discover_all(43);CHECK(discovery_done&&discovery_token==43&&!ota_service_busy());
    CHECK(participant.status.state==OTA_SUCCEEDED&&ota_service_healthy()&&!service_error);
    ++m.campaign_id;CHECK(!ota_service_stage_begin(&m));worker_iteration();CHECK(staging&&stage_state==OTA_READY&&fake_prepares==2);
    for(uint32_t pos=0;pos<768;pos+=256)CHECK(!ota_service_stage_data(pos,data,256));
    CHECK(!ota_service_stage_end());pump();CHECK(staged);
    CHECK(!ota_service_start(43,followers_first,3));pump();
    CHECK(!staged&&participant.status.campaign_id==42);rc=check_campaign_snapshot(43);if(rc)return rc;
    CHECK(local_snapshot.status.commit_id==0&&local_snapshot.status.pass_id==0&&local_snapshot.status.validated);
    for(unsigned i=0;i<500&&coordinator.phase!=OTA_COORD_VALIDATE_BARRIER;i++){
        worker_iteration();CHECK(!service_error);rc=check_campaign_snapshot(43);if(rc)return rc;
    }
    CHECK(coordinator.phase==OTA_COORD_VALIDATE_BARRIER&&participant.status.campaign_id==43);
    return 0;
}
static int stage_abort_snapshot_test(){
    for(unsigned start=0;start<2;start++){
        reset_all();initialize_runtime();CHECK(!ota_service_set_role(true));discover_all();
        ota_manifest m=make_manifest();CHECK(!ota_service_stage_begin(&m));pump();
        uint8_t data[256]={};for(uint32_t pos=0;pos<768;pos+=256)CHECK(!ota_service_stage_data(pos,data,256));
        CHECK(!ota_service_stage_end());pump();
        if(start){CHECK(!ota_service_start(42,followers_first,3));pump();}
        CHECK(participant.status.campaign_id==0);
        CHECK(!ota_service_abort(42));pump();
        int rc=check_campaign_snapshot(42);if(rc)return rc;
        CHECK(local_snapshot.status.state==OTA_ABORTED&&local_snapshot.status.error);
        CHECK(!local_snapshot.status.validated&&fake_maintenance&&fake_inhibited);
        if(!start)CHECK(fake_journal.state==OTA_ABORTED);
    }
    return 0;
}
static int fresh_discovery_test(){
    reset_all();initialize_runtime();CHECK(!ota_service_set_role(true));discover_all();
    CHECK(discovery_done&&!ota_service_busy());
    uint8_t old_hash=inventory[1].observation.active_mcuboot_image_hash[0];
    uint64_t old_seen=inventory[1].time;
    /* Address claims update routing immediately, but cached application status
     * must be reread after the peer is replaced/reinitialized. */
    fake_peers[0].identity.address=14;fake_peers[0].active_mcuboot_image_hash[0]^=1;
    fake_peers[0].status.campaign_id=99;
    ota_runtime_claim(fake_peers[0].identity.eui,14);
    CHECK(inventory[1].observation.identity.address==14&&inventory[1].observation.active_mcuboot_image_hash[0]==old_hash);
    CHECK(!ota_service_discover(0)&&!k_msgq_num_used_get(&ota_queue)&&inventory[1].time==old_seen);
    const uint64_t token=(1ULL<<40)|42;
    CHECK(!ota_service_discover(token));CHECK(ota_service_busy()&&discovery_reserved);
    CHECK(!view().discovery_done&&!strncmp(ota_service_phase(),"DISCOVERING",12));
    CHECK(!ota_service_discover(token)&&!ota_service_discover(0)&&k_msgq_num_used_get(&ota_queue)==1);
    CHECK(ota_service_discover(token+1)==OTA_ERR_STATE);
    ota_manifest m=make_manifest();CHECK(ota_service_stage_begin(&m)==OTA_ERR_STATE);
    CHECK(ota_service_set_role(false)==OTA_ERR_STATE);
    CHECK(ota_service_reconcile(42,ids,3,m.mcuboot_image_hash)==OTA_ERR_STATE);
    worker_iteration();CHECK(discovery_active&&probe_address==2);
    CHECK(!ota_service_discover(token)&&k_msgq_num_used_get(&ota_queue)==0&&probe_address==2);
    for(unsigned i=0;i<300&&discovery_active;i++)worker_iteration();
    CHECK(discovery_done&&!ota_service_busy()&&!discovery_reserved&&!service_error);
    bool found=false;
    for(unsigned i=0;i<inventory_count;i++)if(!memcmp(inventory[i].observation.identity.eui,ids[1],8)){
        found=true;CHECK(inventory[i].observation.identity.address==14&&inventory[i].time>old_seen);
        CHECK(inventory[i].observation.active_mcuboot_image_hash[0]!=old_hash&&inventory[i].observation.status.campaign_id==99);
    }
    CHECK(found);uint64_t deadline=discovery_deadline;
    CHECK(!ota_service_discover(token)&&!ota_service_discover(0)&&!k_msgq_num_used_get(&ota_queue));
    CHECK(discovery_deadline==deadline&&!discovery_active);
    CHECK(!ota_service_discover(token+1));worker_iteration();
    CHECK(discovery_active&&discovery_deadline>deadline);
    for(unsigned i=0;i<300&&discovery_active;i++)worker_iteration();
    CHECK(!ota_service_busy());
    CHECK(!ota_service_stage_begin(&m));
    CHECK(ota_service_discover(token+1)==OTA_ERR_STATE&&ota_service_discover(0)==OTA_ERR_STATE);
    worker_iteration();CHECK(staging&&ota_service_discover(token+2)==OTA_ERR_STATE);

    reset_all();initialize_runtime();CHECK(!ota_service_set_role(true));discover_all();
    Work refresh{};refresh.type=REFRESH;
    for(unsigned i=0;i<CONFIG_OWNTECH_OTA_QUEUE_DEPTH;i++)CHECK(!enqueue(refresh));
    CHECK(ota_service_discover(token)==OTA_ERR_QUEUE_FULL);
    CHECK(!discovery_reserved&&!ota_service_busy()&&discovery_token==0&&view().discovery_done);
    while(k_msgq_num_used_get(&ota_queue))worker_iteration();
    CHECK(!ota_service_discover(token));worker_iteration();CHECK(discovery_active);
    fake_now=discovery_deadline;worker_iteration();
    CHECK(!ota_service_busy()&&!discovery_reserved&&!discovery_active&&service_error==OTA_ERR_TIMEOUT);
    reset_all();initialize_runtime();CHECK(!ota_service_set_role(true));
    CHECK(!ota_service_reconcile(42,ids,3,m.mcuboot_image_hash));
    CHECK(ota_service_discover(token)==OTA_ERR_STATE&&ota_service_discover(0)==OTA_ERR_STATE);
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
static int standalone_boot_test(){
    reset_all();fake_confirmed=false;fake_can.ready=0;fake_can.driver_started=0;
    fake_sleep_event=[](){fake_can.driver_started=1;can_changed();};
    initialize_runtime();
    CHECK(fake_now==20&&fake_confirms==1&&fake_confirmed&&!fake_inhibited&&!service_error);
    CHECK(ota_service_local_healthy()&&!ota_service_healthy()&&!ota_service_can_ready());
    CHECK(!fake_network_inits&&!strncmp(ota_service_phase(),"WAITING_CAN",12));
    ota_observation observation{};ota_service_diagnostics diag{};ota_service_snapshot(&observation,&diag);
    CHECK(!memcmp(observation.active_mcuboot_image_hash,fake_active_hash,32)&&observation.confirmed);
    CHECK(!strncmp(diag.phase,"WAITING_CAN",12)&&diag.local_healthy&&!diag.healthy&&!diag.can_ready&&!diag.busy);
    CHECK(!ota_service_set_role(true));ota_service_snapshot(&observation,&diag);CHECK(diag.is_lead);
    CHECK(ota_service_discover(0)==OTA_ERR_STATE);
    ota_manifest manifest=make_manifest();CHECK(ota_service_stage_begin(&manifest)==OTA_ERR_STATE);
    ota_command command{};command.type=OTA_CMD_PREPARE;command.manifest=manifest;command.lead_address=2;
    memcpy(command.lead_eui,ids[1],8);CHECK(ota_runtime_command(&command,2)==OTA_ERR_STATE);
    CHECK(!fake_prepares&&!k_msgq_num_used_get(&ota_queue));
    unsigned reads=fake_identity_reads;fake_now=100000;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads&&fake_confirms==1&&!service_error);
    /* Readiness arriving exactly as the worker sleeps must wake it once. */
    fake_wait_event=[](){fake_can.ready=1;can_changed();can_changed();};worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&ota_service_healthy()&&ota_service_can_ready());
    CHECK(fake_network_inits==1&&fake_confirms==1&&!k_msgq_num_used_get(&ota_queue));
    ota_service_snapshot(&observation,&diag);
    CHECK(!strncmp(diag.phase,"IDLE",5)&&diag.local_healthy&&diag.healthy&&diag.can_ready&&!diag.error);
    CHECK(observation.healthy&&observation.confirmed&&!fake_inhibited);
    CHECK(!ota_service_discover(0));worker_iteration();CHECK(discovery_active);
    /* Full queues already wake the worker; no readiness notification is lost. */
    reset_all();fake_confirmed=false;fake_can.ready=0;initialize_runtime();
    Work refresh{};refresh.type=REFRESH;
    for(unsigned i=0;i<CONFIG_OWNTECH_OTA_QUEUE_DEPTH;i++)CHECK(!enqueue(refresh));
    fake_can.ready=1;can_changed();CHECK(!atomic_get(&refresh_pending));worker_iteration();
    CHECK(ota_service_healthy()&&fake_network_inits==1&&fake_confirms==1);
    while(k_msgq_num_used_get(&ota_queue))worker_iteration();
    reads=fake_identity_reads;worker_iteration();CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads);
    return 0;
}
static int standalone_failure_test(){
    for(unsigned failure=0;failure<6;failure++) {
        reset_all();fake_confirmed=false;fake_can.ready=0;
        if(failure==0){fake_can.driver_started=0;fake_can.init_error=-5;}
        if(failure==1)fake_can.driver_started=0; /* bounded local driver timeout */
        if(failure==2)fake_health_result=OTA_ERR_HEALTH;
        if(failure==3)fake_hash_result=OTA_ERR_STORAGE;
        if(failure==4)fake_journal_result=OTA_ERR_JOURNAL;
        if(failure==5){fake_can.ready=1;fake_network_result=OTA_ERR_TRANSPORT;}
        initialize_runtime();
        CHECK(!fake_confirms&&!ota_service_healthy()&&fake_inhibited&&service_error);
        CHECK(!strncmp(ota_service_phase(),"FAILED",7)&&ota_service_set_role(true)==OTA_ERR_STATE);
        CHECK(!waiting_can);
        if(failure==1)CHECK(fake_now==CONFIG_OWNTECH_OTA_HEALTH_TIMEOUT_MS);
        if(failure!=3)CHECK(!memcmp(local_snapshot.active_mcuboot_image_hash,fake_active_hash,32));
        if(failure==0||failure==1)CHECK(!ota_service_local_healthy()&&service_error==OTA_ERR_TRANSPORT);
    }
    reset_all();fake_confirmed=false;fake_can.ready=0;initialize_runtime();
    fake_can.init_error=-5;can_changed();worker_iteration();
    CHECK(!ota_service_local_healthy()&&!ota_service_healthy()&&fake_inhibited&&service_error==OTA_ERR_TRANSPORT);
    CHECK(!waiting_can&&fake_confirms==1&&ota_service_set_role(true)==OTA_ERR_STATE);
    return 0;
}
static int campaign_boot_health_test(){
    reset_all();auto manifest=make_manifest();local_journal(nullptr,&manifest,OTA_REBOOTING,77);
    restore_boot_storage();fake_confirmed=false;fake_can.ready=0;initialize_runtime();
    CHECK(fake_now==CONFIG_OWNTECH_OTA_HEALTH_TIMEOUT_MS&&service_error==OTA_ERR_TRANSPORT);
    CHECK(!fake_confirms&&!fake_confirmed&&!ota_service_healthy()&&fake_inhibited&&!waiting_can);
    CHECK(ota_service_local_healthy()&&!memcmp(local_snapshot.active_mcuboot_image_hash,fake_active_hash,32));
    fake_can.ready=1;can_changed();worker_iteration();
    CHECK(!fake_confirms&&!ota_service_healthy()&&fake_inhibited&&!strncmp(ota_service_phase(),"FAILED",7));
    CHECK(!fake_network_inits&&!(event_mask&(1U<<OTA_EVENT_POSTBOOT_CHECK)));
    reset_all();local_journal(nullptr,&manifest,OTA_REBOOTING,77);restore_boot_storage();fake_confirmed=false;
    initialize_runtime();CHECK(fake_confirms==1&&ota_service_healthy()&&fake_inhibited&&fake_maintenance);
    CHECK(event_mask&(1U<<OTA_EVENT_POSTBOOT_CHECK));
    CHECK(ota_service_set_role(true)==OTA_ERR_STATE);
    reset_all();local_journal(nullptr,&manifest,OTA_REBOOTING,77);restore_boot_storage();fake_confirmed=true;
    fake_journal.mcuboot_image_hash[0]^=1;initialize_runtime();
    CHECK(!ota_service_healthy()&&local_snapshot.rolled_back&&fake_inhibited&&service_error==OTA_ERR_HEALTH);
    reset_all();fake_maintenance=fake_recovery=true;fake_confirmed=false;initialize_runtime();
    CHECK(!fake_confirms&&!ota_service_healthy()&&fake_inhibited&&service_error==OTA_ERR_JOURNAL);
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
    ota_observation queued{};ota_service_diagnostics queued_diag{};ota_service_snapshot(&queued,&queued_diag);
    CHECK(!strncmp(queued_diag.phase,"POSTBOOT_CHECK",15)&&queued_diag.busy);
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
    fake_now=reconcile_deadline+1;worker_iteration();CHECK(!reconcile_active&&service_error==OTA_ERR_TIMEOUT);
    CHECK(fake_receive_timeout==K_MSEC(5)&&!strncmp(ota_service_phase(),"PARTIAL",8));
    unsigned reads=fake_identity_reads;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads&&fake_inhibited);
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
static int idle_worker_test(){
    reset_all();initialize_runtime();unsigned reads=fake_identity_reads;
    fake_now=100000;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads&&!ota_service_busy());
    CHECK(!ota_service_set_role(true)&&ota_service_is_lead());
    /* Discovery arrives after the worker chose an indefinite wait. */
    fake_wait_event=[](){
        fake_event_result=ota_service_discover(0);ota_observation queued{};ota_service_diagnostics diag{};
        ota_service_snapshot(&queued,&diag);
        if(strncmp(diag.phase,"DISCOVERING",12))fake_event_result=OTA_ERR_STATE;
    };worker_iteration();
    CHECK(!fake_event_result&&fake_receive_timeout==K_FOREVER&&discovery_active&&probe_address==2);
    for(unsigned i=0;i<300&&discovery_active;i++)worker_iteration();
    CHECK(discovery_done&&inventory_count==3&&!strncmp(ota_service_phase(),"IDLE",5));
    reads=fake_identity_reads;worker_iteration();CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads);
    /* A changed local claim wakes and coalesces, refreshing both local and
     * inventory snapshots. An unchanged claim creates no periodic work. */
    fake_can.node_addr=10;ota_runtime_claim(eui64,10);ota_runtime_claim(eui64,10);
    CHECK(k_msgq_num_used_get(&ota_queue)==1);worker_iteration();
    ota_observation local{};ota_service_local(&local);CHECK(local.identity.address==10);
    bool lead=false;uint64_t last=0;CHECK(!ota_service_target(0,&local,&lead,&last)&&lead&&local.identity.address==10);
    ota_runtime_claim(eui64,10);CHECK(!k_msgq_num_used_get(&ota_queue));
    /* If refresh cannot fit, existing work still publishes the new address. */
    Work refresh{};refresh.type=REFRESH;
    for(unsigned i=0;i<CONFIG_OWNTECH_OTA_QUEUE_DEPTH;i++)CHECK(!enqueue(refresh));
    fake_can.node_addr=11;ota_runtime_claim(eui64,11);CHECK(!atomic_get(&refresh_pending));
    worker_iteration();ota_service_local(&local);CHECK(local.identity.address==11);
    while(k_msgq_num_used_get(&ota_queue))worker_iteration();
    reads=fake_identity_reads;worker_iteration();CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads);
    return 0;
}
static int participant_worker_wake_test(){
    reset_all();initialize_runtime();ota_runtime_claim(ids[1],2);
    /* Remote PREPARE is admitted while the idle queue receive is blocked. */
    fake_wait_event=[](){
        ota_command cmd{};cmd.type=OTA_CMD_PREPARE;cmd.manifest=make_manifest();
        memcpy(cmd.lead_eui,ids[1],8);cmd.lead_address=2;fake_event_result=ota_runtime_command(&cmd,2);
    };
    worker_iteration();CHECK(!fake_event_result&&fake_receive_timeout==K_FOREVER);
    CHECK(ota_service_busy()&&participant.status.state==OTA_READY&&fake_prepares==1);
    unsigned reads=fake_identity_reads;worker_iteration();CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads);
    fake_wait_event=[](){
        ota_command cmd{};cmd.type=OTA_CMD_BEGIN_PASS;cmd.manifest=make_manifest();cmd.pass_id=1;
        memcpy(cmd.lead_eui,ids[1],8);cmd.lead_address=2;fake_event_result=ota_runtime_command(&cmd,2);
    };
    worker_iteration();CHECK(!fake_event_result&&participant.status.state==OTA_PASS_OPEN);
    CHECK(fake_receive_timeout==K_FOREVER);reads=fake_identity_reads;worker_iteration();
    CHECK(fake_receive_timeout==K_FOREVER&&fake_identity_reads==reads);
    return 0;
}
extern "C" int ota_runtime_state_test_run(){
    int rc=idle_worker_test();if(rc)return rc;
    rc=participant_worker_wake_test();if(rc)return rc;
    rc=bootstrap_test();if(rc)return rc;
    rc=standalone_boot_test();if(rc)return rc;
    rc=standalone_failure_test();if(rc)return rc;
    rc=campaign_boot_health_test();if(rc)return rc;
    rc=nominal_test();if(rc)return rc;rc=stage_abort_snapshot_test();if(rc)return rc;
    rc=fresh_discovery_test();if(rc)return rc;
    rc=reconcile_roster_test();if(rc)return rc;
    rc=queued_prepare_test();if(rc)return rc;rc=persisted_lead_participant_test();if(rc)return rc;
    rc=reconcile_failure_test();if(rc)return rc;rc=release_source_test();if(rc)return rc;
    return stale_queue_after_release_test();
}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main(){int rc=ota_runtime_state_test_run();if(rc)fprintf(stderr,"runtime_state_test.cpp:%d\n",rc);return rc?1:0;}
#endif
