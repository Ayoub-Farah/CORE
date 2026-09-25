/* Boundaries only: production console formatter and event-driven LED handler. */
#include "sdk_shim/sdk_host.h"
static device test_device={"console"};
#define DT_CHOSEN(...) 1
#define DT_ALIAS(...) 1
#define DEVICE_DT_GET(...) (&test_device)
#define K_WORK_DEFINE(n,fn) struct k_work n={}
#define K_WORK_DELAYABLE_DEFINE(n,fn) struct k_work_delayable n={{},fn}
static unsigned work_requests, led_requests, led_timers, led_writes;
static int last_led, next_delay;
static int k_work_submit(k_work *){++work_requests;return 0;}
static int k_work_reschedule(k_work_delayable *,k_timeout_t t){++led_requests;next_delay=(int)t.ticks;return 0;}
static int k_work_schedule(k_work_delayable *,k_timeout_t t){++led_timers;next_delay=(int)t.ticks;return 0;}
#define ATOMIC_INIT(n) (n)
#define atomic_set test_atomic_set
static int test_atomic_set(atomic_t *p,int value){int old=*p;*p=value;return old;}
static void atomic_xor(atomic_t *p,int value){*p^=value;}
struct gpio_dt_spec{int unused;};
#define GPIO_DT_SPEC_GET(...) gpio_dt_spec{}
#define GPIO_OUTPUT_INACTIVE 0
static bool gpio_is_ready_dt(const gpio_dt_spec *){return true;}
static int gpio_pin_configure_dt(const gpio_dt_spec *,int){return 0;}
static int gpio_pin_set_dt(const gpio_dt_spec *,int v){++led_writes;last_led=v;return 0;}
#include "../../zephyr/modules/owntech_ota/zephyr/src/ota_console.cpp"
#include "../../zephyr/modules/owntech_ota/zephyr/src/ota_feedback.cpp"
static ota_observation source;
static ota_service_diagnostics diagnostic={"WAITING_CAN",0,true,false,false,false,false};
static char fifo[768];
static unsigned fifo_calls;
static int fifo_capacity=768;
int uart_fifo_fill(const device *,const uint8_t *p,int n)
{++fifo_calls;assert(n>0&&n<768);memcpy(fifo,p,MIN(n,fifo_capacity));return MIN(n,fifo_capacity);}
extern "C" void ota_service_snapshot(ota_observation *o,ota_service_diagnostics *d){*o=source;*d=diagnostic;}
extern "C" bool ota_safety_inhibited(){return false;}
int main()
{
    source.identity.hardware_id=0xffffffff;source.identity.layout_id=0xffffffff;
    source.identity.bootloader_id=0xffffffff;source.identity.usable_slot_size=227328;
    source.identity.usable_image_size=221184;source.confirmed=true;
    memset(source.active_version,'v',31);memset(source.active_build_id,'a',31);
    memset(source.active_mcuboot_image_hash,0x5a,32);source.identity.eui[0]=0xab;
    ota_console_request_status();assert(work_requests==1&&fifo_calls==0);
    status_work(nullptr);assert(fifo_calls==1&&strstr(fifo,"\nOTAR2 {"));
    assert(strstr(fifo,"\"image_class\":\"receiver\""));
    assert(strstr(fifo,"\"identity\":\"ab00000000000000\""));
    assert(strstr(fifo,"\"available\":false"));
    assert(strstr(fifo,"\"mcuboot_image_hash\":\"5a5a"));
    status_work(nullptr);assert(fifo_calls==1); /* Bounded request rate. */
    host_now=300;fifo_capacity=0;status_work(nullptr);
    assert(fifo_calls==2&&host_now==300); /* Full console cannot block or retry internally. */
    host_now=600;fifo_capacity=768;diagnostic.healthy=diagnostic.can_ready=true;
    status_work(nullptr);assert(strstr(fifo,"\"available\":true"));
    ota_feedback_application_led(1);assert(led_requests==1&&led_writes==0);
    feedback(nullptr);assert(last_led==1&&led_timers==0);
    ota_feedback_application_led(2);feedback(nullptr);assert(last_led==0&&led_timers==0);
    ota_feedback_state(OTA_IDLE);assert(led_requests==2); /* No wakeup for repeated idle. */
    host_now=50;ota_feedback_state(OTA_PREPARING);feedback(nullptr);
    assert(last_led==1&&next_delay==50&&led_timers==1);
    host_now=100;feedback(nullptr);assert(!last_led&&next_delay==100);
    ota_feedback_state(OTA_VALID);unsigned timers=led_timers;feedback(nullptr);
    assert(last_led&&led_timers==timers); /* Steady validated LED has no timer. */
    host_isr=true;ota_feedback_application_led(1);assert(led_requests==4);
    ota_feedback_state(OTA_SUCCEEDED);feedback(nullptr);assert(last_led&&led_timers==timers);
    puts("Receiver console/LED: bounded FIFO, no RX ownership, events and idle timers OK");
    return 0;
}
