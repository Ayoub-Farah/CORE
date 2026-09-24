/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <string.h>
#include <limits.h>

/* User group reserved by Core. All operations are SMP WRITE requests. The
 * separate CDC has no console reader and carries only framed SMP traffic. */
static constexpr uint16_t GROUP=64;
struct Request {
    uint64_t campaign=0;
    uint32_t index=UINT32_MAX, offset=UINT32_MAX, protocol=0;
    ota_manifest manifest{};
    zcbor_string data{}, artifact_hash{}, image_hash{}, version{}, build{}, role{};
    uint8_t targets[OTA_MAX_TARGETS][8]{};
    size_t count=0;
};
static int nibble(uint8_t c)
{
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
static bool targets_decode(zcbor_state_t *z,void *out)
{
    auto &r=*static_cast<Request *>(out);
    if(!zcbor_list_start_decode(z)) return false;
    while(!zcbor_array_at_end(z)) {
        zcbor_string id{};if(r.count==OTA_MAX_TARGETS || !zcbor_tstr_decode(z,&id) || id.len!=16) return false;
        for(unsigned i=0;i<8;i++) {
            int hi=nibble(id.value[2*i]),lo=nibble(id.value[2*i+1]);if(hi<0||lo<0) return false;
            r.targets[r.count][i]=(hi<<4)|lo;
        }
        ++r.count;
    }
    return zcbor_list_end_decode(z);
}
static bool decode(smp_streamer *ctxt,Request &r)
{
#define FIELD(key, decoder_fn, destination) \
    {{reinterpret_cast<const uint8_t *>(key), sizeof(key)-1}, \
     reinterpret_cast<zcbor_decoder_t *>(decoder_fn), destination, false}
    zcbor_map_decode_key_val fields[]={
        FIELD("campaign",zcbor_uint64_decode,&r.campaign),
        FIELD("index",zcbor_uint32_decode,&r.index),
        FIELD("offset",zcbor_uint32_decode,&r.offset),
        FIELD("protocol",zcbor_uint32_decode,&r.protocol),
        FIELD("artifact_size",zcbor_uint32_decode,&r.manifest.image_size),
        FIELD("useful_size",zcbor_uint32_decode,&r.manifest.image_content_size),
        FIELD("hardware_id",zcbor_uint32_decode,&r.manifest.hardware_id),
        FIELD("layout_id",zcbor_uint32_decode,&r.manifest.layout_id),
        FIELD("bootloader_id",zcbor_uint32_decode,&r.manifest.bootloader_id),
        FIELD("artifact_sha256",zcbor_bstr_decode,&r.artifact_hash),
        FIELD("mcuboot_image_hash",zcbor_bstr_decode,&r.image_hash),
        FIELD("version",zcbor_tstr_decode,&r.version),
        FIELD("build_id",zcbor_tstr_decode,&r.build),
        FIELD("role",zcbor_tstr_decode,&r.role),
        FIELD("data",zcbor_bstr_decode,&r.data),
        FIELD("targets",targets_decode,&r),
    };
#undef FIELD
    size_t count=0;return zcbor_map_decode_bulk(ctxt->reader->zs,fields,ARRAY_SIZE(fields),&count)==0;
}
static bool text(zcbor_state_t *z,const char *key,const char *value)
{ return zcbor_tstr_encode_ptr(z,key,strlen(key)) && zcbor_tstr_encode_ptr(z,value,strlen(value)); }
static bool number(zcbor_state_t *z,const char *key,uint64_t value)
{ return zcbor_tstr_encode_ptr(z,key,strlen(key)) && zcbor_uint64_put(z,value); }
static bool boolean(zcbor_state_t *z,const char *key,bool value)
{ return zcbor_tstr_encode_ptr(z,key,strlen(key)) && zcbor_bool_put(z,value); }
static bool bytes(zcbor_state_t *z,const char *key,const uint8_t *p,size_t n)
{ return zcbor_tstr_encode_ptr(z,key,strlen(key)) && zcbor_bstr_encode_ptr(z,reinterpret_cast<const char *>(p),n); }
static bool error(zcbor_state_t *z,int rc)
{ return zcbor_tstr_put_lit(z,"rc") && zcbor_int32_put(z,rc<0?rc:0); }
static bool health(zcbor_state_t *z,const ota_service_diagnostics &d)
{
    return boolean(z,"local_healthy",d.local_healthy) &&
        boolean(z,"healthy",d.healthy) && boolean(z,"can_ready",d.can_ready) &&
        zcbor_tstr_put_lit(z,"error") && zcbor_int32_put(z,d.error);
}
static void identity_text(const uint8_t eui[8],char str[17])
{
    constexpr char hex[]="0123456789abcdef";
    for(unsigned i=0;i<8;i++) {str[2*i]=hex[eui[i]>>4];str[2*i+1]=hex[eui[i]&15];}str[16]=0;
}
static bool events(zcbor_state_t *z,const ota_observation &o)
{
    if(!number(z,"event_mask",o.event_mask) || !zcbor_tstr_put_lit(z,"event_ms") ||
       !zcbor_list_start_encode(z,OTA_EVENT_COUNT)) return false;
    for(unsigned i=0;i<OTA_EVENT_COUNT;i++) if(!zcbor_uint32_put(z,o.event_ms[i])) return false;
    if(!zcbor_list_end_encode(z,OTA_EVENT_COUNT) || !zcbor_tstr_put_lit(z,"event_order") ||
       !zcbor_list_start_encode(z,OTA_EVENT_COUNT)) return false;
    for(unsigned i=0;i<OTA_EVENT_COUNT;i++) if(!zcbor_uint32_put(z,o.event_order[i])) return false;
    return zcbor_list_end_encode(z,OTA_EVENT_COUNT);
}
static bool row(zcbor_state_t *z,const ota_observation &o,bool lead,uint64_t seen)
{
    char id[17];identity_text(o.identity.eui,id);
    bool compatible=o.identity.protocol_version==OTA_PROTOCOL_VERSION &&
        o.identity.hardware_id==CONFIG_OWNTECH_OTA_HARDWARE_ID && o.identity.layout_id==CONFIG_OWNTECH_OTA_LAYOUT_ID &&
        o.identity.bootloader_id==CONFIG_OWNTECH_OTA_BOOTLOADER_ID;
    return zcbor_map_start_encode(z,32) && text(z,"identity",id) && number(z,"address",o.identity.address) &&
        text(z,"role",lead?"lead":"follower") && text(z,"state",ota_state_name(o.status.state)) &&
        text(z,"version",o.active_version) && text(z,"build_id",o.active_build_id) &&
        bytes(z,"mcuboot_image_hash",o.active_mcuboot_image_hash,32) &&
        number(z,"offset",o.status.offset) && number(z,"image_size",o.status.image_size) &&
        number(z,"pass",o.status.pass_id) && number(z,"queue_depth",o.status.queue_depth) &&
        number(z,"last_seen_ms",seen) && number(z,"rx_dropped",o.status.rx_dropped) &&
        number(z,"campaign",o.status.campaign_id) && events(z,o) &&
        boolean(z,"flash_complete",o.status.flash_complete) && boolean(z,"validated",o.status.validated) &&
        boolean(z,"healthy",o.healthy) && boolean(z,"confirmed",o.confirmed) &&
        boolean(z,"available",o.identity.active_confirmed&&o.identity.slot_available&&!o.status.error) &&
        boolean(z,"compatible",compatible) &&
        zcbor_tstr_put_lit(z,"error") && zcbor_int32_put(z,o.status.error) && zcbor_map_end_encode(z,32);
}
static bool status(zcbor_state_t *z,uint32_t index)
{
    ota_observation local{};ota_service_diagnostics d{};ota_service_snapshot(&local,&d);
    size_t count=ota_service_target_count();
    if(!text(z,"phase",d.phase) || !text(z,"state",d.phase) || !health(z,d) ||
       !number(z,"campaign",ota_service_campaign()) || !number(z,"pass",ota_service_pass()) ||
       !number(z,"offset",ota_service_stage_offset()) || !number(z,"target_count",count) ||
       !zcbor_tstr_put_lit(z,"targets") || !zcbor_list_start_encode(z,1)) return false;
    if(count) {
        ota_observation o{};bool lead;uint64_t seen;
        if(ota_service_target(index==UINT32_MAX?0:index,&o,&lead,&seen) || !row(z,o,lead,seen)) return false;
    }
    return zcbor_list_end_encode(z,1);
}
static int handle(smp_streamer *ctxt,int command)
{
    Request r;auto z=ctxt->writer->zs;if(!decode(ctxt,r)) return MGMT_ERR_EINVAL;
    int rc=0;bool ok=true;
    switch(command) {
    case 0: {
        ota_observation o{};ota_service_diagnostics d{};ota_service_snapshot(&o,&d);
        char id[17];identity_text(o.identity.eui,id);
        ok=text(z,"service","owntech-ota") && number(z,"protocol",OTA_PROTOCOL_VERSION) &&
            text(z,"identity",id) && text(z,"role",d.is_lead?"lead":"follower") &&
            text(z,"version",o.active_version) && text(z,"build_id",o.active_build_id) &&
            bytes(z,"mcuboot_image_hash",o.active_mcuboot_image_hash,32) &&
            boolean(z,"available",d.healthy&&!d.busy&&o.identity.slot_available) &&
            boolean(z,"active_confirmed",o.identity.active_confirmed) && boolean(z,"slot_available",o.identity.slot_available) &&
            number(z,"slot_size",o.identity.usable_slot_size) && number(z,"useful_capacity",o.identity.usable_image_size) &&
            number(z,"hardware_id",o.identity.hardware_id) && number(z,"layout_id",o.identity.layout_id) &&
            number(z,"bootloader_id",o.identity.bootloader_id) && text(z,"upload","stage_data") &&
            text(z,"phase",d.phase) && health(z,d);break;
    }
    case 1:
        if(r.role.len==4 && !memcmp(r.role.value,"lead",4)) rc=ota_service_set_role(true);
        else if(r.role.len==8 && !memcmp(r.role.value,"follower",8)) rc=ota_service_set_role(false);
        else rc=OTA_ERR_ARGUMENT;
        break;
    case 2:
        rc=ota_service_discover();ok=status(z,r.index);break;
    case 3:
        if(!r.campaign || r.protocol!=OTA_PROTOCOL_VERSION || r.artifact_hash.len!=32 || r.image_hash.len!=32 ||
           !r.version.len || r.version.len>=32 || !r.build.len || r.build.len>=32 ||
           memchr(r.version.value,0,r.version.len) || memchr(r.build.value,0,r.build.len)) {rc=OTA_ERR_ARGUMENT;break;}
        r.manifest.campaign_id=r.campaign;r.manifest.protocol_version=r.protocol;
        memcpy(r.manifest.artifact_sha256,r.artifact_hash.value,32);memcpy(r.manifest.mcuboot_image_hash,r.image_hash.value,32);
        memcpy(r.manifest.version,r.version.value,r.version.len);memcpy(r.manifest.build_id,r.build.value,r.build.len);
        rc=ota_service_stage_begin(&r.manifest);ok=text(z,"state","ACCEPTED");break;
    case 4:
        if(r.offset==UINT32_MAX || !r.data.len || r.data.len>OTA_MAX_PAYLOAD) {rc=OTA_ERR_ARGUMENT;break;}
        rc=ota_service_stage_data(r.offset,r.data.value,r.data.len);
        ok=number(z,"offset",ota_service_stage_offset());break;
    case 5: rc=ota_service_stage_end();ok=text(z,"state","ACCEPTED");break;
    case 6: rc=ota_service_start(r.campaign,r.targets,r.count);ok=text(z,"state","ACCEPTED");break;
    case 7: ok=status(z,r.index);break;
    case 8: rc=ota_service_commit(r.campaign);ok=text(z,"state","ACCEPTED");break;
    case 9: rc=ota_service_abort(r.campaign);ok=text(z,"state","ACCEPTED");break;
    case 10:
        if(!r.count || r.image_hash.len!=32) {rc=OTA_ERR_ARGUMENT;break;}
        rc=ota_service_reconcile(r.campaign,r.targets,r.count,r.image_hash.value);ok=status(z,r.index);break;
    }
    return ok && error(z,rc) ? MGMT_ERR_EOK:MGMT_ERR_EMSGSIZE;
}
#define HANDLER(n) static int command_##n(smp_streamer *ctxt) {return handle(ctxt,n);}
HANDLER(0) HANDLER(1) HANDLER(2) HANDLER(3) HANDLER(4) HANDLER(5)
HANDLER(6) HANDLER(7) HANDLER(8) HANDLER(9) HANDLER(10)
#define ENTRY(n) { .mh_read=nullptr, .mh_write=command_##n }
static const mgmt_handler handlers[]={ENTRY(0),ENTRY(1),ENTRY(2),ENTRY(3),ENTRY(4),ENTRY(5),ENTRY(6),ENTRY(7),ENTRY(8),ENTRY(9),ENTRY(10)};
static mgmt_group group={.mg_handlers=handlers,.mg_handlers_count=ARRAY_SIZE(handlers),.mg_group_id=GROUP};

/* Dispatch guard runs BEFORE handlers, unlike the informational upload status
 * notifications. Standard IMG upload/erase/test/confirm and OS reset cannot
 * bypass the single writer. stage_data always writes a real secondary copy. */
static mgmt_cb_return guard(uint32_t,mgmt_cb_return,int32_t *err,uint16_t *,bool *stop,void *data,size_t)
{
    auto cmd=static_cast<mgmt_evt_op_cmd_arg *>(data);
    if(cmd->group!=GROUP) {*err=MGMT_ERR_EACCESSDENIED;*stop=true;return MGMT_CB_ERROR_RC;}
    return MGMT_CB_OK;
}
static mgmt_callback callback={.callback=guard,.event_id=MGMT_EVT_OP_CMD_RECV};
static void register_group(void) {mgmt_callback_register(&callback);mgmt_register_group(&group);}
MCUMGR_HANDLER_DEFINE(owntech_ota,register_group);
