/* Deterministic host substitutes for kernel/device boundaries, not transport logic. */
#ifndef SDK_HOST_H
#define SDK_HOST_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
/* Windows CRT assert can open a blocking GUI dialog in unattended CI. */
#undef assert
#define assert(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#ifndef OWNTECH_RECEIVER_TRANSPORT_TEST
#define CONFIG_THINGSET_CAN_CLIENT 1
#endif
#define CONFIG_THINGSET_CAN_MULTIPLE_INSTANCES 1
#define CONFIG_THINGSET_CAN_REPORT_RX 1
#define CONFIG_THINGSET_CAN_ROUTING_BUSES 1
#define CONFIG_THINGSET_CAN_TX_BUF_SIZE 600
#define CONFIG_THINGSET_CAN_RX_BUF_SIZE 600
#define CONFIG_THINGSET_CAN_REPORT_RX_BUFFER_SIZE 512
#define CONFIG_THINGSET_CAN_REPORT_RX_NUM_BUFFERS 2
#define CONFIG_THINGSET_CAN_REPORT_RX_TIMEOUT 50
#define CONFIG_THINGSET_CAN_REPORT_MAX_SIZE 1024
#define CONFIG_THINGSET_CAN_REPORT_TIMEOUT 1000
#define CONFIG_THINGSET_CAN_FRAME_SEPARATION_TIME 1
#define CONFIG_THINGSET_CAN_STORAGE 0
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...)
#define LOG_WRN(...)
#define LOG_INF(...)
#define LOG_DBG(...)
#define BIT(x) (1U << (x))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define HOST_ENABLED_1 ,1,
#define HOST_SECOND(a,b,...) b
#define HOST_ENABLED_I(...) HOST_SECOND(__VA_ARGS__,0,)
#define HOST_ENABLED(x) HOST_ENABLED_I(HOST_ENABLED_##x)
#define IS_ENABLED(x) HOST_ENABLED(x)
typedef struct { int64_t ticks; } k_timeout_t;
typedef int64_t k_timepoint_t;
#define K_MSEC(x) ((k_timeout_t){(x)})
#define K_TICKS(x) K_MSEC(x)
#define K_NO_WAIT K_MSEC(0)
#define K_FOREVER K_MSEC(-1)
#define K_TIMEOUT_EQ(a,b) ((a).ticks==(b).ticks)
static int64_t host_now;
static bool host_isr;
typedef void *k_tid_t;
static k_tid_t host_thread=(void *)1;
static k_tid_t k_current_get(void) { return host_thread; }
static int64_t k_uptime_get(void) { return host_now; }
static bool k_is_in_isr(void) { return host_isr; }
static k_timepoint_t sys_timepoint_calc(k_timeout_t t) {return host_now+t.ticks;}
static bool sys_timepoint_expired(k_timepoint_t t) {return host_now>=t;}
static k_timeout_t sys_timepoint_timeout(k_timepoint_t t) {return K_MSEC(t>host_now?t-host_now:0);}
static int64_t k_ticks_to_ms_ceil64(int64_t t) {return t;}
static void k_sleep(k_timeout_t t) { host_now+=t.ticks; }
struct k_sem {int count; int limit;};
static void k_sem_init(struct k_sem *s,int n,int max) {s->count=n;s->limit=max;}
static int k_sem_take(struct k_sem *s,k_timeout_t t) {if(s->count){s->count--;return 0;} if(t.ticks>0)host_now+=t.ticks;return -EAGAIN;}
static void k_sem_give(struct k_sem *s) {assert(s->count<s->limit);s->count++;}
static void k_sem_reset(struct k_sem *s) {s->count=0;}
struct k_mutex {bool held;};
static void k_mutex_init(struct k_mutex *m) {m->held=false;}
static int k_mutex_lock(struct k_mutex *m,k_timeout_t t) {if(m->held)return -EBUSY;m->held=true;return 0;}
static void k_mutex_unlock(struct k_mutex *m) {assert(m->held);m->held=false;}
struct k_spinlock {bool held;};
typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock *l){assert(!l->held);l->held=true;return 0;}
static void k_spin_unlock(struct k_spinlock *l,int key){assert(l->held);l->held=false;}
typedef int atomic_t;
static int atomic_get(atomic_t *v){return *v;}
static void atomic_set(atomic_t *v,int n){*v=n;}
static void atomic_clear(atomic_t *v){*v=0;}
static void atomic_inc(atomic_t *v){(*v)++;}
struct k_timer {
    void (*handler)(struct k_timer *);
    bool active;
    int64_t deadline,period;
    unsigned starts,stops,fires;
};
static void k_timer_init(struct k_timer *t,void (*fn)(struct k_timer *),void *unused){
    memset(t,0,sizeof(*t));t->handler=fn;
}
static void k_timer_start(struct k_timer *t,k_timeout_t a,k_timeout_t b){
    t->active=a.ticks>=0;t->deadline=host_now+a.ticks;t->period=b.ticks;t->starts++;
}
static void k_timer_stop(struct k_timer *t){t->active=false;t->stops++;}
/* Fire elapsed timers as Zephyr does: one-shot is inactive before its callback.
 * The callback may rearm it. No production transport code is replaced here.
 */
static void host_advance_timer(struct k_timer *t,int64_t until){
    assert(until>=host_now);
    unsigned count=0;
    while(t->active&&t->deadline<=until){
        assert(count++<1000);host_now=t->deadline;
        t->active=t->period>0;if(t->active)t->deadline+=t->period;
        t->fires++;t->handler(t);
    }
    host_now=until;
}
#define K_TIMER_DEFINE(n,fn,unused) struct k_timer n={.handler=fn}
struct k_work {int unused;};
struct k_work_delayable {struct k_work work;void (*handler)(struct k_work *);};
static bool host_run_work;
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *w){return (struct k_work_delayable *)w;}
static void k_work_init_delayable(struct k_work_delayable *w,void (*fn)(struct k_work *)){w->handler=fn;}
static void thingset_sdk_reschedule_work(struct k_work_delayable *w,k_timeout_t t){if(host_run_work&&w->handler)w->handler(&w->work);}
struct k_event {uint32_t flags;};
static void (*host_event_wait_hook)(struct k_event *);
static void k_event_init(struct k_event *e){e->flags=0;}
static void k_event_post(struct k_event *e,uint32_t f){e->flags|=f;}
static void k_event_set(struct k_event *e,uint32_t f){e->flags=f;}
static void k_event_clear(struct k_event *e,uint32_t f){e->flags&=~f;}
static uint32_t k_event_test(struct k_event *e,uint32_t f){return e->flags&f;}
static uint32_t k_event_wait(struct k_event *e,uint32_t f,bool reset,k_timeout_t t){if(host_event_wait_hook)host_event_wait_hook(e);return e->flags&f;}
struct device {const char *name;};
static bool device_is_ready(const struct device *d){return d!=NULL;}
#ifdef CONFIG_CAN_FD_MODE
#define CAN_MAX_DLEN 64
#else
#define CAN_MAX_DLEN 8
#endif
#define CAN_FRAME_IDE 1
#define CAN_FRAME_RTR 2
#define CAN_FRAME_FDF 4
#define CAN_FRAME_BRS 8
#define CAN_FILTER_IDE 1
#define CAN_MODE_FD 1
typedef int can_mode_t;
struct can_filter {uint32_t id,mask;int flags;};
struct can_frame {uint32_t id;uint8_t dlc,flags;uint8_t data[CAN_MAX_DLEN];};
struct can_bus_err_cnt {int tx_err_cnt;};
static uint8_t can_dlc_to_bytes(uint8_t dlc){const uint8_t n[]={0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};return dlc<16?n[dlc]:0;}
static uint8_t can_bytes_to_dlc(size_t n){for(uint8_t i=0;i<16;i++)if(can_dlc_to_bytes(i)>=n)return i;return 15;}
typedef void (*host_can_cb)(const struct device *,int,void *);
static int host_can_error,host_can_async_error,host_filter_count;
static int host_can_start_error,host_can_start_count,host_can_mode_error,host_filter_fail_at;
static bool host_can_defer;
static host_can_cb host_pending_cb;
static void *host_pending_arg;
static struct can_frame host_frames[200];
static size_t host_frame_count;
static int can_send(const struct device *dev,const struct can_frame *frame,k_timeout_t t,host_can_cb cb,void *arg){
 if(host_can_error)return host_can_error;
 assert(host_frame_count<ARRAY_SIZE(host_frames));host_frames[host_frame_count++]=*frame;
 if(cb){if(host_can_defer){host_pending_cb=cb;host_pending_arg=arg;}else cb(dev,host_can_async_error,arg);}return 0;
}
static int can_add_rx_filter(const struct device *d,void (*fn)(const struct device *,struct can_frame *,void *),void *arg,const struct can_filter *f){int n=host_filter_count++;return n==host_filter_fail_at?-ENOSPC:n;}
static void can_remove_rx_filter(const struct device *d,int id){}
static int can_get_capabilities(const struct device *d,can_mode_t *m){*m=CAN_MODE_FD;return 0;}
static int can_set_mode(const struct device *d,can_mode_t m){return host_can_mode_error;}
static int can_start(const struct device *d){host_can_start_count++;return host_can_start_error;}
static int can_get_state(const struct device *d,void *state,struct can_bus_err_cnt *e){e->tx_err_cnt=0;return 0;}
static uint32_t sys_rand32_get(void){return 42;}
#define ISOTP_FAST_ADDRESSING_MODE_CUSTOM 1
#define ISOTP_MSG_FDF 1
#define ISOTP_N_OK 0
#define ISOTP_N_TIMEOUT_A -1
#define ISOTP_N_TIMEOUT_BS -2
#define ISOTP_N_TIMEOUT_CR -3
#define ISOTP_N_BUFFER_OVERFLW -8
#define ISOTP_N_ERROR -9
#define ISOTP_NO_NET_BUF_LEFT -11
#define ISOTP_NO_CTX_LEFT -13
struct isotp_fast_addr {uint32_t ext_id;};
struct isotp_fast_opts {int bs,stmin,addressing_mode,flags;};
struct net_buf {uint8_t *data;size_t len;struct net_buf *frags;};
struct isotp_fast_ctx {struct isotp_fast_addr (*get_tx_addr_callback)(const struct isotp_fast_addr *);void (*sent_callback)(int,void *);};
static size_t net_buf_frags_len(struct net_buf *b){size_t n=0;for(;b;b=b->frags)n+=b->len;return n;}
static size_t net_buf_linearize(void *dst,size_t cap,struct net_buf *b,size_t offset,size_t length){size_t n=0;for(;b&&n<cap&&n<length;b=b->frags){size_t k=MIN(b->len,MIN(cap-n,length-n));memcpy((uint8_t*)dst+n,b->data,k);n+=k;}return n;}
static int host_isotp_error,host_isotp_bind_error;
static bool host_isotp_defer;
static const uint8_t *host_isotp_data;
static void *host_isotp_arg;
static struct isotp_fast_ctx *host_isotp_ctx;
static int isotp_fast_send(struct isotp_fast_ctx *ctx,const uint8_t *buf,size_t n,struct isotp_fast_addr addr,void *arg){host_isotp_data=buf;host_isotp_arg=arg;host_isotp_ctx=ctx;if(!host_isotp_defer)ctx->sent_callback(host_isotp_error,arg);return host_isotp_error;}
static int isotp_fast_bind(struct isotp_fast_ctx *ctx,const struct device *d,struct isotp_fast_addr a,const struct isotp_fast_opts *o,void (*rx)(struct net_buf *,int,struct isotp_fast_addr,void *),void *arg,void (*err)(int8_t,struct isotp_fast_addr,void *),void (*tx)(int,void *)){ctx->sent_callback=tx;return host_isotp_bind_error;}
enum thingset_data_format {THINGSET_BIN_IDS_VALUES};
struct shared_buffer {struct k_sem lock;uint8_t *data;size_t size;};
static uint8_t host_shared_data[600];
static struct shared_buffer host_shared={.lock={1,1},.data=host_shared_data,.size=sizeof(host_shared_data)};
static struct shared_buffer *thingset_sdk_shared_buffer(void){return &host_shared;}
static int ts;
static int thingset_report_path(void *ts,uint8_t *buf,size_t cap,const char *path,enum thingset_data_format f){memset(buf,0xA7,17);return 17;}
static int host_process_count;
static void (*host_process_hook)(void);
static int thingset_process_message(void *ts,const uint8_t *buf,size_t n,uint8_t *out,size_t cap){host_process_count++;if(host_process_hook)host_process_hook();memset(out,0x85,20);return 20;}
#endif
