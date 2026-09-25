/* Production receiver worker and participant; only kernel, flash and CAN seams are fake. */
#include "ota_receiver_runtime.cpp"
#include "ota_protocol.h"
#define CHECK(x) do {if(!(x)) return __LINE__;} while(0)
uint8_t eui64[8]={1,2,3,4,5,6,7,8};
static thingset_can_context fake_can{1,1,1,0};
static thingset_can_state_callback_t state_callback;
static void *state_arg;
static uint64_t now;
static bool confirmed,inhibited,maintenance,recovery;
static int health_result,confirms,prepares,appends,arms,last_wait;
static ota_storage_journal journal;
static uint8_t active_hash[32];
extern "C" int strncmp(const char *a,const char *b,size_t n)
{while(n--){if(*a!=*b)return (unsigned char)*a-(unsigned char)*b;if(!*a)return 0;++a;++b;}return 0;}
int64_t k_uptime_get(){return now;}
void k_sleep(int ms){now+=ms;}
void runtime_test_msgq_wait(k_msgq *,int timeout){last_wait=timeout;}
thingset_can_context *thingset_can_get_inst(){return &fake_can;}
void thingset_can_set_state_callback(thingset_can_state_callback_t cb,void *arg)
{state_callback=cb;state_arg=arg;cb(arg);}
bool boot_is_img_confirmed(){return confirmed;}
int boot_write_img_confirmed(){confirmed=true;++confirms;return 0;}
extern "C" int owntech_ota_check_health(){return health_result;}
extern "C" void ota_safety_restore(bool value){inhibited=value;}
extern "C" bool ota_safety_inhibited(){return inhibited;}
extern "C" void ota_feedback_state(ota_state){}
extern "C" int ota_network_init(){return 0;}
int ota_storage_init(){return 0;}
bool ota_storage_recovery_required(){return recovery;}
bool ota_storage_maintenance(){return maintenance;}
int ota_storage_active_hash(uint8_t *hash){memcpy(hash,active_hash,32);return 0;}
int ota_storage_get_journal(ota_storage_journal *j){*j=journal;return 0;}
void ota_storage_set_events(uint32_t,const uint32_t *,const uint8_t *){}
void ota_storage_boot_identity(ota_identity *id)
{
    id->protocol_version=OTA_PROTOCOL_VERSION;id->image_class=OTA_IMAGE_RECEIVER;
    id->hardware_id=1;id->layout_id=2;id->bootloader_id=3;
    id->usable_slot_size=4096;id->usable_image_size=3072;
    id->active_confirmed=confirmed;id->slot_available=!maintenance;
}
int ota_storage_expect_lead(const uint8_t *id){memcpy(journal.lead_eui,id,8);return 0;}
int ota_storage_release_maintenance(const uint8_t *hash)
{if(memcmp(hash,active_hash,32)||!confirmed)return OTA_ERR_HEALTH;maintenance=recovery=inhibited=false;return 0;}
static int prepare(void *,const ota_manifest *,bool){++prepares;maintenance=inhibited=true;return 0;}
static int append(void *,uint32_t,const uint8_t *,size_t){++appends;return 0;}
static int flush(void *){return 0;}
static int validate(void *,const ota_manifest *){return 0;}
static int persist(void *,const ota_manifest *m,ota_state state,uint32_t commit)
{
    journal.campaign_id=m->campaign_id;journal.commit_id=commit;journal.image_size=m->image_size;journal.state=state;
    memcpy(journal.version,m->version,32);memcpy(journal.build_id,m->build_id,32);
    memcpy(journal.mcuboot_image_hash,m->mcuboot_image_hash,32);return 0;
}
static int arm(void *,const ota_manifest *,uint32_t){++arms;return 0;}
static int reboot(void *,uint32_t,uint32_t){return 0;}
static void close(void *){}
void ota_storage_hooks(ota_participant_hooks *h)
{*h={nullptr,prepare,append,flush,validate,persist,reboot,close,arm};}
static void reset()
{
    initialized=busy=healthy=local_healthy=can_ready=losses=0;
    identity_conflict=refresh_pending=release_pending=0;
    participant={};storage_hooks={};local_snapshot={};diagnostics_snapshot={"BOOT",0,false,false,false,false,false};
    memset(lead_eui,0,8);lead_address=0;lead_bound=lead_seen=waiting_can=false;
    accepted_campaign=0;accepted_manifest={};service_error=0;event_mask=next_event_order=0;
    memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
    ota_queue.head=ota_queue.count=0;
    now=0;fake_can={1,1,1,0};confirmed=true;inhibited=true;maintenance=recovery=false;
    health_result=confirms=prepares=appends=arms=last_wait=0;journal={};memset(active_hash,0x55,32);
}
static ota_command command()
{
    ota_command cmd{};cmd.type=OTA_CMD_PREPARE;cmd.lead_address=2;memset(cmd.lead_eui,0xaa,8);
    auto &m=cmd.manifest;m.campaign_id=77;m.image_size=m.image_content_size=256;
    m.protocol_version=OTA_PROTOCOL_VERSION;m.image_class=OTA_IMAGE_RECEIVER;
    m.hardware_id=1;m.layout_id=2;m.bootloader_id=3;
    memcpy(m.version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
    memcpy(m.build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));
    memcpy(m.mcuboot_image_hash,active_hash,32);return cmd;
}
static int test_receiver()
{
    reset();fake_can.ready=0;confirmed=false;initialize_runtime();
    CHECK(confirms==1 && ota_service_local_healthy() && !ota_service_healthy());
    CHECK(!inhibited && !memcmp(ota_service_phase(),"WAITING_CAN",11));
    worker_iteration();CHECK(last_wait==K_FOREVER && now==0);
    fake_can.ready=1;state_callback(state_arg);worker_iteration();CHECK(ota_service_healthy());
    reset();health_result=OTA_ERR_HEALTH;confirmed=false;initialize_runtime();
    CHECK(!confirms && inhibited && !ota_service_local_healthy());
    reset();initialize_runtime();auto cmd=command();
    CHECK(ota_runtime_command(&cmd,2)==OTA_AGAIN); /* No evidence of Lead claim. */
    ota_runtime_claim(cmd.lead_eui,2);
    for(unsigned i=0;i<300;i++){uint8_t unrelated[8]{};unrelated[0]=9;unrelated[7]=i;ota_runtime_claim(unrelated,3);}
    CHECK(ota_runtime_command(&cmd,2)==OTA_AGAIN); /* Single claim cache, no fleet limit. */
    ota_runtime_claim(cmd.lead_eui,2);
    CHECK(!ota_runtime_command(&cmd,2));worker_iteration();CHECK(prepares==1 && participant.status.state==OTA_READY);
    CHECK(!ota_runtime_command(&cmd,2));worker_iteration();CHECK(prepares==1); /* Idempotent PREPARE. */
    auto contradicted=cmd;contradicted.manifest.artifact_sha256[0]^=1;
    CHECK(ota_runtime_command(&contradicted,2)==OTA_ERR_CONFLICT && !ota_queue.count);
    uint8_t other[8]{};other[0]=9;ota_runtime_claim(other,3);
    CHECK(!identity_conflict); /* Arbitrary neighbors consume no receiver memory. */
    auto wrong=cmd;wrong.manifest.campaign_id++;CHECK(ota_runtime_command(&wrong,2)==OTA_ERR_CONFLICT);
    cmd.type=OTA_CMD_BEGIN_PASS;cmd.pass_id=1;
    CHECK(!ota_runtime_command(&cmd,2));worker_iteration();CHECK(participant.status.state==OTA_PASS_OPEN);
    uint8_t payload[32]{},packet[OTA_MAX_REPORT_SIZE];size_t length;
    ota_report r{OTA_REPORT_DATA,77,1,0,sizeof(payload),payload};
    CHECK(!ota_report_encode(&r,packet,sizeof(packet),&length));
    ota_runtime_report(packet,length,3);CHECK(!ota_queue.count);
    for(size_t i=0;i<ota_queue.capacity+1;i++)ota_runtime_report(packet,length,2);
    CHECK(losses==1);cmd.type=OTA_CMD_END_PASS;
    CHECK(ota_runtime_command(&cmd,2)==OTA_ERR_QUEUE_FULL);
    while(ota_queue.count)worker_iteration();
    CHECK(appends==1 && participant.status.offset==32 && participant.status.rx_dropped==1);
    CHECK(!ota_runtime_command(&cmd,2));worker_iteration();CHECK(participant.status.state==OTA_PASS_CLOSED);
    worker_iteration();CHECK(last_wait==K_FOREVER); /* Busy does not imply polling. */
    ota_runtime_claim(other,2);worker_iteration();
    CHECK(ota_service_error()==OTA_ERR_IDENTITY && inhibited);
    CHECK(ota_runtime_command(&cmd,2)==OTA_ERR_IDENTITY);

    /* Interrupted arm before COMMITTED can confirm only the expected running image. */
    reset();cmd=command();persist(nullptr,&cmd.manifest,OTA_COMMIT_INTENT,42);
    memcpy(journal.lead_eui,cmd.lead_eui,8);maintenance=recovery=true;confirmed=false;
    initialize_runtime();CHECK(confirms==1 && inhibited && !ota_service_error());
    reset();cmd=command();persist(nullptr,&cmd.manifest,OTA_COMMIT_INTENT,42);
    journal.mcuboot_image_hash[0]^=1;maintenance=recovery=true;confirmed=false;
    initialize_runtime();CHECK(!confirms && inhibited && ota_service_error()==OTA_ERR_HEALTH);

    /* Committed image can confirm locally, retaining collective maintenance. */
    reset();cmd=command();persist(nullptr,&cmd.manifest,OTA_COMMITTED,42);
    memcpy(journal.lead_eui,cmd.lead_eui,8);maintenance=recovery=true;confirmed=false;
    fake_can.ready=0;initialize_runtime();CHECK(confirms==1 && inhibited && !ota_service_healthy());
    fake_can.ready=1;state_callback(state_arg);worker_iteration();
    ota_runtime_claim(cmd.lead_eui,2);
    CHECK(ota_runtime_release(eui64,active_hash,76,2)==OTA_ERR_HEALTH);
    CHECK(!ota_runtime_release(eui64,active_hash,77,2));worker_iteration();
    CHECK(!inhibited && participant.status.state==OTA_SUCCEEDED);
    CHECK(!ota_runtime_release(eui64,active_hash,77,2)); /* Completed release is repeatable. */
    uint8_t next_lead[8]={0x33};ota_runtime_claim(next_lead,3);
    cmd=command();cmd.manifest.campaign_id=78;cmd.lead_address=3;memcpy(cmd.lead_eui,next_lead,8);
    CHECK(!ota_runtime_command(&cmd,3));worker_iteration();CHECK(prepares==1);
    return 0;
}
#ifdef OWNTECH_FREESTANDING_TEST
extern "C" __declspec(dllexport) int ota_receiver_runtime_test_run(){return test_receiver();}
#else
int main(){return test_receiver();}
#endif
