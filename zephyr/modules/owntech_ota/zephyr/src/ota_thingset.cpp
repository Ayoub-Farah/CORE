/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_runtime.h"
#include "ota_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <thingset.h>
#include <thingset/can.h>
#include <thingset/sdk.h>
#include <string.h>

/* Application range, separate from Control and SDK legacy DFU. Wire fields are
 * explicit little endian, versioned, and independent of C struct padding. */
enum { GROUP=0x0D, COMMAND=0x100, PARAM=0x101, STATUS=0x102, RELEASE=0x103, RELEASE_PARAM=0x104 };
static uint8_t command_buffer[224], status_buffer[240], release_buffer[48];
static THINGSET_DEFINE_BYTES(command_value, command_buffer, 0);
static THINGSET_DEFINE_BYTES(status_value, status_buffer, 0);
static THINGSET_DEFINE_BYTES(release_value, release_buffer, 0);

struct Wire {
    uint8_t *p;
    void u8(uint8_t v) { *p++=v; }
    void u16(uint16_t v) { sys_put_le16(v,p); p+=2; }
    void u32(uint32_t v) { sys_put_le32(v,p); p+=4; }
    void u64(uint64_t v) { sys_put_le64(v,p); p+=8; }
    void bytes(const void *v,size_t n) { memcpy(p,v,n);p+=n; }
};
struct Reader {
    const uint8_t *p;
    uint8_t u8() { return *p++; }
    uint16_t u16() { auto v=sys_get_le16(p);p+=2;return v; }
    uint32_t u32() { auto v=sys_get_le32(p);p+=4;return v; }
    uint64_t u64() { auto v=sys_get_le64(p);p+=8;return v; }
    void bytes(void *v,size_t n) { memcpy(v,p,n);p+=n; }
};
static constexpr size_t COMMAND_LENGTH=189, STATUS_LENGTH=174+4+OTA_EVENT_COUNT*5;
static_assert(COMMAND_LENGTH==1+8+1+(8+5*4+2+4*32)+8+1+3*4,"Command wire layout");
static_assert(STATUS_LENGTH==238 && STATUS_LENGTH<256,"Status wire layout");
#ifdef CONFIG_OWNTECH_OTA_LEAD
static void put_manifest(Wire &w,const ota_manifest &m)
{
    w.u64(m.campaign_id); w.u32(m.image_size); w.u32(m.image_content_size);
    w.u32(m.hardware_id);w.u32(m.layout_id);w.u32(m.bootloader_id);w.u8(m.protocol_version);w.u8(m.image_class);
    w.bytes(m.artifact_sha256,32);w.bytes(m.mcuboot_image_hash,32);
    w.bytes(m.version,32);w.bytes(m.build_id,32);
}
#endif
static void get_manifest(Reader &r,ota_manifest &m)
{
    m.campaign_id=r.u64();m.image_size=r.u32();m.image_content_size=r.u32();
    m.hardware_id=r.u32();m.layout_id=r.u32();m.bootloader_id=r.u32();m.protocol_version=r.u8();m.image_class=r.u8();
    r.bytes(m.artifact_sha256,32);r.bytes(m.mcuboot_image_hash,32);
    r.bytes(m.version,32);r.bytes(m.build_id,32);
}
static int32_t receive_command()
{
    if(command_value.num_bytes!=COMMAND_LENGTH) return OTA_ERR_FORMAT;
    Reader r{command_buffer};
    if(r.u8()!=OTA_PROTOCOL_VERSION) return OTA_ERR_FORMAT;
    uint8_t destination[8];r.bytes(destination,8);
    if(memcmp(destination,eui64,8)) return OTA_ERR_IDENTITY;
    ota_command cmd{};cmd.type=(ota_command_type)r.u8();get_manifest(r,cmd.manifest);
    if(cmd.type>OTA_CMD_ABORT) return OTA_ERR_FORMAT;
    r.bytes(cmd.lead_eui,8);cmd.lead_address=r.u8();
    cmd.pass_id=r.u32();cmd.start_offset=r.u32();cmd.commit_id=r.u32();
    uint8_t source=0,route=0;
    if(thingset_can_get_request_source(&source,&route) || route || source!=cmd.lead_address)
        return OTA_ERR_IDENTITY;
    if(memchr(cmd.manifest.version,0,32)==nullptr || memchr(cmd.manifest.build_id,0,32)==nullptr)
        return OTA_ERR_FORMAT;
    return ota_runtime_command(&cmd,source);
}
static int32_t release_maintenance()
{
    if(release_value.num_bytes!=48) return OTA_ERR_FORMAT;
    uint8_t source=0,route=0;
    if(thingset_can_get_request_source(&source,&route) || route) return OTA_ERR_IDENTITY;
    return ota_runtime_release(release_buffer,release_buffer+16,sys_get_le64(release_buffer+8),source);
}
static void status_callback(thingset_callback_reason why)
{
    if(why!=THINGSET_CALLBACK_PRE_READ) return;
    ota_observation o{};ota_service_local(&o);Wire w{status_buffer};
    w.u8(OTA_PROTOCOL_VERSION);w.bytes(o.identity.eui,8);w.u8(o.identity.address);
    w.u32(o.identity.usable_slot_size);w.u32(o.identity.usable_image_size);
    w.u32(o.identity.hardware_id);w.u32(o.identity.layout_id);w.u32(o.identity.bootloader_id);
    w.u8(o.identity.active_confirmed);w.u8(o.identity.slot_available);w.u8(o.identity.image_class);
    w.u64(o.status.campaign_id);w.u32(o.status.pass_id);w.u32(o.status.offset);
    w.u32(o.status.image_size);w.u32(o.status.commit_id);w.u8(o.status.state);
    w.u32((uint32_t)o.status.error);w.u16(o.status.queue_depth);
    w.u8(o.status.flash_complete);w.u8(o.status.validated);w.u8(o.status.pass_has_hole);
    w.u32(o.status.rx_dropped);w.u32(o.status.rx_rejected);
    w.bytes(o.active_mcuboot_image_hash,32);w.bytes(o.active_version,32);w.bytes(o.active_build_id,32);
    w.u8(o.healthy);w.u8(o.confirmed);w.u8(o.rolled_back);
    w.u32(o.event_mask);
    for(size_t i=0;i<OTA_EVENT_COUNT;i++) w.u32(o.event_ms[i]);
    w.bytes(o.event_order,OTA_EVENT_COUNT);
    status_value.num_bytes=w.p-status_buffer;
}
THINGSET_ADD_GROUP(TS_ID_ROOT,GROUP,"DFUCampaign",status_callback);
THINGSET_ADD_FN_INT32(GROUP,COMMAND,"xCommand",&receive_command,THINGSET_ANY_RW);
THINGSET_ADD_ITEM_BYTES(COMMAND,PARAM,"bCommand",&command_value,THINGSET_ANY_RW,0);
THINGSET_ADD_ITEM_BYTES(GROUP,STATUS,"rStatus",&status_value,THINGSET_ANY_R,0);
THINGSET_ADD_FN_INT32(GROUP,RELEASE,"xRelease",&release_maintenance,THINGSET_ANY_RW);
THINGSET_ADD_ITEM_BYTES(RELEASE,RELEASE_PARAM,"bResult",&release_value,THINGSET_ANY_RW,0);

/* Receiver has no request client or response-copy buffer. */
#ifdef CONFIG_OWNTECH_OTA_LEAD
static K_SEM_DEFINE(response_ready,0,1);
static K_MUTEX_DEFINE(request_lock);
static uint8_t response_data[256];
static size_t response_length;
static int response_error;
static k_spinlock response_lock;
static uintptr_t request_generation, active_request;
static void response(uint8_t *data,size_t size,int tx,int rx,uint8_t,void *token)
{
    auto key=k_spin_lock(&response_lock);
    if((uintptr_t)token!=active_request || !active_request) {
        k_spin_unlock(&response_lock,key);return;
    }
    response_error=tx?tx:rx;
    if(size>sizeof(response_data) || (size && !data)) response_error=OTA_ERR_CAPACITY;
    response_length=response_error?0:size;
    if(response_length) memcpy(response_data,data,size);
    k_sem_give(&response_ready);
    k_spin_unlock(&response_lock,key);
}
static int request(uint8_t address,uint8_t *data,size_t size)
{
    auto key=k_spin_lock(&response_lock);
    k_sem_reset(&response_ready);response_length=0;response_error=0;
    if(++request_generation==0) ++request_generation;
    active_request=request_generation;uintptr_t token=active_request;
    k_spin_unlock(&response_lock,key);
    int rc=thingset_can_send(data,size,address,0,response,(void *)token,K_MSEC(800));
    /* SDK always completes by its deadline; keep stack-independent buffers. */
    if(rc) rc=OTA_ERR_TRANSPORT;
    else if(k_sem_take(&response_ready,K_MSEC(1000))) rc=OTA_ERR_TIMEOUT;
    key=k_spin_lock(&response_lock);active_request=0;
    if(!rc && response_error) rc=OTA_ERR_TRANSPORT;
    k_spin_unlock(&response_lock,key);return rc;
}
static int exec_result()
{
    if(response_length<3 || response_data[0]!=THINGSET_STATUS_CHANGED || response_data[1]!=0xF6)
        return OTA_ERR_TRANSPORT;
    uint8_t v=response_data[2];
    if(response_length==3 && v<24) return v;
    if(response_length==3 && v>=0x20 && v<0x38) return -(int)(v-0x20)-1;
    if(response_length==4 && v==0x38) return -(int)response_data[3]-1;
    return OTA_ERR_TRANSPORT;
}
extern "C" int ota_network_command(const ota_target *target,const ota_command *cmd)
{
    uint8_t packet[224];Wire w{packet};
    w.u8(THINGSET_BIN_EXEC);w.u8(0x19);w.u8(COMMAND>>8);w.u8(COMMAND&255);
    w.u8(0x81);w.u8(0x58);w.u8(COMMAND_LENGTH);
    w.u8(OTA_PROTOCOL_VERSION);w.bytes(target->identity.eui,8);w.u8(cmd->type);
    put_manifest(w,cmd->manifest);w.bytes(cmd->lead_eui,8);w.u8(cmd->lead_address);
    w.u32(cmd->pass_id);w.u32(cmd->start_offset);w.u32(cmd->commit_id);
    k_mutex_lock(&request_lock,K_FOREVER);
    int rc=request(target->identity.address,packet,w.p-packet);if(!rc) rc=exec_result();
    k_mutex_unlock(&request_lock);return rc;
}
extern "C" int ota_network_status(uint8_t address,ota_observation *o)
{
    uint8_t packet[]={THINGSET_BIN_GET,0x19,STATUS>>8,STATUS&255};
    k_mutex_lock(&request_lock,K_FOREVER);
    int rc=request(address,packet,sizeof(packet));
    if(!rc && (response_length!=STATUS_LENGTH+4 || response_data[0]!=THINGSET_STATUS_CONTENT ||
               response_data[1]!=0xF6 || response_data[2]!=0x58 || response_data[3]!=STATUS_LENGTH))
        rc=OTA_ERR_FORMAT;
    if(!rc) {
        *o={};Reader r{response_data+4};o->identity.protocol_version=r.u8();
        r.bytes(o->identity.eui,8);o->identity.address=r.u8();
        o->identity.usable_slot_size=r.u32();o->identity.usable_image_size=r.u32();
        o->identity.hardware_id=r.u32();o->identity.layout_id=r.u32();o->identity.bootloader_id=r.u32();
        o->identity.active_confirmed=r.u8();o->identity.slot_available=r.u8();o->identity.image_class=r.u8();
        o->status.campaign_id=r.u64();o->status.pass_id=r.u32();o->status.offset=r.u32();
        o->status.image_size=r.u32();o->status.commit_id=r.u32();o->status.state=(ota_state)r.u8();
        o->status.error=(int32_t)r.u32();o->status.queue_depth=r.u16();
        o->status.flash_complete=r.u8();o->status.validated=r.u8();o->status.pass_has_hole=r.u8();
        o->status.rx_dropped=r.u32();o->status.rx_rejected=r.u32();
        r.bytes(o->active_mcuboot_image_hash,32);r.bytes(o->active_version,32);r.bytes(o->active_build_id,32);
        o->healthy=r.u8();o->confirmed=r.u8();o->rolled_back=r.u8();
        o->event_mask=r.u32();
        for(size_t i=0;i<OTA_EVENT_COUNT;i++) o->event_ms[i]=r.u32();
        r.bytes(o->event_order,OTA_EVENT_COUNT);
        if(o->event_mask&~((1U<<OTA_EVENT_COUNT)-1)) rc=OTA_ERR_FORMAT;
        uint32_t orders=0;uint8_t count=0;
        for(size_t i=0;i<OTA_EVENT_COUNT;i++) {
            if(o->event_mask&(1U<<i)) {
                ++count;
                uint8_t order=o->event_order[i];
                if(!order || order>OTA_EVENT_COUNT || (orders&(1U<<order))) rc=OTA_ERR_FORMAT;
                else orders|=1U<<order;
            }
            else if(o->event_ms[i] || o->event_order[i]) rc=OTA_ERR_FORMAT;
        }
        if(orders!=((1U<<(count+1))-2)) rc=OTA_ERR_FORMAT;
        if(o->identity.protocol_version!=OTA_PROTOCOL_VERSION || o->identity.address!=address ||
           (o->identity.image_class!=OTA_IMAGE_RECEIVER && o->identity.image_class!=OTA_IMAGE_LEAD) ||
           o->status.state>OTA_COMMIT_INTENT || !memchr(o->active_version,0,32) ||
           !memchr(o->active_build_id,0,32)) rc=OTA_ERR_FORMAT;
        const size_t boolean_offsets[]={30,31,64,65,66,171,172,173};
        for(size_t i:boolean_offsets) if(response_data[4+i]>1) rc=OTA_ERR_FORMAT;
    }
    k_mutex_unlock(&request_lock);return rc;
}
extern "C" int ota_network_release(uint8_t address,const uint8_t identity[8],const uint8_t hash[32],uint64_t campaign)
{
    if(thingset_can_announce_address(K_MSEC(100))) return OTA_ERR_TRANSPORT;
    uint8_t packet[55]={THINGSET_BIN_EXEC,0x19,RELEASE>>8,RELEASE&255,0x81,0x58,48};
    memcpy(packet+7,identity,8);sys_put_le64(campaign,packet+15);memcpy(packet+23,hash,32);
    k_mutex_lock(&request_lock,K_FOREVER);
    int rc=request(address,packet,sizeof(packet));if(!rc) rc=exec_result();
    k_mutex_unlock(&request_lock);return rc;
}
 #endif /* CONFIG_OWNTECH_OTA_LEAD */
extern "C" int ota_network_init(void)
{
    thingset_can_set_addr_claim_rx_callback(ota_runtime_claim);
    return thingset_can_set_report_rx_callback(ota_runtime_report);
}
