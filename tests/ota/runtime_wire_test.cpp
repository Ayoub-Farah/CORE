/* Execute both sides of the production ThingSet OTA adapter wire codec. */
#include "sdk_shim/sdk_host.h"
#undef CONFIG_THINGSET_CAN_MULTIPLE_INSTANCES
#define K_SEM_DEFINE(n,v,m) struct k_sem n={(v),(m)}
#define K_MUTEX_DEFINE(n) struct k_mutex n={false}
#define THINGSET_DEFINE_BYTES(n,b,l) struct host_bytes n={(b),(l)}
#define THINGSET_ADD_GROUP(...)
#define THINGSET_ADD_FN_INT32(...)
#define THINGSET_ADD_ITEM_BYTES(...)
#define THINGSET_BIN_EXEC 0x02
#define THINGSET_BIN_GET 0x01
#define THINGSET_STATUS_CHANGED 0x84
#define THINGSET_STATUS_CONTENT 0x85
enum thingset_callback_reason {THINGSET_CALLBACK_PRE_READ,THINGSET_CALLBACK_POST_READ};
struct host_bytes {uint8_t *bytes;size_t num_bytes;};
uint8_t eui64[8]={1,2,3,4,5,6,7,8};
#include "../../zephyr/modules/owntech_ota/zephyr/src/ota_thingset.cpp"

static ota_observation observation;
static ota_command captured_command;
static uint8_t actual_source=2, actual_route=0;
static bool request_scope=true;
static int command_result, release_calls, claim_announcements;
static uint8_t packet_copy[256];
static size_t packet_length;
extern "C" int ota_runtime_command(const ota_command *cmd,uint8_t source)
{captured_command=*cmd;assert(source==2);return command_result;}
extern "C" int ota_runtime_release(const uint8_t id[8],const uint8_t hash[32],uint8_t source)
{assert(!memcmp(id,eui64,8));assert(!memcmp(hash,observation.active_mcuboot_image_hash,32));assert(source==2);release_calls++;return 0;}
extern "C" void ota_service_local(ota_observation *o){*o=observation;}
extern "C" void ota_runtime_claim(const uint8_t id[8],uint8_t address){}
extern "C" void ota_runtime_report(const uint8_t *data,size_t length,uint8_t address){}
extern "C" int thingset_can_get_request_source(uint8_t *source,uint8_t *route)
{if(!request_scope)return -ENOENT;*source=actual_source;*route=actual_route;return 0;}
extern "C" void thingset_can_set_addr_claim_rx_callback(thingset_can_addr_claim_rx_callback_t cb){}
extern "C" int thingset_can_set_report_rx_callback(thingset_can_report_rx_callback_t cb){return 0;}
extern "C" int thingset_can_announce_address(k_timeout_t timeout){claim_announcements++;return 0;}
extern "C" int thingset_can_send(uint8_t *packet,size_t n,uint8_t address,uint8_t route,
    thingset_can_reqresp_callback_t callback,void *arg,k_timeout_t timeout)
{
    assert(n<=sizeof(packet_copy));memcpy(packet_copy,packet,n);packet_length=n;
    uint8_t reply[256]={THINGSET_STATUS_CHANGED,0xF6,0};size_t size=3;
    assert(packet[1]==0x19);
    uint16_t id=(uint16_t)packet[2]<<8|packet[3];
    if(packet[0]==THINGSET_BIN_GET){
        assert(id==STATUS&&n==4);status_callback(THINGSET_CALLBACK_PRE_READ);
        assert(status_value.num_bytes==STATUS_LENGTH);
        reply[0]=THINGSET_STATUS_CONTENT;reply[2]=0x58;reply[3]=STATUS_LENGTH;
        memcpy(reply+4,status_buffer,STATUS_LENGTH);size=STATUS_LENGTH+4;
    }else{
        assert(packet[0]==THINGSET_BIN_EXEC&&packet[4]==0x81&&packet[5]==0x58);
        int rc;
        if(id==COMMAND){assert(n==7+COMMAND_LENGTH);memcpy(command_buffer,packet+7,COMMAND_LENGTH);
            command_value.num_bytes=packet[6];rc=receive_command();}
        else {assert(id==RELEASE&&n==47);memcpy(release_buffer,packet+7,40);release_value.num_bytes=40;rc=release_maintenance();}
        assert(rc>=-24&&rc<24);reply[2]=rc<0?0x20-rc-1:rc;
    }
    callback(reply,size,0,0,address,arg);return 0;
}
int main()
{
    ota_target target{};memcpy(target.identity.eui,eui64,8);target.identity.address=2;
    ota_command cmd{};cmd.type=OTA_CMD_PREPARE;cmd.manifest.campaign_id=UINT64_C(0x1234567890ABCDEF);
    cmd.manifest.image_size=227328;cmd.manifest.image_content_size=98127;
    cmd.manifest.hardware_id=42;cmd.manifest.layout_id=17;cmd.manifest.bootloader_id=99;
    cmd.manifest.protocol_version=1;memcpy(cmd.manifest.version,"1.2.3",6);memcpy(cmd.manifest.build_id,"build-A",8);
    memset(cmd.manifest.artifact_sha256,0xAB,32);memset(cmd.manifest.mcuboot_image_hash,0xCD,32);
    memset(cmd.lead_eui,0x19,8);cmd.lead_address=2;cmd.pass_id=3;cmd.start_offset=512;cmd.commit_id=7;
    assert(ota_network_command(&target,&cmd)==0);
    assert(packet_length==195&&!memcmp(&cmd,&captured_command,sizeof(cmd)));
    request_scope=false;assert(ota_network_command(&target,&cmd)==OTA_ERR_IDENTITY);request_scope=true;
    actual_source=3;assert(ota_network_command(&target,&cmd)==OTA_ERR_IDENTITY);actual_source=2;
    actual_route=1;assert(ota_network_command(&target,&cmd)==OTA_ERR_IDENTITY);actual_route=0;
    command_result=OTA_AGAIN;assert(ota_network_command(&target,&cmd)==OTA_AGAIN);command_result=0;
    command_result=OTA_ERR_QUEUE_FULL;assert(ota_network_command(&target,&cmd)==OTA_ERR_QUEUE_FULL);command_result=0;

    observation.identity=target.identity;observation.identity.protocol_version=1;
    observation.identity.usable_slot_size=227328;observation.identity.usable_image_size=221184;
    observation.identity.hardware_id=42;observation.identity.layout_id=17;observation.identity.bootloader_id=99;
    observation.identity.active_confirmed=true;observation.identity.slot_available=true;
    observation.status.campaign_id=cmd.manifest.campaign_id;observation.status.pass_id=3;
    observation.status.offset=1024;observation.status.image_size=227328;observation.status.commit_id=7;
    observation.status.state=OTA_VALID;observation.status.error=-17;observation.status.queue_depth=4;
    observation.status.flash_complete=true;observation.status.validated=true;
    observation.status.rx_dropped=200;observation.status.rx_rejected=19;
    memset(observation.active_mcuboot_image_hash,0xE5,32);
    memcpy(observation.active_version,"1.2.3",6);memcpy(observation.active_build_id,"build-B",8);
    observation.healthy=true;observation.confirmed=true;
    observation.event_mask=(1U<<OTA_EVENT_ERASE_BEGIN)|(1U<<OTA_EVENT_ERASE_END)|(1U<<OTA_EVENT_POSTBOOT_CHECK);
    observation.event_ms[OTA_EVENT_ERASE_BEGIN]=200;observation.event_order[OTA_EVENT_ERASE_BEGIN]=1;
    observation.event_ms[OTA_EVENT_ERASE_END]=200;observation.event_order[OTA_EVENT_ERASE_END]=2;
    observation.event_ms[OTA_EVENT_POSTBOOT_CHECK]=10;observation.event_order[OTA_EVENT_POSTBOOT_CHECK]=3;
    ota_observation decoded{};assert(ota_network_status(2,&decoded)==0);
    assert(!memcmp(&observation,&decoded,sizeof(observation)));
    assert(ota_network_release(2,eui64,observation.active_mcuboot_image_hash)==0);
    assert(release_calls==1&&claim_announcements==1);
    response_data[0]=THINGSET_STATUS_CHANGED;response_data[1]=0xF6;response_data[2]=0;
    response_length=4;assert(exec_result()==OTA_ERR_TRANSPORT);
    active_request=9;response_length=0;
    uint8_t late[]={0x84,0xF6,0};response(late,3,0,0,2,(void *)8);
    assert(response_length==0);active_request=0;
    puts("OTA ThingSet wire: command/status/release round trips and source validation OK");
}
