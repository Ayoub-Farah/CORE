/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_runtime.h"
#include "OtaLeadService.h"
#include "ota_storage.h"
#include "ota_protocol.h"
#include "owntech_build_info.h"
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <thingset.h>
#include <thingset/can.h>
#include <thingset/sdk.h>
#include <string.h>

static_assert(sizeof(OWNTECH_FIRMWARE_VERSION)<=OTA_IDENTITY_TEXT_SIZE,"Firmware version too long");
static_assert(sizeof(OWNTECH_FIRMWARE_BUILD_ID)<=OTA_IDENTITY_TEXT_SIZE,"Firmware build ID too long");

enum WorkType { COMMAND, REPORT, STAGE_BEGIN, STAGE_DATA, STAGE_END, START, COMMIT,
                ABORT, DISCOVER, RECONCILE, RELEASE, REFRESH };
struct Work {
    WorkType type;
    uint8_t source;
    uint16_t length;
    uint32_t offset;
    ota_command command;
    uint8_t data[OTA_MAX_REPORT_SIZE];
};
K_MSGQ_DEFINE(ota_queue,sizeof(Work),CONFIG_OWNTECH_OTA_QUEUE_DEPTH,4);
static k_spinlock snapshot_lock;
static atomic_t losses, initialized, busy, healthy, local_healthy, can_ready, lead_role;
static atomic_t identity_conflict, discovery_requested, stage_end_requested, reconcile_mode;
static atomic_t refresh_pending;
static ota_participant participant;
static ota_coordinator coordinator;
static ota_manifest staged_manifest;
static ota_observation local_snapshot;
static ota_service_diagnostics diagnostics_snapshot={"BOOT",0,false,false,false,false,false};
struct Seen { ota_observation observation; uint64_t time; bool valid; };
static Seen inventory[OTA_MAX_TARGETS], frozen[OTA_MAX_TARGETS];
static uint8_t inventory_count, frozen_count;
static bool discovery_active, discovery_done, staged, staging, verifying;
static bool waiting_can;
static uint16_t probe_address=1;
static uint64_t discovery_deadline, campaign_id;
/* snapshot_lock protects admission, including the queued discovery interval. */
static uint64_t discovery_token;
static bool discovery_reserved;
static uint32_t stage_offset;
static int service_error;
static ota_state stage_state=OTA_IDLE;
static const char *phase_snapshot="BOOT";
static uint32_t pass_snapshot;
static uint8_t reconcile_hash[32];
static ota_manifest reconcile_manifest;
static uint32_t reconcile_commit;
static uint64_t reconcile_deadline;
static bool reconcile_active;
/* Exactly one PC-supplied block, never an image in the Lead's secondary slot. */
struct SourceWindow {
    uint64_t campaign, deadline;
    uint32_t offset, length;
    bool requested, ready;
    uint8_t data[OTA_MAX_PAYLOAD];
};
static SourceWindow source_window;
/* One previous response permits an ACK-lost retry after the worker advanced
 * its credit. It cannot authorize a different payload at the same offset. */
static SourceWindow last_source;
static ota_manifest accepted_source;
static uint32_t event_mask, event_ms[OTA_EVENT_COUNT];
static uint8_t event_order[OTA_EVENT_COUNT], next_event_order;
static uint64_t event_campaign;
static uint64_t next_stream_poll;
static uint8_t stream_poll_target;
struct AcceptedRoster {
    bool valid, terminal;
    int result;
    uint64_t campaign;
    uint8_t count, ids[OTA_MAX_TARGETS][8], hash[32];
};
/* Admission records are protected by snapshot_lock, including queued requests. */
static AcceptedRoster accepted_start, accepted_reconcile;
static uint64_t accepted_prepare_campaign;

static bool same_roster(const uint8_t *a,size_t count,const uint8_t *b,size_t n)
{
    if(count!=n) return false;
    for(size_t i=0;i<n;i++) {
        bool found=false;
        for(size_t j=0;j<i;j++) if(!memcmp(b+i*8,b+j*8,8)) return false;
        for(size_t j=0;j<count;j++) if(!memcmp(b+i*8,a+j*8,8)) found=true;
        if(!found) return false;
    }
    return true;
}
static void event(ota_event id)
{
    if(event_mask&(1U<<id)) return;
    event_mask|=1U<<id;event_ms[id]=(uint32_t)k_uptime_get();event_order[id]=++next_event_order;

}
static void event_campaign_begin(uint64_t id)
{
    if(event_campaign==id) return;
    event_campaign=id;event_mask=0;next_event_order=0;
    memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));

}

extern "C" const char *ota_state_name(ota_state s)
{
    static const char *names[]={"IDLE","PREPARING","READY","PASS_OPEN","PASS_CLOSED",
        "VERIFYING","VALID","COMMITTED","REBOOTING","FAILED","ABORTED","RECOVERY_REQUIRED","SUCCESS","COMMIT_INTENT"};
    return (unsigned)s<ARRAY_SIZE(names)?names[s]:"UNKNOWN";
}
extern "C" bool ota_service_busy(void) { return atomic_get(&busy); }
extern "C" bool ota_service_healthy(void) { return atomic_get(&healthy); }
extern "C" bool ota_service_local_healthy(void) { return atomic_get(&local_healthy); }
extern "C" bool ota_service_can_ready(void) { return atomic_get(&can_ready); }
extern "C" bool ota_service_is_lead(void) { return atomic_get(&lead_role); }
/* Worker-owned state is exposed only as a coherent snapshot. */
struct RuntimeView {
    uint64_t campaign;
    uint32_t offset, image_size, pass;
    int error;
    ota_coordinator_phase coordinator_phase;
    ota_state staging_state;
    bool staged, staging, verifying, discovery_done, reconcile_active;
    bool reboot_scheduled;
};
static RuntimeView runtime_snapshot{};
static RuntimeView view() {
    auto key=k_spin_lock(&snapshot_lock);auto value=runtime_snapshot;
    k_spin_unlock(&snapshot_lock,key);return value;
}
extern "C" int ota_service_error(void) { return view().error; }
extern "C" uint32_t ota_service_stage_offset(void) { return view().offset; }
extern "C" uint32_t ota_service_pass(void) { return view().pass; }
extern "C" uint64_t ota_service_campaign(void) { return view().campaign; }
extern "C" const char *ota_service_phase(void) {
    auto key=k_spin_lock(&snapshot_lock);const char *name=phase_snapshot;
    k_spin_unlock(&snapshot_lock,key);return name;
}
static void set_phase(const char *name,bool expose_admission=false) {
    /* All callers use immutable string literals, so returned pointers stay valid. */
    auto key=k_spin_lock(&snapshot_lock);phase_snapshot=name;
    if(expose_admission) diagnostics_snapshot.phase=name;
    k_spin_unlock(&snapshot_lock,key);
}
static void update_can_readiness();
static void publish()
{
    update_can_readiness();
    participant.identity.address=thingset_can_get_inst()->node_addr;
    ota_storage_boot_identity(&participant.identity);
    ota_observation o{};o.identity=participant.identity;
    o.status=participant.status;
    /* START releases the staging admission flag before the coordinator reaches
     * this Lead's PREPARE. Keep its staged observation until the participant
     * adopts this campaign; its previous status may be idle or another campaign.
     * This changes publication only, never participant admission or ownership. */
    bool awaiting_adoption=staged_manifest.campaign_id && staged_manifest.campaign_id==event_campaign &&
        participant.status.campaign_id!=staged_manifest.campaign_id;
    if(staging || verifying || staged || awaiting_adoption) {
        o.status={};
        o.status.campaign_id=staged_manifest.campaign_id;o.status.image_size=staged_manifest.image_size;
        o.status.offset=stage_offset;o.status.state=stage_state;o.status.error=service_error;
        if(service_error && stage_state!=OTA_ABORTED) o.status.state=OTA_FAILED;
        o.status.flash_complete=(event_mask&(1U<<OTA_EVENT_FLASH_COMPLETE))!=0;
        o.status.validated=stage_state==OTA_VALID && !service_error;
    }
    if(service_error && !o.status.error) o.status.error=service_error;
    o.status.queue_depth=k_msgq_num_used_get(&ota_queue);
    o.status.rx_dropped+=atomic_get(&losses);
    o.healthy=ota_service_healthy();o.confirmed=boot_is_img_confirmed();
    o.event_mask=event_mask;memcpy(o.event_ms,event_ms,sizeof(event_ms));
    memcpy(o.event_order,event_order,sizeof(event_order));
    memcpy(o.active_version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
    memcpy(o.active_build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));
    auto key=k_spin_lock(&snapshot_lock);
    /* Release only with the completed snapshot, never between finishing the
     * scan and publishing its table/phase. Reconciliation owns its own busy. */
    if(discovery_reserved && !discovery_active && !atomic_get(&discovery_requested)) {
        discovery_reserved=false;atomic_clear(&busy);
    }
    memcpy(o.active_mcuboot_image_hash,local_snapshot.active_mcuboot_image_hash,32);
    o.rolled_back=local_snapshot.rolled_back;local_snapshot=o;
    diagnostics_snapshot={phase_snapshot,service_error,ota_service_local_healthy(),
                          ota_service_healthy(),ota_service_can_ready(),ota_service_busy(),ota_service_is_lead()};
    runtime_snapshot={campaign_id,stage_offset,staged_manifest.image_size,pass_snapshot,
        service_error,coordinator.phase,stage_state,staged,staging,verifying,
        discovery_done,reconcile_active,participant.reboot_scheduled};
    for(unsigned i=0;i<inventory_count;i++)
        if(!memcmp(inventory[i].observation.identity.eui,eui64,8)) {
            inventory[i].observation=o;inventory[i].time=k_uptime_get();inventory[i].valid=true;
        }
    if(frozen_count) for(unsigned i=0;i<frozen_count;i++)
        if(!memcmp(frozen[i].observation.identity.eui,eui64,8)) {frozen[i].observation=o;frozen[i].time=k_uptime_get();frozen[i].valid=true;}
    k_spin_unlock(&snapshot_lock,key);
    ota_feedback_state(service_error?OTA_FAILED:(staging||verifying?stage_state:participant.status.state));
}
extern "C" void ota_service_local(ota_observation *o) {
    auto key=k_spin_lock(&snapshot_lock);*o=local_snapshot;k_spin_unlock(&snapshot_lock,key);
}
extern "C" size_t ota_service_target_count(void) {
    auto key=k_spin_lock(&snapshot_lock);size_t n=frozen_count?frozen_count:inventory_count;
    k_spin_unlock(&snapshot_lock,key);return n;
}
extern "C" int ota_service_target(size_t i,ota_observation *o,bool *is_lead,uint64_t *last)
{
    auto key=k_spin_lock(&snapshot_lock);
    auto table=frozen_count?frozen:inventory;auto count=frozen_count?frozen_count:inventory_count;
    if(i>=count) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_ARGUMENT;}
    *o=table[i].observation;*last=table[i].time;*is_lead=!memcmp(o->identity.eui,eui64,8);
    k_spin_unlock(&snapshot_lock,key);return 0;
}
extern "C" void ota_service_snapshot(ota_observation *o,ota_service_diagnostics *d) {
    auto key=k_spin_lock(&snapshot_lock);*o=local_snapshot;*d=diagnostics_snapshot;
    /* Admission may reserve the worker before its next publication. Reflect
     * that reservation immediately so USB cannot advertise a free service. */
    d->busy=ota_service_busy();d->is_lead=ota_service_is_lead();
    k_spin_unlock(&snapshot_lock,key);
}
static int enqueue(Work &w);
static void request_refresh()
{
    if(!atomic_cas(&refresh_pending,0,1)) return;
    Work w{};w.type=REFRESH;
    /* A full queue already wakes the worker, whose next publication will read
     * the current address. No extra work item or periodic retry is necessary. */
    if(enqueue(w)) atomic_clear(&refresh_pending);
}
static void can_state_changed(void *) { request_refresh(); }
extern "C" void ota_runtime_claim(const uint8_t eui[8],uint8_t address)
{
    if(!address || address>=254) return;
    uint8_t local_address=thingset_can_get_inst()->node_addr;
    if(!memcmp(eui,eui64,8)) {
        if(address!=local_address) atomic_set(&identity_conflict,1);
        else {
            auto key=k_spin_lock(&snapshot_lock);
            bool changed=local_snapshot.identity.address!=local_address;
            k_spin_unlock(&snapshot_lock,key);
            if(changed) request_refresh();
        }
        return;
    }
    if(address==local_address) atomic_set(&identity_conflict,1);
    auto key=k_spin_lock(&snapshot_lock);
    for(unsigned i=0;i<inventory_count;i++) {
        auto &id=inventory[i].observation.identity;
        if(id.address==address && memcmp(id.eui,eui,8)) atomic_set(&identity_conflict,1);
        if(!memcmp(id.eui,eui,8)) {
            if(id.address!=address && ota_service_busy() && !atomic_get(&reconcile_mode)) atomic_set(&identity_conflict,1);
            id.address=address;k_spin_unlock(&snapshot_lock,key);return;
        }
    }
    if(inventory_count==OTA_MAX_TARGETS) atomic_set(&identity_conflict,1);
    else {auto &s=inventory[inventory_count++];s={};memcpy(s.observation.identity.eui,eui,8);s.observation.identity.address=address;}
    k_spin_unlock(&snapshot_lock,key);
}
static int enqueue(Work &w)
{
    if(!atomic_get(&initialized)) return OTA_ERR_STATE;
    return k_msgq_put(&ota_queue,&w,K_NO_WAIT)?OTA_ERR_QUEUE_FULL:0;
}
extern "C" void ota_runtime_report(const uint8_t *,size_t,uint8_t) {}
extern "C" int ota_runtime_command(const ota_command *,uint8_t) { return OTA_ERR_COMPATIBILITY; }

static int control(void *,const ota_target *t,const ota_command *cmd)
{
    if(atomic_get(&identity_conflict)) return OTA_ERR_IDENTITY;
    if(t->is_lead) return OTA_ERR_IDENTITY;
    if(cmd->type==OTA_CMD_PREPARE && thingset_can_announce_address(K_MSEC(100))) return OTA_AGAIN;
    int rc=ota_network_command(t,cmd);
    return rc==OTA_ERR_TRANSPORT || rc==OTA_ERR_TIMEOUT || rc==OTA_ERR_QUEUE_FULL?OTA_AGAIN:rc;
}
static int observe(void *,const ota_target *t,ota_observation *o)
{
    if(t->is_lead) {publish();ota_service_local(o);return 0;}
    uint8_t addr=t->identity.address;
    if(reconcile_active) {
        auto key=k_spin_lock(&snapshot_lock);
        for(unsigned i=0;i<inventory_count;i++) if(!memcmp(inventory[i].observation.identity.eui,t->identity.eui,8))
            addr=inventory[i].observation.identity.address;
        k_spin_unlock(&snapshot_lock,key);
    }
    int rc=ota_network_status(addr,o);
    if(rc) return OTA_AGAIN;
    if(memcmp(o->identity.eui,t->identity.eui,8)) return OTA_ERR_IDENTITY;
    auto key=k_spin_lock(&snapshot_lock);
    for(unsigned i=0;i<frozen_count;i++) if(!memcmp(frozen[i].observation.identity.eui,t->identity.eui,8)) {
        frozen[i].observation=*o;frozen[i].time=k_uptime_get();frozen[i].valid=true;
    }
    k_spin_unlock(&snapshot_lock,key);return 0;
}
static int read_image(void *,uint32_t off,uint8_t *p,size_t n)
{
    auto key=k_spin_lock(&snapshot_lock);
    auto &w=source_window;
    if(!w.requested || w.offset!=off || w.length!=n) {
        w={};w.campaign=campaign_id;w.offset=off;w.length=n;
        w.requested=true;w.deadline=k_uptime_get()+20000;
    }
    int rc=OTA_AGAIN;
    if(w.ready) {memcpy(p,w.data,n);rc=0;}
    else if(k_uptime_get()>=(int64_t)w.deadline) rc=OTA_ERR_TIMEOUT;
    k_spin_unlock(&snapshot_lock,key);return rc;
}
extern "C" void ota_service_source_request(uint64_t *campaign,uint32_t *offset,uint32_t *length)
{
    auto key=k_spin_lock(&snapshot_lock);
    *campaign=source_window.campaign;*offset=source_window.offset;
    *length=source_window.requested&&!source_window.ready&&!runtime_snapshot.error?source_window.length:0;
    k_spin_unlock(&snapshot_lock,key);
}
extern "C" int ota_service_source_data(uint64_t campaign,uint32_t off,const uint8_t *data,size_t n)
{
    if(!data || !n || n>OTA_MAX_PAYLOAD) return OTA_ERR_ARGUMENT;
    auto key=k_spin_lock(&snapshot_lock);auto &w=source_window;
    int rc=0;
    if(runtime_snapshot.error) rc=runtime_snapshot.error;
    else if(last_source.ready && campaign==last_source.campaign && off==last_source.offset && n==last_source.length) {
        rc=memcmp(last_source.data,data,n)?OTA_ERR_CONFLICT:0;
    }
    else if(!w.requested || campaign!=w.campaign || off!=w.offset || n!=w.length)
        rc=OTA_ERR_CONFLICT;
    else if(w.ready && memcmp(w.data,data,n)) rc=OTA_ERR_CONFLICT;
    else if(!w.ready && k_uptime_get()>=(int64_t)w.deadline) rc=OTA_ERR_TIMEOUT;
    else {memcpy(w.data,data,n);w.ready=true;last_source=w;}
    k_spin_unlock(&snapshot_lock,key);return rc;
}
static int send_report(void *,const uint8_t *p,size_t n) {return thingset_can_send_raw_report(p,n,K_SECONDS(2));}
static int report_complete(void *) {return 0;} /* raw sender waits for actual callbacks */
static int persist(void *,const ota_manifest *m,const ota_target *t,size_t n,uint32_t commit,ota_state state)
{ return ota_storage_persist_campaign(m,t,n,commit,state); }
/* The dedicated Lead remains running while every receiver reboots. */
static int reboot(void *,uint64_t,uint32_t,uint32_t) { return 0; }
static ota_coordinator_hooks hooks={nullptr,control,observe,read_image,send_report,report_complete,persist,reboot};

extern "C" int ota_service_set_role(bool value)
{ return value?0:OTA_ERR_COMPATIBILITY; }

extern "C" int ota_service_stage_begin(const ota_manifest *m)
{
    if(!m || m->protocol_version!=OTA_PROTOCOL_VERSION || m->image_class!=OTA_IMAGE_RECEIVER ||
       !m->campaign_id || !m->image_size || m->image_size!=m->image_content_size ||
       m->image_size>CONFIG_OWNTECH_OTA_USABLE_SLOT_SIZE ||
       m->hardware_id!=CONFIG_OWNTECH_OTA_HARDWARE_ID || m->layout_id!=CONFIG_OWNTECH_OTA_LAYOUT_ID ||
       m->bootloader_id!=CONFIG_OWNTECH_OTA_BOOTLOADER_ID) return OTA_ERR_COMPATIBILITY;
    if(!ota_service_is_lead() || !ota_service_healthy()) return OTA_ERR_STATE;
    auto key=k_spin_lock(&snapshot_lock);
    if(accepted_source.campaign_id) {
        const auto &a=accepted_source;
        bool same=a.campaign_id==m->campaign_id && a.image_size==m->image_size &&
            a.image_content_size==m->image_content_size && a.hardware_id==m->hardware_id &&
            a.layout_id==m->layout_id && a.bootloader_id==m->bootloader_id &&
            a.protocol_version==m->protocol_version && a.image_class==m->image_class &&
            !memcmp(a.artifact_sha256,m->artifact_sha256,32) && !memcmp(a.mcuboot_image_hash,m->mcuboot_image_hash,32) &&
            !memcmp(a.version,m->version,32) && !memcmp(a.build_id,m->build_id,32);
        int result=same?runtime_snapshot.error:OTA_ERR_CONFLICT;
        k_spin_unlock(&snapshot_lock,key);return result;
    }
    if(!atomic_cas(&busy,0,1)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_STATE;}
    accepted_source=*m;
    k_spin_unlock(&snapshot_lock,key);
    Work w{};w.type=STAGE_BEGIN;w.command.manifest=*m;
    set_phase("SOURCE_OPEN",true);
    key=k_spin_lock(&snapshot_lock);
    accepted_start={};accepted_reconcile={};accepted_prepare_campaign=0;
    runtime_snapshot.campaign=m->campaign_id;runtime_snapshot.image_size=m->image_size;
    runtime_snapshot.offset=0;runtime_snapshot.staging_state=OTA_PREPARING;
    local_snapshot.status.campaign_id=m->campaign_id;local_snapshot.status.state=OTA_PREPARING;
    k_spin_unlock(&snapshot_lock,key);
    int rc=enqueue(w);if(rc) {
        key=k_spin_lock(&snapshot_lock);accepted_source={};k_spin_unlock(&snapshot_lock,key);
        atomic_clear(&busy);set_phase("FAILED",true);
    }return rc;
}
extern "C" int ota_service_stage_data(uint32_t off,const uint8_t *data,size_t n)
{ return ota_service_source_data(view().campaign,off,data,n); }

extern "C" int ota_service_stage_end(void)
{
    auto state=view();
    if(state.staged) return 0;
    if(state.verifying || atomic_get(&stage_end_requested)) return OTA_AGAIN;
    if(!state.staging) return OTA_ERR_STATE;
    if(!atomic_cas(&stage_end_requested,0,1)) return OTA_AGAIN;
    Work w{};w.type=STAGE_END;int rc=enqueue(w);if(rc) atomic_clear(&stage_end_requested);return rc;
}
extern "C" int ota_service_start(uint64_t campaign,const uint8_t ids[][8],size_t n)
{
    if(!ids || !n || n>OTA_MAX_TARGETS || !campaign || !same_roster(&ids[0][0],n,&ids[0][0],n))
        return OTA_ERR_ARGUMENT;
    auto key=k_spin_lock(&snapshot_lock);
    if(accepted_start.valid) {
        bool same=accepted_start.campaign==campaign && same_roster(&accepted_start.ids[0][0],
            accepted_start.count,&ids[0][0],n);
        int rc=same?(accepted_start.terminal?accepted_start.result:0):OTA_ERR_CONFLICT;
        k_spin_unlock(&snapshot_lock,key);return rc;
    }
    auto state=runtime_snapshot;
    if(!state.staged || state.error || state.coordinator_phase!=OTA_COORD_IDLE || campaign!=state.campaign ||
       atomic_get(&identity_conflict) || !state.discovery_done) {
        k_spin_unlock(&snapshot_lock,key);return OTA_ERR_STATE;
    }
    accepted_start.valid=true;accepted_start.campaign=campaign;accepted_start.count=n;
    memcpy(accepted_start.ids,ids,n*8);
    Work w{};w.type=START;w.length=n;w.command.manifest.campaign_id=campaign;memcpy(w.data,ids,n*8);
    int rc=enqueue(w);if(rc) accepted_start={};
    k_spin_unlock(&snapshot_lock,key);return rc;
}
extern "C" int ota_service_commit(uint64_t campaign)
{
    auto state=view();
    if(campaign==state.campaign && (state.coordinator_phase==OTA_COORD_COMMIT ||
       state.coordinator_phase==OTA_COORD_REBOOT || state.coordinator_phase==OTA_COORD_RECONCILE)) return state.error;
    if(state.coordinator_phase!=OTA_COORD_VALIDATE_BARRIER || campaign!=state.campaign) return OTA_ERR_STATE;
    Work w{};w.type=COMMIT;w.command.manifest.campaign_id=campaign;return enqueue(w);
}
extern "C" int ota_service_abort(uint64_t campaign)
{
    auto state=view();
    if(!campaign || campaign!=state.campaign) return OTA_ERR_CONFLICT;
    if(state.reboot_scheduled || state.coordinator_phase==OTA_COORD_COMMIT || state.coordinator_phase==OTA_COORD_REBOOT ||
       state.coordinator_phase==OTA_COORD_RECONCILE) return OTA_ERR_STATE;
    Work w{};w.type=ABORT;return enqueue(w);
}
extern "C" int ota_service_discover(uint64_t campaign)
{
    if(!ota_service_is_lead() || !ota_service_healthy()) return OTA_ERR_STATE;
    auto key=k_spin_lock(&snapshot_lock);
    if(discovery_reserved) {
        int rc=!campaign || campaign==discovery_token?0:OTA_ERR_STATE;
        k_spin_unlock(&snapshot_lock,key);return rc;
    }
    auto state=runtime_snapshot;
    if(!ota_service_is_lead() || !ota_service_healthy() || ota_service_busy() || state.error || state.coordinator_phase!=OTA_COORD_IDLE ||
       state.staging || state.staged || state.verifying || state.reconcile_active) {
        k_spin_unlock(&snapshot_lock,key);return OTA_ERR_STATE;
    }
    if(state.discovery_done && (!campaign || campaign==discovery_token)) {
        k_spin_unlock(&snapshot_lock,key);return 0;
    }
    if(!atomic_cas(&busy,0,1)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_STATE;}
    Work w{};w.type=DISCOVER;w.command.manifest.campaign_id=campaign;
    int rc=enqueue(w);
    if(rc) atomic_clear(&busy);
    else {
        discovery_reserved=true;discovery_token=campaign;atomic_set(&discovery_requested,1);
        runtime_snapshot.discovery_done=false;
        phase_snapshot=diagnostics_snapshot.phase="DISCOVERING";
    }
    k_spin_unlock(&snapshot_lock,key);return rc;
}
extern "C" int ota_service_reconcile(uint64_t campaign,const uint8_t ids[][8],size_t n,const uint8_t hash[32])
{
    if(!ids || !hash || !campaign || !n || n>OTA_MAX_TARGETS || !ota_service_is_lead() ||
       !ota_service_can_ready() ||
       !same_roster(&ids[0][0],n,&ids[0][0],n)) return OTA_ERR_ARGUMENT;
    auto key=k_spin_lock(&snapshot_lock);
    if(accepted_reconcile.valid) {
        bool same=accepted_reconcile.campaign==campaign && !memcmp(accepted_reconcile.hash,hash,32) &&
            same_roster(&accepted_reconcile.ids[0][0],accepted_reconcile.count,&ids[0][0],n);
        int rc=same?(accepted_reconcile.terminal?accepted_reconcile.result:0):OTA_ERR_CONFLICT;
        k_spin_unlock(&snapshot_lock,key);return rc;
    }
    if(!atomic_cas(&busy,0,1)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_STATE;}
    accepted_reconcile.valid=true;accepted_reconcile.campaign=campaign;accepted_reconcile.count=n;
    memcpy(accepted_reconcile.ids,ids,n*8);memcpy(accepted_reconcile.hash,hash,32);
    runtime_snapshot.campaign=campaign;phase_snapshot=diagnostics_snapshot.phase="POSTBOOT_CHECK";
    Work w{};w.type=RECONCILE;w.command.manifest.campaign_id=campaign;w.length=n;
    memcpy(w.data,ids,n*8);memcpy(w.command.manifest.mcuboot_image_hash,hash,32);
    int rc=enqueue(w);if(rc) {accepted_reconcile={};atomic_clear(&busy);}
    k_spin_unlock(&snapshot_lock,key);return rc;
}
extern "C" int ota_runtime_release(const uint8_t[8],const uint8_t[32],uint64_t,uint8_t)
{ return OTA_ERR_COMPATIBILITY; }

static void begin_discovery()
{
    discovery_active=true;discovery_done=false;probe_address=1;discovery_deadline=k_uptime_get()+18000;
    atomic_set(&discovery_requested,1);
    (void)thingset_can_announce_address(K_MSEC(100));
    auto key=k_spin_lock(&snapshot_lock);inventory_count=0;
    if(!reconcile_active) frozen_count=0;
    /* A previously observed conflicting identity cannot be silently forgotten
     * by a rescan, especially when it used this node's own (unprobed) address. */
    k_spin_unlock(&snapshot_lock,key);
    set_phase("DISCOVERING");
}
static void discovery_step()
{
    if(k_uptime_get()>=(int64_t)discovery_deadline) {
        discovery_active=false;discovery_done=true;atomic_clear(&discovery_requested);
        service_error=OTA_ERR_TIMEOUT;set_phase("FAILED");return;
    }
    if(probe_address<=253) {
        if(probe_address!=thingset_can_get_inst()->node_addr)
            (void)thingset_can_probe_address(probe_address,K_MSEC(20));
        ++probe_address;return;
    }
    unsigned i;
    auto key=k_spin_lock(&snapshot_lock);
    for(i=0;i<inventory_count;i++) if(!inventory[i].valid) break;
    uint8_t address=i<inventory_count?inventory[i].observation.identity.address:0;
    uint8_t expected[8]={};if(address) memcpy(expected,inventory[i].observation.identity.eui,8);
    k_spin_unlock(&snapshot_lock,key);
    if(address) {
        ota_observation o{};int rc=ota_network_status(address,&o);
        key=k_spin_lock(&snapshot_lock);
        if(!rc && memcmp(expected,o.identity.eui,8)) atomic_set(&identity_conflict,1);
        if(!rc) {inventory[i].observation=o;inventory[i].time=k_uptime_get();}
        /* A non-OTA claimant is retained and marked incompatible, never dropped. */
        else inventory[i].observation.status.error=OTA_ERR_TRANSPORT;
        inventory[i].valid=true;k_spin_unlock(&snapshot_lock,key);return;
    }
    discovery_active=false;discovery_done=true;atomic_clear(&discovery_requested);
    if(atomic_get(&identity_conflict)) service_error=OTA_ERR_IDENTITY;
    set_phase(reconcile_active?"POSTBOOT_CHECK":"IDLE");
}
static int freeze(const uint8_t *ids,size_t n,ota_target *targets,bool recovery)
{
    auto key=k_spin_lock(&snapshot_lock);
    /* Validate the entire set before changing the published frozen roster. */
    for(size_t i=0;i<n;i++) {
        for(size_t j=0;j<i;j++) if(!memcmp(ids+i*8,ids+j*8,8)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_IDENTITY;}
        size_t j=0;for(;j<inventory_count;j++) if(!memcmp(ids+i*8,inventory[j].observation.identity.eui,8)) break;
        if(j==inventory_count && !recovery) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_IDENTITY;}
        if(!memcmp(ids+i*8,eui64,8)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_IDENTITY;}
    }
    for(size_t i=0;i<n;i++) {
        size_t j=0;for(;j<inventory_count;j++) if(!memcmp(ids+i*8,inventory[j].observation.identity.eui,8)) break;
        frozen[i]={};
        if(j<inventory_count) frozen[i]=inventory[j];
        else {memcpy(frozen[i].observation.identity.eui,ids+i*8,8);frozen[i].observation.status.error=OTA_ERR_TIMEOUT;}
        targets[i].identity=frozen[i].observation.identity;targets[i].is_lead=false;
    }
    frozen_count=n;k_spin_unlock(&snapshot_lock,key);return 0;
}

static void complete_local_release()
{
    participant.status.state=OTA_SUCCEEDED;
    ota_storage_boot_identity(&participant.identity);
    coordinator={};staging=staged=verifying=false;discovery_done=false;
    auto key=k_spin_lock(&snapshot_lock);accepted_prepare_campaign=0;source_window={};last_source={};accepted_source={};k_spin_unlock(&snapshot_lock,key);
    service_error=0;atomic_clear(&busy);set_phase("SUCCESS");
}
static void finish_reconcile(int result)
{
    reconcile_active=false;atomic_clear(&reconcile_mode);atomic_clear(&busy);
    auto key=k_spin_lock(&snapshot_lock);
    accepted_reconcile.terminal=true;accepted_reconcile.result=result;
    k_spin_unlock(&snapshot_lock,key);
    if(result) {service_error=result;set_phase("PARTIAL");}
}
static void process(Work &w)
{
    int rc=0;
    switch(w.type) {
    case REFRESH: atomic_clear(&refresh_pending);break;
    case COMMAND: case REPORT: case RELEASE: rc=OTA_ERR_COMPATIBILITY;break;
    case STAGE_BEGIN:
        staged_manifest=w.command.manifest;campaign_id=staged_manifest.campaign_id;stage_offset=0;
        event_campaign_begin(campaign_id);
        staging=true;staged=verifying=false;service_error=0;atomic_clear(&stage_end_requested);
        {auto key=k_spin_lock(&snapshot_lock);source_window={};last_source={};k_spin_unlock(&snapshot_lock,key);}
        stage_state=OTA_READY;set_phase("SOURCE_OPEN");break;
    case STAGE_DATA: rc=OTA_ERR_STATE;break;
    case STAGE_END:
        if(!staging && !staged) rc=OTA_ERR_STATE;
        else {staged=true;staging=false;stage_state=OTA_READY;set_phase("SOURCE_READY");}
        atomic_clear(&stage_end_requested);break;

    case START: {
        if(!staged || coordinator.phase!=OTA_COORD_IDLE ||
           w.command.manifest.campaign_id!=staged_manifest.campaign_id) {rc=OTA_ERR_STATE;break;}
        ota_target targets[OTA_MAX_TARGETS]{};rc=freeze(w.data,w.length,targets,false);
        if(!rc) {
            ota_coordinator_options opts;ota_coordinator_default_options(&opts);
            opts.inter_block_ms=CONFIG_OWNTECH_OTA_BLOCK_INTERVAL_MS;
            memcpy(opts.lead_eui,eui64,8);opts.lead_address=thingset_can_get_inst()->node_addr;
            uint32_t commit=(uint32_t)staged_manifest.campaign_id;if(!commit) commit=1;
            rc=ota_coordinator_start(&coordinator,&staged_manifest,targets,w.length,commit,&opts,&hooks,k_uptime_get());
            if(!rc) {staged=false;next_stream_poll=0;stream_poll_target=0;set_phase("PREPARING");}
        }
        if(rc) {
            auto key=k_spin_lock(&snapshot_lock);accepted_start.terminal=true;accepted_start.result=rc;
            k_spin_unlock(&snapshot_lock,key);
        }
        break;
    }
    case COMMIT: rc=ota_coordinator_commit(&coordinator,w.command.manifest.campaign_id,coordinator.commit_id);break;
    case ABORT:
        if(participant.reboot_scheduled) rc=OTA_ERR_STATE;
        else if(coordinator.phase!=OTA_COORD_IDLE) rc=ota_coordinator_abort(&coordinator);
        else rc=0;
        if(rc<0 && participant.reboot_scheduled) {
            service_error=rc;set_phase("REBOOTING");publish();return;
        }
        if(rc<0 && coordinator.phase!=OTA_COORD_FAILED) {
            service_error=rc;publish();return;
        }
        if(reconcile_active) finish_reconcile(OTA_ERR_STATE);
        discovery_active=false;atomic_clear(&discovery_requested);
        staging=staged=verifying=false;stage_state=OTA_ABORTED;participant.status.state=OTA_ABORTED;set_phase("ABORTED");
        service_error=OTA_ERR_STATE;break;
    case DISCOVER: {
        auto key=k_spin_lock(&snapshot_lock);
        bool owner=discovery_reserved && discovery_token==w.command.manifest.campaign_id;
        k_spin_unlock(&snapshot_lock,key);
        if(owner) begin_discovery();
        break;
    }
    case RECONCILE: {
        if(reconcile_active || staging || staged || verifying || coordinator.phase!=OTA_COORD_IDLE) {
            rc=OTA_ERR_STATE;break;
        }
        ota_manifest recorded{};ota_target expected[OTA_MAX_TARGETS]{};
        size_t count=OTA_MAX_TARGETS;uint32_t commit=0;
        rc=ota_storage_load_campaign(&recorded,expected,&count,&commit);
        if(rc || count!=w.length || recorded.campaign_id!=w.command.manifest.campaign_id || !commit ||
           memcmp(recorded.mcuboot_image_hash,w.command.manifest.mcuboot_image_hash,32)) {
            rc=OTA_ERR_IDENTITY;break;
        }
        for(size_t i=0;i<count;i++) {
            bool found=false;
            for(size_t k=0;k<w.length;k++) if(!memcmp(expected[i].identity.eui,w.data+k*8,8)) found=true;
            if(!found) {rc=OTA_ERR_IDENTITY;break;}
        }
        if(rc) break;
        ota_target targets[OTA_MAX_TARGETS]{};rc=freeze(w.data,w.length,targets,true);
        if(rc) break;
        campaign_id=recorded.campaign_id;memcpy(reconcile_hash,w.command.manifest.mcuboot_image_hash,32);
        reconcile_manifest=recorded;reconcile_commit=commit;
        reconcile_active=true;atomic_set(&reconcile_mode,1);atomic_set(&busy,1);reconcile_deadline=k_uptime_get()+60000;
        begin_discovery();break;
    }
    }
    if(rc<0) {
        if(w.type==RECONCILE) finish_reconcile(rc);
        else {service_error=rc;set_phase("FAILED");}
    }
    publish();
}
static void reconcile_step()
{
    if(atomic_get(&identity_conflict)) {finish_reconcile(OTA_ERR_IDENTITY);return;}
    if(k_uptime_get()>(int64_t)reconcile_deadline) {
        finish_reconcile(OTA_ERR_TIMEOUT);return;
    }
    bool all=true;
    for(unsigned i=0;i<frozen_count;i++) {
        ota_target t{frozen[i].observation.identity,!memcmp(frozen[i].observation.identity.eui,eui64,8)};
        ota_observation o{};int rc=observe(nullptr,&t,&o);
        if(rc==OTA_ERR_IDENTITY) {finish_reconcile(rc);return;}
        if(rc || !o.healthy || !o.confirmed || !o.identity.active_confirmed || o.rolled_back || o.status.error ||
           (o.status.state!=OTA_RECOVERY_REQUIRED && o.status.state!=OTA_SUCCEEDED) ||
           o.status.campaign_id!=campaign_id || o.status.commit_id!=reconcile_commit ||
           o.status.image_size!=reconcile_manifest.image_size ||
           o.identity.protocol_version!=OTA_PROTOCOL_VERSION || o.identity.image_class!=OTA_IMAGE_RECEIVER ||
           o.identity.hardware_id!=reconcile_manifest.hardware_id || o.identity.layout_id!=reconcile_manifest.layout_id ||
           o.identity.bootloader_id!=reconcile_manifest.bootloader_id ||
           memcmp(o.active_mcuboot_image_hash,reconcile_hash,32) ||
           strncmp(o.active_version,reconcile_manifest.version,32) || strncmp(o.active_build_id,reconcile_manifest.build_id,32))
            all=false;
    }
    if(all) {
        /* Release only after the complete frozen group passed the same barrier. */
        bool released=true;
        for(unsigned i=0;i<frozen_count;i++) if(memcmp(frozen[i].observation.identity.eui,eui64,8)) {
            if(frozen[i].observation.status.state==OTA_SUCCEEDED &&
               frozen[i].observation.status.campaign_id==campaign_id &&
               frozen[i].observation.status.commit_id==reconcile_commit) continue;
            int rc=ota_network_release(frozen[i].observation.identity.address,frozen[i].observation.identity.eui,reconcile_hash,campaign_id);
            if(rc) return;
            released=false; /* Accepted is not completed: confirm the next fresh status. */
        }
        if(!released) return;
        ota_target targets[OTA_MAX_TARGETS]{};
        for(unsigned i=0;i<frozen_count;i++) targets[i].identity=frozen[i].observation.identity;
        if(ota_storage_persist_campaign(&reconcile_manifest,targets,frozen_count,reconcile_commit,OTA_SUCCEEDED)) {
            finish_reconcile(OTA_ERR_JOURNAL);return;
        }
        complete_local_release();finish_reconcile(0);return;
    }
    if(k_uptime_get()>(int64_t)reconcile_deadline) finish_reconcile(OTA_ERR_TIMEOUT);
}
/* Called only by the worker, including every publication. A CAN notification
 * whose FIFO insertion loses to a full queue is therefore not lost. */
static void update_can_readiness()
{
    if(!waiting_can || service_error || !ota_service_local_healthy()) return;
    auto can=thingset_can_get_inst();
    if(atomic_get(&can->init_error)) {
        waiting_can=false;service_error=OTA_ERR_TRANSPORT;
        atomic_clear(&local_healthy);ota_safety_restore(true);set_phase("FAILED");
    }
    else if(atomic_get(&can->ready)) {
        waiting_can=false;
        if(ota_network_init()) {
            service_error=OTA_ERR_TRANSPORT;ota_safety_restore(true);set_phase("FAILED");
        }
        else {atomic_set(&can_ready,1);atomic_set(&healthy,1);set_phase("IDLE");}
    }
}
static void initialize_runtime()
{
    int rc=ota_storage_init();
    atomic_set(&lead_role,1);memcpy(participant.identity.eui,eui64,8);
    ota_storage_boot_identity(&participant.identity);
    if(rc || ota_storage_recovery_required() || ota_storage_maintenance()) service_error=OTA_ERR_JOURNAL;
    if(!service_error && ota_storage_active_hash(local_snapshot.active_mcuboot_image_hash)) service_error=OTA_ERR_HEALTH;
    bool strict_boot=false;

    auto can=thingset_can_get_inst();
    thingset_can_set_state_callback(can_state_changed,nullptr);
    uint64_t deadline=k_uptime_get()+CONFIG_OWNTECH_OTA_HEALTH_TIMEOUT_MS;
    /* A successfully started controller is required even for USB-only first
     * provisioning. ACK/address acquisition is a separate network condition. */
    while(!service_error && !atomic_get(&can->driver_started) && !atomic_get(&can->init_error) &&
          k_uptime_get()<(int64_t)deadline) k_sleep(K_MSEC(20));
    if(!service_error && (!atomic_get(&can->driver_started) || atomic_get(&can->init_error))) service_error=OTA_ERR_TRANSPORT;
    if(!service_error && owntech_ota_check_health()) service_error=OTA_ERR_HEALTH;
    if(!service_error) atomic_set(&local_healthy,1);

    /* Campaign images keep the collective postboot barrier. A late peer must
     * not turn a failed campaign boot into an implicitly accepted image. */
    while(!service_error && strict_boot && !atomic_get(&can->ready) && !atomic_get(&can->init_error) &&
          k_uptime_get()<(int64_t)deadline) k_sleep(K_MSEC(20));
    if(!service_error && (atomic_get(&can->init_error) || (strict_boot && !atomic_get(&can->ready)))) service_error=OTA_ERR_TRANSPORT;
    if(!service_error && atomic_get(&can->ready)) {
        if(ota_network_init()) service_error=OTA_ERR_TRANSPORT;
        else atomic_set(&can_ready,1);
    }
    if(!service_error && strict_boot) event(OTA_EVENT_POSTBOOT_CHECK);
    if(!service_error && !boot_is_img_confirmed() && boot_write_img_confirmed()) service_error=OTA_ERR_HEALTH;
    if(!service_error) {
        if(ota_service_can_ready()) atomic_set(&healthy,1);
        else waiting_can=true;
        if(!ota_storage_maintenance()) ota_safety_restore(false);
    }
    set_phase(service_error?"FAILED":waiting_can?"WAITING_CAN":ota_storage_recovery_required()?"RECOVERY_REQUIRED":"IDLE");
    atomic_set(&initialized,1);
    /* Recheck after enabling FIFO delivery, closing a ready-during-init race. */
    publish();
}
static void stream_status_poll()
{
    if(!coordinator.target_count || k_uptime_get()<(int64_t)next_stream_poll) return;
    next_stream_poll=k_uptime_get()+500;
    for(unsigned attempts=0;attempts<coordinator.target_count;attempts++) {
        uint8_t index=stream_poll_target++%coordinator.target_count;
        if(coordinator.targets[index].is_lead) continue;
        ota_observation o{};int rc=observe(nullptr,&coordinator.targets[index],&o);
        next_stream_poll=k_uptime_get()+500;
        if(rc==OTA_ERR_IDENTITY) atomic_set(&identity_conflict,1);
        /* Supervision is periodic, not an acknowledgement or acceptance gate
         * for any data block. The end-pass repair barrier remains authoritative. */
        break;
    }
}
static bool coordinator_active()
{
    return coordinator.phase!=OTA_COORD_IDLE && coordinator.phase!=OTA_COORD_FAILED &&
           coordinator.phase!=OTA_COORD_DONE && coordinator.phase!=OTA_COORD_RECOVERY;
}
static void worker_iteration()
{
    /* Only these worker-owned states have deadlines or autonomous progress.
     * A participant, USB staging, recovery awaiting a request, and completed
     * campaigns all advance through the same FIFO, which wakes a blocked get.
     * In particular busy alone must not turn an idle participant into a poller. */
    bool timed=discovery_active || reconcile_active || coordinator_active();
    Work w{};
    bool received=!k_msgq_get(&ota_queue,&w,timed?K_MSEC(5):K_FOREVER);
    if(received) process(w);
    else if(!timed) return; /* No publication on a cancelled/spurious wait. */
    if(discovery_active) discovery_step();
    else if(reconcile_active) reconcile_step();
    else if(coordinator_active()) {
        if(atomic_get(&identity_conflict) && !participant.reboot_scheduled) {
            (void)ota_coordinator_abort(&coordinator);service_error=OTA_ERR_IDENTITY;set_phase("FAILED");publish();return;
        }
        int rc=ota_coordinator_step(&coordinator,k_uptime_get());
        pass_snapshot=coordinator.pass_id;stage_offset=coordinator.tx_offset;
        if(coordinator.phase==OTA_COORD_STREAM) {event(OTA_EVENT_CAN_TRANSFER_BEGIN);stream_status_poll();}
        if(coordinator.phase==OTA_COORD_FINALIZE) event(OTA_EVENT_CAN_TRANSFER_END);
        if(coordinator.phase==OTA_COORD_VALIDATE_BARRIER) event(OTA_EVENT_ALL_VALIDATED);
        if(coordinator.phase==OTA_COORD_RECONCILE) {
            reconcile_manifest=coordinator.manifest;reconcile_commit=coordinator.commit_id;
            memcpy(reconcile_hash,coordinator.manifest.mcuboot_image_hash,32);
            reconcile_active=true;atomic_set(&reconcile_mode,1);
            reconcile_deadline=k_uptime_get()+60000;begin_discovery();
        }
        if(rc<0) {service_error=rc;set_phase("FAILED");}
        else {
            static const char *phases[]={"IDLE","PREPARING","BEGIN_PASS","CAN_TRANSFER","END_PASS","VERIFYING",
                "ALL_VALIDATED","COMMITTING","REBOOTING","REBOOTING","SUCCESS","FAILED","RECOVERY_REQUIRED"};
            set_phase(phases[coordinator.phase]);
        }
    }
    else return; /* process() already published the event's final snapshot. */
    publish();
}
static void run(void *,void *,void *)
{
    initialize_runtime();
    for (;;) worker_iteration();
}
K_THREAD_DEFINE(ota_worker,CONFIG_OWNTECH_OTA_STACK_SIZE,run,nullptr,nullptr,nullptr,7,0,0);
