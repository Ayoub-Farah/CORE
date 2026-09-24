/* Actual production USB handlers and actual zcbor; only Zephyr/runtime boundaries
 * are mocked. The exported byte bridge substitutes SMP dispatch, not its codec. */
#define CONFIG_OWNTECH_OTA_HARDWARE_ID 0x01020142
#define CONFIG_OWNTECH_OTA_LAYOUT_ID 0x00010001
#define CONFIG_OWNTECH_OTA_BOOTLOADER_ID 0x00010100
#include "../../zephyr/modules/owntech_ota/zephyr/src/ota_usb.cpp"

static ota_manifest stored;
static uint8_t ids[OTA_MAX_TARGETS][8];
static size_t target_count;
static uint32_t accepted;
static unsigned stage_calls, commit_calls;
static bool lead_role, staged, active_target;
static const char *phase;

extern "C" void usb_reset(void)
{
    memset(&stored,0,sizeof(stored));memset(ids,0,sizeof(ids));
    for(unsigned n=0;n<3;n++) for(unsigned i=0;i<8;i++) ids[n][i]=i+1+16*n;
    target_count=3;accepted=stage_calls=commit_calls=0;
    lead_role=true;staged=active_target=false;phase="IDLE";
}
extern "C" unsigned usb_stat(unsigned which)
{ return which==0?stage_calls:which==1?commit_calls:which==2?accepted:which==3?stored.hardware_id:stored.image_content_size; }
extern "C" bool ota_service_busy(void) { return false; }
extern "C" bool ota_service_healthy(void) { return true; }
extern "C" bool ota_service_is_lead(void) { return lead_role; }
extern "C" int ota_service_set_role(bool lead) { lead_role=lead;return 0; }
extern "C" const char *ota_service_phase(void) { return phase; }
extern "C" const char *ota_state_name(ota_state state) { return state==OTA_VALID?"VALIDATED":"IDLE"; }
extern "C" uint64_t ota_service_campaign(void) { return stored.campaign_id; }
extern "C" uint32_t ota_service_pass(void) { return 1; }
extern "C" uint32_t ota_service_stage_offset(void) { return accepted; }
extern "C" size_t ota_service_target_count(void) { return target_count; }
static void observation(ota_observation *o,size_t index)
{
    memset(o,0,sizeof(*o));memcpy(o->identity.eui,ids[index],8);
    o->identity.address=index+1;o->identity.protocol_version=1;
    o->identity.hardware_id=CONFIG_OWNTECH_OTA_HARDWARE_ID;
    o->identity.layout_id=CONFIG_OWNTECH_OTA_LAYOUT_ID;
    o->identity.bootloader_id=CONFIG_OWNTECH_OTA_BOOTLOADER_ID;
    o->identity.usable_slot_size=227328;o->identity.usable_image_size=221184;
    o->identity.active_confirmed=o->identity.slot_available=true;
    o->healthy=o->confirmed=true;
    o->status.state=staged?OTA_VALID:OTA_IDLE;
    o->status.campaign_id=stored.campaign_id;o->status.pass_id=1;
    o->status.offset=accepted;o->status.image_size=stored.image_size;
    o->status.flash_complete=o->status.validated=staged;
    if(stored.campaign_id) {
        o->event_mask=active_target?((1U<<OTA_EVENT_COUNT)-1):7;
        for(unsigned i=0;i<OTA_EVENT_COUNT;i++) {
            o->event_ms[i]=0xFFFFFFFFU-i;o->event_order[i]=i+1;
        }
        if(active_target)o->event_ms[OTA_EVENT_POSTBOOT_CHECK]=5;
    }
    const char *version=active_target?stored.version:"1.0.0+0";
    const char *build=active_target?stored.build_id:"ota-blink-A";
    memcpy(o->active_version,version,strlen(version)+1);memcpy(o->active_build_id,build,strlen(build)+1);
    if(active_target)memcpy(o->active_mcuboot_image_hash,stored.mcuboot_image_hash,32);
}
extern "C" void ota_service_local(ota_observation *o) { observation(o,0); }
extern "C" int ota_service_target(size_t i,ota_observation *o,bool *lead,uint64_t *seen)
{ if(i>=target_count)return OTA_ERR_ARGUMENT;observation(o,i);*lead=i==0;*seen=123456;return 0; }
extern "C" int ota_service_discover(void) { return 0; }
extern "C" int ota_service_stage_begin(const ota_manifest *m)
{
    ++stage_calls;
    if(!m->image_size || !m->image_content_size || m->image_content_size>m->image_size ||
       m->hardware_id!=CONFIG_OWNTECH_OTA_HARDWARE_ID || m->layout_id!=CONFIG_OWNTECH_OTA_LAYOUT_ID ||
       m->bootloader_id!=CONFIG_OWNTECH_OTA_BOOTLOADER_ID)return OTA_ERR_ARGUMENT;
    stored=*m;accepted=0;phase="STAGING";return 0;
}
extern "C" int ota_service_stage_data(uint32_t offset,const uint8_t *,size_t n)
{ if(offset!=accepted || n>stored.image_size-accepted)return OTA_ERR_OFFSET;accepted+=n;return 0; }
extern "C" int ota_service_stage_end(void)
{ if(accepted!=stored.image_size)return OTA_ERR_INCOMPLETE;staged=true;phase="STAGED";return 0; }
extern "C" int ota_service_start(uint64_t campaign,const uint8_t targets[][8],size_t n)
{
    if(!staged || campaign!=stored.campaign_id || !n)return OTA_ERR_STATE;
    memcpy(ids,targets,n*8);target_count=n;phase="ALL_VALIDATED";return 0;
}
extern "C" int ota_service_commit(uint64_t campaign)
{ if(campaign!=stored.campaign_id)return OTA_ERR_STATE;++commit_calls;active_target=true;phase="REBOOTING";return 0; }
extern "C" int ota_service_abort(uint64_t) { phase="ABORTED";return 0; }
extern "C" int ota_service_reconcile(uint64_t campaign,const uint8_t targets[][8],size_t n,const uint8_t hash[32])
{
    if(campaign!=stored.campaign_id || !n || memcmp(hash,stored.mcuboot_image_hash,32))return OTA_ERR_STATE;
    memcpy(ids,targets,n*8);target_count=n;phase="SUCCESS";return 0;
}

/* Input/output are complete network-order SMP packets. Negative returns are
 * harness errors. Production handler errors become standard CBOR rc replies. */
extern "C" int usb_smp_request(const uint8_t *input,size_t length,uint8_t *output,size_t capacity)
{
    if(length<8 || capacity<16 || length-8!=((size_t)input[2]<<8|input[3]))return -1;
    uint16_t group_id=(uint16_t)input[4]<<8|input[5];uint8_t command=input[7];
    mgmt_evt_op_cmd_arg argument{group_id,command,input[0]};
    bool stop=false;int32_t error_code=0;uint16_t error_group=0;
    mgmt_cb_return guarded=guard(MGMT_EVT_OP_CMD_RECV,MGMT_CB_OK,&error_code,&error_group,&stop,&argument,sizeof(argument));
    cbor_nb_reader reader{};cbor_nb_writer writer{};smp_streamer stream{&reader,&writer};
    zcbor_new_decode_state(reader.zs,ARRAY_SIZE(reader.zs),input+8,length-8,1,nullptr,0);
    zcbor_new_encode_state(writer.zs,ARRAY_SIZE(writer.zs),output+8,capacity-8,0);
    if(!zcbor_map_start_encode(writer.zs,40))return -2;
    int rc=guarded==MGMT_CB_OK?MGMT_ERR_EOK:error_code;
    if(!rc) {
        if(input[0]!=2 || command>=ARRAY_SIZE(handlers))rc=MGMT_ERR_ENOTSUP;
        else rc=handlers[command].mh_write(&stream);
    }
    if(rc) {
        zcbor_new_encode_state(writer.zs,ARRAY_SIZE(writer.zs),output+8,capacity-8,0);
        if(!zcbor_map_start_encode(writer.zs,40) || !zcbor_tstr_put_lit(writer.zs,"rc") ||
           !zcbor_int32_put(writer.zs,rc))return -3;
    }
    if(!zcbor_map_end_encode(writer.zs,40))return -4;
    size_t result=writer.zs[0].payload-(output+8);
    output[0]=input[0]+1;output[1]=0;output[2]=result>>8;output[3]=result;
    output[4]=input[4];output[5]=input[5];output[6]=input[6];output[7]=command;
    return (int)(result+8);
}
