/* SPDX-License-Identifier: Apache-2.0 */
#include "ota_runtime.h"
#include "ota_storage.h"
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

/* Exactly one campaign, no neighbor table. Commands and data share storage. */
enum WorkType { COMMAND, REPORT, RELEASE, REFRESH };
struct Work {
    WorkType type;
    uint8_t source;
    uint16_t length;
    union { ota_command command; uint8_t data[OTA_MAX_REPORT_SIZE]; };
};
static_assert(sizeof(Work)<=OTA_MAX_REPORT_SIZE+16,"Receiver queue must remain compact");
K_MSGQ_DEFINE(ota_queue,sizeof(Work),CONFIG_OWNTECH_OTA_QUEUE_DEPTH,4);
static k_spinlock snapshot_lock;
static atomic_t initialized, busy, healthy, local_healthy, can_ready, losses;
static atomic_t identity_conflict, refresh_pending, release_pending;
static ota_participant participant;
static ota_participant_hooks storage_hooks;
static ota_observation local_snapshot;
static ota_service_diagnostics diagnostics_snapshot={"BOOT",0,false,false,false,false,false};
/* Before admission this is the last claimant; afterwards only the authorized
 * Lead can update it. PREPARE is preceded by an address announcement. */
static uint8_t lead_eui[8], lead_address;
static bool lead_bound, lead_seen, waiting_can;
static uint64_t accepted_campaign;
static ota_manifest accepted_manifest;
static int service_error;
static uint32_t event_mask, event_ms[OTA_EVENT_COUNT];
static uint8_t event_order[OTA_EVENT_COUNT], next_event_order;

extern "C" const char *ota_state_name(ota_state s)
{
    static const char *names[]={"IDLE","PREPARING","READY","PASS_OPEN","PASS_CLOSED",
        "VERIFYING","VALID","COMMITTED","REBOOTING","FAILED","ABORTED","RECOVERY_REQUIRED","SUCCESS",
        "COMMIT_INTENT"};
    return (unsigned)s<ARRAY_SIZE(names)?names[s]:"UNKNOWN";
}
extern "C" bool ota_service_busy(void) { return atomic_get(&busy); }
extern "C" bool ota_service_healthy(void) { return atomic_get(&healthy); }
extern "C" bool ota_service_local_healthy(void) { return atomic_get(&local_healthy); }
extern "C" bool ota_service_can_ready(void) { return atomic_get(&can_ready); }
extern "C" bool ota_service_is_lead(void) { return false; }
extern "C" void ota_service_snapshot(ota_observation *o,ota_service_diagnostics *d)
{
    auto key=k_spin_lock(&snapshot_lock);*o=local_snapshot;*d=diagnostics_snapshot;
    d->busy=ota_service_busy();k_spin_unlock(&snapshot_lock,key);
}
extern "C" void ota_service_local(ota_observation *o)
{
    auto key=k_spin_lock(&snapshot_lock);*o=local_snapshot;k_spin_unlock(&snapshot_lock,key);
}
extern "C" int ota_service_error(void)
{ auto key=k_spin_lock(&snapshot_lock);int rc=diagnostics_snapshot.error;k_spin_unlock(&snapshot_lock,key);return rc; }
extern "C" const char *ota_service_phase(void)
{ auto key=k_spin_lock(&snapshot_lock);auto s=diagnostics_snapshot.phase;k_spin_unlock(&snapshot_lock,key);return s; }
extern "C" uint64_t ota_service_campaign(void)
{ ota_observation o{};ota_service_local(&o);return o.status.campaign_id; }
extern "C" uint32_t ota_service_pass(void)
{ ota_observation o{};ota_service_local(&o);return o.status.pass_id; }
extern "C" uint32_t ota_service_stage_offset(void)
{ ota_observation o{};ota_service_local(&o);return o.status.offset; }

static void event(ota_event id)
{
    if(event_mask&(1U<<id)) return;
    event_mask|=1U<<id;event_ms[id]=(uint32_t)k_uptime_get();event_order[id]=++next_event_order;
    ota_storage_set_events(event_mask,event_ms,event_order);
}
static int enqueue(Work &w)
{
    if(!atomic_get(&initialized)) return OTA_ERR_STATE;
    return k_msgq_put(&ota_queue,&w,K_NO_WAIT)?OTA_ERR_QUEUE_FULL:0;
}
static bool same_manifest(const ota_manifest &a,const ota_manifest &b)
{
    return a.campaign_id==b.campaign_id && a.image_size==b.image_size &&
        a.image_content_size==b.image_content_size && a.hardware_id==b.hardware_id &&
        a.layout_id==b.layout_id && a.bootloader_id==b.bootloader_id &&
        a.protocol_version==b.protocol_version && a.image_class==b.image_class &&
        !memcmp(a.artifact_sha256,b.artifact_sha256,32) && !memcmp(a.mcuboot_image_hash,b.mcuboot_image_hash,32) &&
        !memcmp(a.version,b.version,32) && !memcmp(a.build_id,b.build_id,32);
}
static void request_refresh()
{
    if(!atomic_cas(&refresh_pending,0,1)) return;
    Work w{};w.type=REFRESH;
    if(enqueue(w)) atomic_clear(&refresh_pending);
}
static void can_state_changed(void *) { request_refresh(); }
static void publish()
{
    auto can=thingset_can_get_inst();
    if(waiting_can && !service_error && ota_service_local_healthy()) {
        if(atomic_get(&can->init_error)) {service_error=OTA_ERR_TRANSPORT;waiting_can=false;ota_safety_restore(true);}
        else if(atomic_get(&can->ready)) {waiting_can=false;atomic_set(&can_ready,1);atomic_set(&healthy,1);}
    }
    participant.identity.address=can->node_addr;
    ota_storage_boot_identity(&participant.identity);
    ota_observation o{};o.identity=participant.identity;o.status=participant.status;
    if(service_error && !o.status.error) o.status.error=service_error;
    o.status.queue_depth=k_msgq_num_used_get(&ota_queue);o.status.rx_dropped+=atomic_get(&losses);
    o.healthy=ota_service_healthy();o.confirmed=boot_is_img_confirmed();
    o.event_mask=event_mask;memcpy(o.event_ms,event_ms,sizeof(event_ms));memcpy(o.event_order,event_order,sizeof(event_order));
    memcpy(o.active_version,OWNTECH_FIRMWARE_VERSION,sizeof(OWNTECH_FIRMWARE_VERSION));
    memcpy(o.active_build_id,OWNTECH_FIRMWARE_BUILD_ID,sizeof(OWNTECH_FIRMWARE_BUILD_ID));
    auto key=k_spin_lock(&snapshot_lock);
    memcpy(o.active_mcuboot_image_hash,local_snapshot.active_mcuboot_image_hash,32);
    o.rolled_back=local_snapshot.rolled_back;local_snapshot=o;
    diagnostics_snapshot={service_error?"FAILED":waiting_can?"WAITING_CAN":ota_state_name(o.status.state),
        service_error,ota_service_local_healthy(),o.healthy,ota_service_can_ready(),ota_service_busy(),false};
    k_spin_unlock(&snapshot_lock,key);
    ota_feedback_state(service_error?OTA_FAILED:participant.status.state);
}
extern "C" void ota_runtime_claim(const uint8_t eui[8],uint8_t address)
{
    if(!address || address>=254) return;
    uint8_t local_address=thingset_can_get_inst()->node_addr;
    auto key=k_spin_lock(&snapshot_lock);
    if(!memcmp(eui,eui64,8)) {
        if(address!=local_address) atomic_set(&identity_conflict,1);
    }
    else if(address==local_address) atomic_set(&identity_conflict,1);
    else if(!lead_bound || (!ota_service_busy() && local_snapshot.status.state==OTA_SUCCEEDED)) {memcpy(lead_eui,eui,8);lead_address=address;lead_seen=true;}
    else if(!memcmp(eui,lead_eui,8)) {
        if(lead_seen && lead_address!=address && ota_service_busy()) atomic_set(&identity_conflict,1);
        else {lead_address=address;lead_seen=true;}
    }
    else if(lead_seen && address==lead_address) atomic_set(&identity_conflict,1);
    k_spin_unlock(&snapshot_lock,key);
    if(atomic_get(&identity_conflict)) request_refresh();
}
extern "C" int ota_runtime_command(const ota_command *cmd,uint8_t source)
{
    if(!cmd || !cmd->manifest.campaign_id) return OTA_ERR_ARGUMENT;
    if(!ota_service_healthy()) return OTA_ERR_STATE;
    if(source!=cmd->lead_address || cmd->adopt || !memcmp(cmd->lead_eui,eui64,8)) return OTA_ERR_IDENTITY;
    Work w{};w.type=COMMAND;w.source=source;w.command=*cmd;
    auto key=k_spin_lock(&snapshot_lock);
    if(atomic_get(&identity_conflict)) {k_spin_unlock(&snapshot_lock,key);return OTA_ERR_IDENTITY;}
    if(!lead_seen || lead_address!=source || memcmp(lead_eui,cmd->lead_eui,8)) {
        k_spin_unlock(&snapshot_lock,key);return OTA_AGAIN;
    }
    bool reserved=false;
    if(cmd->type==OTA_CMD_PREPARE) {
        reserved=atomic_cas(&busy,0,1);
        if(!reserved && (accepted_campaign!=cmd->manifest.campaign_id || !same_manifest(accepted_manifest,cmd->manifest))) {
            k_spin_unlock(&snapshot_lock,key);return OTA_ERR_CONFLICT;
        }
        if(reserved) {accepted_campaign=cmd->manifest.campaign_id;accepted_manifest=cmd->manifest;lead_bound=true;}
    }
    else if(!ota_service_busy() || accepted_campaign!=cmd->manifest.campaign_id || !same_manifest(accepted_manifest,cmd->manifest)) {
        k_spin_unlock(&snapshot_lock,key);return OTA_ERR_CONFLICT;
    }
    int rc=enqueue(w);
    if(rc && reserved) {atomic_clear(&busy);accepted_campaign=0;lead_bound=false;}
    k_spin_unlock(&snapshot_lock,key);return rc;
}
extern "C" void ota_runtime_report(const uint8_t *data,size_t n,uint8_t source)
{
    if(n>OTA_MAX_REPORT_SIZE || n<OTA_HEADER_SIZE || memcmp(data,"OTAC",4)) return;
    auto key=k_spin_lock(&snapshot_lock);
    bool owner=lead_bound && lead_seen && source==lead_address && ota_service_busy();
    k_spin_unlock(&snapshot_lock,key);if(!owner) return;
    Work w{};w.type=REPORT;w.source=source;w.length=n;memcpy(w.data,data,n);
    if(enqueue(w)) atomic_inc(&losses);
}
extern "C" int ota_runtime_release(const uint8_t identity[8],const uint8_t hash[32],uint64_t campaign,uint8_t source)
{
    if(memcmp(identity,eui64,8) || !ota_service_healthy()) return OTA_ERR_IDENTITY;
    auto key=k_spin_lock(&snapshot_lock);
    bool owner=lead_bound && lead_seen && lead_address==source;
    k_spin_unlock(&snapshot_lock,key);
    if(atomic_get(&identity_conflict)) return OTA_ERR_IDENTITY;
    if(!owner) return OTA_AGAIN;
    ota_observation local{};ota_service_local(&local);
    if(!campaign || local.status.campaign_id!=campaign || !local.status.commit_id || local.rolled_back || !local.confirmed ||
       local.status.error || memcmp(local.active_mcuboot_image_hash,hash,32)) return OTA_ERR_HEALTH;
    if(local.status.state==OTA_SUCCEEDED) return 0;
    if(!atomic_cas(&release_pending,0,1)) return 0;
    Work w{};w.type=RELEASE;w.source=source;memcpy(w.data,hash,32);
    int rc=enqueue(w);if(rc) atomic_clear(&release_pending);return rc;
}
static int tracked_prepare(void *,const ota_manifest *m,bool adopt)
{
    event_mask=next_event_order=0;memset(event_ms,0,sizeof(event_ms));memset(event_order,0,sizeof(event_order));
    event(OTA_EVENT_ERASE_BEGIN);publish();
    int rc=storage_hooks.prepare(storage_hooks.context,m,adopt);
    if(!rc) event(OTA_EVENT_ERASE_END);
    return rc;
}
static int tracked_flush(void *)
{ int rc=storage_hooks.flush(storage_hooks.context);if(!rc) event(OTA_EVENT_FLASH_COMPLETE);return rc; }
static int tracked_validate(void *,const ota_manifest *m)
{
    event(OTA_EVENT_VERIFY_BEGIN);publish();
    int rc=storage_hooks.validate(storage_hooks.context,m);if(!rc) event(OTA_EVENT_VERIFY_END);return rc;
}
static int tracked_journal(void *,const ota_manifest *m,ota_state state,uint32_t commit)
{
    if(state==OTA_COMMITTED) event(OTA_EVENT_ALL_VALIDATED);
    if(state==OTA_REBOOTING) event(OTA_EVENT_REBOOTING);
    return storage_hooks.journal(storage_hooks.context,m,state,commit);
}
static void process(Work &w)
{
    int rc=0;
    ota_participant_note_loss(&participant,atomic_set(&losses,0));
    if(atomic_get(&identity_conflict)) {
        service_error=OTA_ERR_IDENTITY;ota_safety_restore(true);publish();return;
    }
    switch(w.type) {
    case REFRESH: atomic_clear(&refresh_pending);break;
    case COMMAND:
        if(w.command.type==OTA_CMD_PREPARE) rc=ota_storage_expect_lead(w.command.lead_eui);
        if(!rc) rc=ota_participant_command(&participant,&w.command);
        if(!rc && w.command.type==OTA_CMD_BEGIN_PASS) event(OTA_EVENT_CAN_TRANSFER_BEGIN);
        if(!rc && w.command.type==OTA_CMD_END_PASS && participant.status.offset==participant.status.image_size)
            event(OTA_EVENT_CAN_TRANSFER_END);
        if(rc==OTA_ERR_CONFLICT || rc==OTA_ERR_STATE) {participant.status.rx_rejected++;rc=0;}
        break;
    case REPORT: (void)ota_participant_report(&participant,w.source,w.data,w.length,false);break;
    case RELEASE: {
        /* Recheck durable campaign identity in worker order, after all writes. */
        ota_storage_journal j{};
        rc=ota_storage_get_journal(&j);
        if(!rc && (j.campaign_id!=participant.status.campaign_id || j.commit_id!=participant.status.commit_id ||
           memcmp(j.lead_eui,lead_eui,8) || memcmp(j.mcuboot_image_hash,w.data,32))) rc=OTA_ERR_IDENTITY;
        if(!rc) rc=ota_storage_release_maintenance(w.data);
        if(!rc) {participant.status.state=OTA_SUCCEEDED;atomic_clear(&busy);accepted_campaign=0;}
        atomic_clear(&release_pending);break;
    }
    }
    if(rc<0) service_error=rc;
    publish();
}
static void initialize_runtime()
{
    int rc=ota_storage_init();
    ota_identity id{};memcpy(id.eui,eui64,8);id.protocol_version=OTA_PROTOCOL_VERSION;
    ota_storage_boot_identity(&id);ota_storage_hooks(&storage_hooks);
    auto tracked=storage_hooks;tracked.prepare=tracked_prepare;tracked.flush=tracked_flush;
    tracked.validate=tracked_validate;tracked.journal=tracked_journal;
    ota_participant_init(&participant,&id,&tracked,ota_storage_recovery_required());
    if(rc) service_error=OTA_ERR_JOURNAL;
    if(!service_error && ota_storage_active_hash(local_snapshot.active_mcuboot_image_hash)) service_error=OTA_ERR_HEALTH;
    ota_storage_journal j{};int jr=ota_storage_get_journal(&j);
    if(jr) service_error=OTA_ERR_JOURNAL;
    bool strict_boot=j.campaign_id || ota_storage_maintenance() || ota_storage_recovery_required();
    if(!jr && j.campaign_id) {
        participant.status.campaign_id=j.campaign_id;participant.status.commit_id=j.commit_id;
        participant.status.image_size=j.image_size;
        participant.manifest.campaign_id=j.campaign_id;participant.manifest.image_size=j.image_size;
        memcpy(participant.manifest.version,j.version,32);memcpy(participant.manifest.build_id,j.build_id,32);
        memcpy(participant.manifest.mcuboot_image_hash,j.mcuboot_image_hash,32);
        memcpy(participant.manifest.artifact_sha256,j.artifact_sha256,32);
        memcpy(participant.lead_eui,j.lead_eui,8);memcpy(lead_eui,j.lead_eui,8);lead_bound=true;
        if(j.state==OTA_SUCCEEDED && !ota_storage_maintenance()) participant.status.state=OTA_SUCCEEDED;
        if(j.state==OTA_VALID || j.state==OTA_COMMITTED || j.state==OTA_REBOOTING || j.state==OTA_SUCCEEDED) {
            participant.status.offset=j.image_size;participant.status.flash_complete=true;participant.status.validated=true;
        }
        if(memcmp(j.mcuboot_image_hash,local_snapshot.active_mcuboot_image_hash,32) ||
           strncmp(j.version,OWNTECH_FIRMWARE_VERSION,32) || strncmp(j.build_id,OWNTECH_FIRMWARE_BUILD_ID,32))
            local_snapshot.rolled_back=true;
        if(local_snapshot.rolled_back || (!boot_is_img_confirmed() &&
           j.state!=OTA_COMMITTED && j.state!=OTA_REBOOTING && j.state!=OTA_COMMIT_INTENT && j.state!=OTA_SUCCEEDED)) service_error=OTA_ERR_HEALTH;
    }
    else if(strict_boot) service_error=OTA_ERR_JOURNAL;
    auto can=thingset_can_get_inst();
    /* Install claim/report callbacks before claiming finishes: no lost first
     * Lead announcement and no client/discovery machinery in this variant. */
    if(ota_network_init()) service_error=OTA_ERR_TRANSPORT;
    thingset_can_set_state_callback(can_state_changed,nullptr);
    uint64_t deadline=k_uptime_get()+CONFIG_OWNTECH_OTA_HEALTH_TIMEOUT_MS;
    while(!service_error && !atomic_get(&can->driver_started) && !atomic_get(&can->init_error) &&
          k_uptime_get()<(int64_t)deadline) k_sleep(K_MSEC(20));
    if(!service_error && (!atomic_get(&can->driver_started) || atomic_get(&can->init_error))) service_error=OTA_ERR_TRANSPORT;
    if(!service_error && owntech_ota_check_health()) service_error=OTA_ERR_HEALTH;
    if(!service_error) atomic_set(&local_healthy,1);
    if(!service_error && strict_boot) event(OTA_EVENT_POSTBOOT_CHECK);
    /* A controller that started is sufficient for local confirmation; no CAN
     * ACK is required. Campaign maintenance remains until collective release. */
    if(!service_error && !boot_is_img_confirmed() && boot_write_img_confirmed()) service_error=OTA_ERR_HEALTH;
    if(!service_error) {
        if(atomic_get(&can->ready)) {atomic_set(&can_ready,1);atomic_set(&healthy,1);}
        else waiting_can=true;
        if(!ota_storage_maintenance()) ota_safety_restore(false);
    }
    atomic_set(&initialized,1);publish();
}
static void worker_iteration()
{
    Work w{};
    if(!k_msgq_get(&ota_queue,&w,K_FOREVER)) process(w);
}
static void run(void *,void *,void *)
{ initialize_runtime();for(;;) worker_iteration(); }
K_THREAD_DEFINE(ota_worker,CONFIG_OWNTECH_OTA_STACK_SIZE,run,nullptr,nullptr,nullptr,7,0,0);
