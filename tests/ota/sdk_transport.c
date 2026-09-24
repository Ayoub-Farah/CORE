/* Compile the production public API and drive its real callbacks deterministically. */
#include "sdk_shim/sdk_host.h"
#include "../../third_party/thingset-zephyr-sdk/src/can.c"

uint8_t eui64[8]={1,2,3,4,5,6,7,8};
static struct device dev={"host CAN"};
static struct thingset_can instance;
static int completions,last_send,last_receive,claims,reports;
static uint8_t received[1024];
static size_t received_len;

static void completed(uint8_t *data,size_t len,int tx,int rx,uint8_t source,void *arg)
{
    assert(source==2);assert(arg==&instance);completions++;last_send=tx;last_receive=rx;
    if(data){assert(len<=sizeof(received));memcpy(received,data,len);received_len=len;}
}
static void report(const uint8_t *data,size_t len,uint8_t source)
{
    assert(len<=sizeof(received));reports++;memcpy(received,data,len);received_len=len;
}
static void claim(const uint8_t id[8],uint8_t source){claims++;assert(source==2);assert(id[0]==1);}
static void setup(void)
{
    memset(&instance,0,sizeof(instance));memset(rx_slots,0,sizeof(rx_slots));
    instance.dev=&dev;instance.node_addr=1;instance.report_rx_cb=report;
    instance.ctx.sent_callback=thingset_can_reqresp_sent_callback;
    instance.client_tx.owner=&instance;instance.client_tx.client=true;
    instance.server_tx.owner=&instance;
    k_sem_init(&instance.client_tx.sem,1,1);k_sem_init(&instance.server_tx.sem,1,1);
    k_sem_init(&instance.request_response.sem,1,1);k_sem_init(&instance.report_tx_sem,0,1);
    k_timer_init(&instance.request_response.timer,thingset_can_reqresp_timeout_handler,NULL);
    k_timer_init(&thingset_can_report_expiry_timer,thingset_can_report_expiry,NULL);
    rx_expiry_deadline=0;
    atomic_set(&instance.ready,1);host_shared.lock.count=1;
    host_now=0;host_isr=false;host_can_error=0;host_can_async_error=0;host_can_defer=false;
    host_isotp_error=0;host_isotp_defer=false;host_frame_count=0;
    host_filter_count=0;host_process_count=0;completions=0;claims=0;reports=0;
    received_len=0;last_send=last_receive=0;
    host_process_hook=NULL;host_thread=(void *)1;
}
static uint32_t response_id(void){return THINGSET_CAN_PRIO_REQRESP|THINGSET_CAN_TARGET_SET(1)|2;}
static int request(uint8_t *bytes,size_t n){return thingset_can_send_inst(&instance,bytes,n,2,0,completed,&instance,K_MSEC(100));}
static void receive(const uint8_t *data,size_t n,uint32_t id)
{
    struct net_buf b={(uint8_t *)data,n,NULL};
    thingset_can_reqresp_recv_callback(&b,0,(struct isotp_fast_addr){id},&instance);
}
static void fragment(uint8_t src,uint8_t msg,uint32_t type,uint8_t seq,size_t n,uint8_t byte)
{
    struct can_frame frame={.id=THINGSET_CAN_PRIO_REPORT_LOW|THINGSET_CAN_TYPE_MF_REPORT
        |THINGSET_CAN_MSG_NO_SET(msg)|type|THINGSET_CAN_SEQ_NO_SET(seq)|src,
        .dlc=can_bytes_to_dlc(n),.flags=CAN_FRAME_IDE};
    memset(frame.data,byte,sizeof(frame.data));
    thingset_can_report_rx_cb(&dev,&frame,&instance);
}
static void test_client(void)
{
    uint8_t req[80]={1,2,3},response[]={0x85,0xF6};
    setup();assert(request(req,3)==0);assert(completions==0);
    assert(instance.request_response.timer.active);assert(instance.client_tx.sem.count==1);
    receive(response,2,response_id()^1);assert(completions==0);assert(host_process_count==0);
    receive(response,2,response_id()|THINGSET_CAN_SOURCE_BUS_SET(1));assert(completions==0);
    host_now=10;receive(response,2,response_id());assert(completions==1);
    assert(last_send==0&&last_receive==0&&received_len==2);
    thingset_can_reqresp_timeout_handler(&instance.request_response.timer);assert(completions==1);
    assert(instance.request_response.sem.count==1);assert(host_shared.lock.count==1);

    setup();host_isotp_defer=true;assert(request(req,sizeof(req))==0);
    receive(response,2,response_id());assert(completions==1);
    assert(instance.client_tx.sem.count==0);
    host_isotp_ctx->sent_callback(ISOTP_N_ERROR,host_isotp_arg);
    assert(completions==1&&instance.client_tx.sem.count==1);

    setup();host_isotp_defer=true;assert(request(req,sizeof(req))==0);
    assert(host_isotp_data!=req);req[0]=99;assert(host_isotp_data[0]==1);
    host_now=100;thingset_can_reqresp_timeout_handler(&instance.request_response.timer);
    assert(completions==1&&last_receive==-ETIMEDOUT);
    assert(instance.client_tx.sem.count==0); /* Timeout cannot recycle ISO-TP bytes. */
    assert(request(req,sizeof(req))==-EBUSY);assert(completions==1);
    host_isotp_ctx->sent_callback(0,host_isotp_arg);assert(completions==1);
    assert(instance.client_tx.sem.count==1);
    host_isotp_defer=false;assert(request(req,3)==0);
    thingset_can_reqresp_timeout_handler(&instance.request_response.timer);assert(completions==1);
    receive(response,2,response_id());assert(completions==2);

    setup();host_isotp_error=ISOTP_N_TIMEOUT_A;assert(request(req,3)==-ETIMEDOUT);
    assert(completions==1&&last_send==-ETIMEDOUT&&last_receive==0);
    assert(instance.client_tx.sem.count==1&&instance.request_response.sem.count==1);
    setup();host_isotp_error=ISOTP_NO_NET_BUF_LEFT;host_isotp_defer=true;
    assert(request(req,sizeof(req))==-ENOBUFS);assert(completions==1);
    assert(instance.client_tx.sem.count==1);

    setup();assert(request(req,3)==0);
    thingset_can_reqresp_recv_error_callback(ISOTP_N_TIMEOUT_CR,
        (struct isotp_fast_addr){response_id()},&instance);
    assert(completions==1&&last_receive==-ETIMEDOUT);
    receive(response,2,response_id());assert(completions==1);

    setup();assert(request(req,3)==0);uint8_t oversized[601]={0x85};
    receive(oversized,sizeof(oversized),response_id());
    assert(completions==1&&last_receive==-EMSGSIZE&&received_len==0);
    assert(request(oversized,sizeof(oversized))==-EMSGSIZE);
}
static void inspect_request_peer(void)
{
    uint8_t source=0,route=99;
    assert(thingset_can_get_request_source_inst(&instance,&source,&route)==0);
    assert(source==2&&route==0);
    host_thread=(void *)2;
    assert(thingset_can_get_request_source_inst(&instance,&source,&route)==-ENOENT);
    host_thread=(void *)1;host_isr=true;
    assert(thingset_can_get_request_source_inst(&instance,&source,&route)==-ENOENT);
    host_isr=false;
}
static void test_server_ownership(void)
{
    uint8_t req[]={1,2,3};setup();host_isotp_defer=true;
    uint8_t source,route;
    assert(thingset_can_get_request_source_inst(&instance,&source,&route)==-ENOENT);
    host_process_hook=inspect_request_peer;
    receive(req,sizeof(req),response_id());assert(host_process_count==1);
    assert(thingset_can_get_request_source_inst(&instance,&source,&route)==-ENOENT);
    assert(host_shared.lock.count==1&&instance.server_tx.sem.count==0);
    assert(host_isotp_data!=host_shared.data&&host_isotp_data[0]==0x85);
    memset(host_shared.data,0x11,20);assert(host_isotp_data[0]==0x85);
    host_isotp_ctx->sent_callback(0,host_isotp_arg);
    assert(instance.server_tx.sem.count==1&&host_shared.lock.count==1);
}
static void test_reports(void)
{
    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    fragment(2,1,THINGSET_CAN_MF_TYPE_FIRST,0,8,2);
    fragment(2,1,THINGSET_CAN_MF_TYPE_LAST,1,3,3);
    assert(reports==1&&received_len==11&&received[0]==2&&received[8]==3);
    fragment(2,2,THINGSET_CAN_MF_TYPE_LAST,0,8,4);assert(reports==1);
    fragment(2,2,THINGSET_CAN_MF_TYPE_FIRST,0,8,4);
    fragment(2,2,THINGSET_CAN_MF_TYPE_LAST,2,8,4);assert(reports==1);
    fragment(2,2,THINGSET_CAN_MF_TYPE_SINGLE,1,8,4);assert(reports==1);
    fragment(2,2,THINGSET_CAN_MF_TYPE_SINGLE,0,8,4);assert(reports==2);

    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    fragment(3,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    fragment(4,0,THINGSET_CAN_MF_TYPE_SINGLE,0,8,1);assert(reports==0);
    host_advance_timer(&thingset_can_report_expiry_timer,51);
    assert(instance.report_rx_expired==2);
    fragment(4,0,THINGSET_CAN_MF_TYPE_SINGLE,0,8,1);assert(reports==1);
    fragment(2,0,THINGSET_CAN_MF_TYPE_LAST,1,8,1);assert(reports==1);

    setup();for(unsigned i=0;i<65;i++)fragment(2,0,i==0?THINGSET_CAN_MF_TYPE_FIRST:
        (i==64?THINGSET_CAN_MF_TYPE_LAST:THINGSET_CAN_MF_TYPE_CONSEC),i&15,8,(uint8_t)i);
    assert(reports==0&&instance.report_rx_overflow==1);
    fragment(2,3,THINGSET_CAN_MF_TYPE_SINGLE,0,8,5);assert(reports==1);
    assert(thingset_can_set_report_rx_callback_inst(&instance,report)==0);
    assert(thingset_can_set_report_rx_callback_inst(&instance,report)==0);
    assert(host_filter_count==1);
}
static void test_report_timer_lifecycle(void)
{
    struct k_timer *timer=&thingset_can_report_expiry_timer;
    setup();assert(!timer->active);
    host_advance_timer(timer,1000);assert(timer->fires==0&&timer->starts==0);
    fragment(2,0,THINGSET_CAN_MF_TYPE_SINGLE,0,8,1);
    assert(reports==1&&!timer->active&&timer->starts==0);

    /* Earliest of independently arriving reports expires, then the next one.
     * A later arrival must not postpone the earlier deadline. */
    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    assert(timer->active&&timer->period==0&&timer->deadline==50);
    host_advance_timer(timer,20);
    fragment(3,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,2);
    assert(timer->deadline==50&&timer->starts==1);
    host_advance_timer(timer,49);assert(instance.report_rx_expired==0);
    host_advance_timer(timer,50);
    assert(instance.report_rx_expired==1&&timer->active&&timer->deadline==70);
    host_advance_timer(timer,70);
    assert(instance.report_rx_expired==2&&!timer->active&&rx_expiry_deadline==0);
    unsigned fires=timer->fires;
    host_advance_timer(timer,10000);assert(timer->fires==fires);

    /* A continuation extends only its own timeout; the other slot becomes
     * the earliest. Completing the last report cancels the final timeout. */
    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    host_advance_timer(timer,10);fragment(3,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,2);
    host_advance_timer(timer,20);fragment(2,0,THINGSET_CAN_MF_TYPE_CONSEC,1,8,1);
    assert(timer->active&&timer->deadline==60);
    host_advance_timer(timer,60);
    assert(instance.report_rx_expired==1&&timer->active&&timer->deadline==70);
    host_advance_timer(timer,69);fragment(2,0,THINGSET_CAN_MF_TYPE_LAST,2,8,1);
    assert(reports==1&&!timer->active&&rx_expiry_deadline==0);
    fires=timer->fires;host_advance_timer(timer,10000);assert(timer->fires==fires);

    /* Reset-by-FIRST refreshes a stalled report. Out-of-order and overflow
     * discard the last incomplete slot and must also restore idle. */
    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    host_advance_timer(timer,40);fragment(2,1,THINGSET_CAN_MF_TYPE_FIRST,0,8,2);
    assert(timer->deadline==90);
    host_advance_timer(timer,50);assert(instance.report_rx_expired==0);
    fragment(2,1,THINGSET_CAN_MF_TYPE_LAST,2,8,2);
    assert(!timer->active&&rx_expiry_deadline==0&&instance.report_rx_dropped==1);
    fragment(2,2,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    assert(timer->active);
    for(unsigned i=1;i<65;i++)
        fragment(2,2,THINGSET_CAN_MF_TYPE_CONSEC,i&15,8,1);
    assert(instance.report_rx_overflow==1&&!timer->active&&rx_expiry_deadline==0);
    fires=timer->fires;host_advance_timer(timer,10000);assert(timer->fires==fires);

    /* RX can observe an elapsed deadline before the timer interrupt runs.
     * Dropping that late continuation must cancel the stale pending timer. */
    setup();fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    host_now=50;fragment(2,0,THINGSET_CAN_MF_TYPE_LAST,1,8,1);
    assert(instance.report_rx_expired==1&&instance.report_rx_dropped==1);
    assert(!timer->active&&rx_expiry_deadline==0);
    host_advance_timer(timer,10000);assert(timer->fires==0);

    /* Shared timer remains armed for another CAN instance's pending report. */
    setup();struct thingset_can other={0};other.dev=&dev;other.node_addr=1;
    fragment(2,0,THINGSET_CAN_MF_TYPE_FIRST,0,8,1);
    host_advance_timer(timer,10);
    struct can_frame frame={.id=THINGSET_CAN_PRIO_REPORT_LOW|THINGSET_CAN_TYPE_MF_REPORT
        |THINGSET_CAN_MF_TYPE_FIRST|3,.dlc=8,.flags=CAN_FRAME_IDE};
    thingset_can_report_rx_cb(&dev,&frame,&other);
    fragment(2,0,THINGSET_CAN_MF_TYPE_LAST,1,8,1);
    assert(timer->active&&timer->deadline==60);
    host_advance_timer(timer,60);
    assert(instance.report_rx_expired==0&&other.report_rx_expired==1&&!timer->active);
}
static void test_sender_and_discovery(void)
{
    uint8_t data[290];memset(data,0x37,sizeof(data));setup();
    assert(thingset_can_send_raw_report_inst(&instance,data,sizeof(data),K_MSEC(1000))==0);
    assert(host_frame_count==(sizeof(data)+CAN_MAX_DLEN-1)/CAN_MAX_DLEN);
    size_t pos=0;
    for(size_t i=0;i<host_frame_count;i++){
        struct can_frame *f=&host_frames[i];
        assert(THINGSET_CAN_SEQ_NO_GET(f->id)==(i&15));
        size_t n=MIN(sizeof(data)-pos,CAN_MAX_DLEN);
        assert(memcmp(f->data,data+pos,n)==0);pos+=n;
        for(size_t j=n;j<can_dlc_to_bytes(f->dlc);j++)assert(f->data[j]==0);
#ifdef CONFIG_CAN_FD_MODE
        assert((f->flags&(CAN_FRAME_FDF|CAN_FRAME_BRS))==(CAN_FRAME_FDF|CAN_FRAME_BRS));
#endif
    }
    assert((host_frames[0].id&THINGSET_CAN_MF_TYPE_MASK)==THINGSET_CAN_MF_TYPE_FIRST);
    assert((host_frames[host_frame_count-1].id&THINGSET_CAN_MF_TYPE_MASK)==THINGSET_CAN_MF_TYPE_LAST);
    setup();host_can_async_error=-EIO;
    assert(thingset_can_send_raw_report_inst(&instance,data,20,K_MSEC(100))==-EIO);
    setup();host_can_defer=true;
    assert(thingset_can_send_raw_report_inst(&instance,data,20,K_MSEC(10))==-ETIMEDOUT);
    assert(thingset_can_send_raw_report_inst(&instance,data,20,K_MSEC(10))==-EBUSY);
    host_pending_cb(&dev,0,host_pending_arg);host_can_defer=false;
    assert(thingset_can_send_raw_report_inst(&instance,data,20,K_MSEC(100))==0);
    setup();assert(thingset_can_send_report_inst(&instance,"test",THINGSET_BIN_IDS_VALUES)==0);
    assert(host_shared.lock.count==1);
    assert(thingset_can_probe_address_inst(&instance,2,K_MSEC(100))==0);
    struct can_frame *probe=&host_frames[host_frame_count-1];
    assert(probe->dlc==0&&THINGSET_CAN_TARGET_GET(probe->id)==2);
    assert(THINGSET_CAN_SOURCE_GET(probe->id)==THINGSET_CAN_ADDR_ANONYMOUS);
    assert(thingset_can_announce_address_inst(&instance,K_MSEC(100))==0);
    struct can_frame *announcement=&host_frames[host_frame_count-1];
    assert(announcement->dlc==8&&THINGSET_CAN_TARGET_GET(announcement->id)==255);
    assert(THINGSET_CAN_SOURCE_GET(announcement->id)==1);
    assert(memcmp(announcement->data,eui64,8)==0);
    assert(thingset_can_probe_address_inst(&instance,0,K_MSEC(100))==-EINVAL);
    thingset_can_set_addr_claim_rx_callback_inst(&instance,claim);
    struct can_frame frame={.id=THINGSET_CAN_TYPE_NETWORK|THINGSET_CAN_TARGET_SET(255)|2,
        .flags=CAN_FRAME_IDE,.dlc=7,.data={1,2,3,4,5,6,7,8}};
    thingset_can_addr_claim_rx_cb(&dev,&frame,&instance);assert(claims==0);
    frame.dlc=8;thingset_can_addr_claim_rx_cb(&dev,&frame,&instance);assert(claims==1);
}
int main(void)
{
    test_client();test_server_ownership();test_reports();test_report_timer_lifecycle();test_sender_and_discovery();
    puts("ThingSet public transport: client lifecycle, RX, raw sender and discovery OK");
    return 0;
}
