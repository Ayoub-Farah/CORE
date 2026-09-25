/* The production transport without any client context or response timer. */
#define OWNTECH_RECEIVER_TRANSPORT_TEST 1
#include "sdk_shim/sdk_host.h"
#include "../../third_party/thingset-zephyr-sdk/src/can.c"
uint8_t eui64[8]={1,2,3,4,5,6,7,8};
static void forbidden_callback(uint8_t *p,size_t n,int tx,int rx,uint8_t src,void *arg)
{ assert(false); }
int main(void)
{
    struct device dev={"receiver"};struct thingset_can can={0};
    can.dev=&dev;can.node_addr=1;host_run_work=true;host_filter_fail_at=-1;
    assert(thingset_can_init_inst(&can,&dev,0,K_MSEC(1000))==0);
    uint8_t response[]={0x85,0xf6,0x01};
    assert(thingset_can_send_inst(&can,response,sizeof(response),2,0,
           forbidden_callback,NULL,K_MSEC(100))==-ENOTSUP);
    assert(thingset_can_send_inst(&can,response,sizeof(response),2,0,NULL,NULL,K_NO_WAIT)==0);
    assert(can.server_tx.sem.count==1);
    host_isotp_defer=true;
    assert(thingset_can_send_inst(&can,response,sizeof(response),2,0,NULL,NULL,K_NO_WAIT)==0);
    response[0]=0;
    assert(host_isotp_data[0]==0x85); /* Owned reply remains immutable. */
    assert(thingset_can_send_inst(&can,response,sizeof(response),2,0,NULL,NULL,K_NO_WAIT)==-EBUSY);
    thingset_can_reqresp_sent_callback(ISOTP_N_OK,host_isotp_arg);
    assert(can.server_tx.sem.count==1);
    host_isotp_defer=false;
    uint8_t late[]={0x85,0xf6};struct net_buf reply={late,sizeof(late),NULL};
    thingset_can_reqresp_recv_callback(&reply,0,(struct isotp_fast_addr){2},&can);
    assert(host_process_count==0); /* No client means unsolicited replies are dropped. */
    uint8_t request[]={1,2};struct net_buf req={request,sizeof(request),NULL};
    thingset_can_reqresp_recv_callback(&req,0,(struct isotp_fast_addr){2},&can);
    assert(host_process_count==1 && host_shared.lock.count==1 && can.server_tx.sem.count==1);
    assert(!can.timeout_timer.active); /* No periodic receiver polling. */
    puts("ThingSet receiver: client absent, server ownership and idle timers OK");
    return 0;
}
