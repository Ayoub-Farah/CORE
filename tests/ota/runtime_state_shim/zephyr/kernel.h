#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_OWNTECH_OTA_QUEUE_DEPTH 8
#define CONFIG_OWNTECH_OTA_STACK_SIZE 8192
#define CONFIG_OWNTECH_OTA_HEALTH_TIMEOUT_MS 15000
#define CONFIG_OWNTECH_OTA_BLOCK_INTERVAL_MS 1
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define K_MSEC(n) (n)
#define K_SECONDS(n) ((n)*1000)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct k_mutex {};
struct k_spinlock {};
struct k_sem {unsigned count, maximum;};
struct k_msgq {size_t item_size,capacity,head,count;uint8_t bytes[16384];};
void runtime_test_msgq_wait(k_msgq *,int);
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_SEM_DEFINE(name, initial, maximum) struct k_sem name={initial,maximum}
#define K_MSGQ_DEFINE(name, size, count, alignment) struct k_msgq name={size,count,0,0,{0}}
#define K_THREAD_DEFINE(name, stack, entry, a, b, c, priority, options, delay) \
    [[maybe_unused]] static auto name = &entry
inline int k_mutex_lock(k_mutex *,int) {return 0;}
inline void k_mutex_unlock(k_mutex *) {}
inline int k_spin_lock(k_spinlock *) {return 0;}
inline void k_spin_unlock(k_spinlock *,int) {}
inline void k_sem_reset(k_sem *s){s->count=0;}
inline void k_sem_give(k_sem *s){if(s->count<s->maximum)++s->count;}
int k_sem_take(k_sem *,int);
inline int k_msgq_put(k_msgq *q,const void *p,int){
    if(q->count==q->capacity)return -1;
    memcpy(q->bytes+((q->head+q->count)%q->capacity)*q->item_size,p,q->item_size);++q->count;return 0;
}
inline int k_msgq_get(k_msgq *q,void *p,int timeout){
    runtime_test_msgq_wait(q,timeout);
    if(!q->count)return -1;
    memcpy(p,q->bytes+q->head*q->item_size,q->item_size);q->head=(q->head+1)%q->capacity;--q->count;return 0;
}
inline unsigned k_msgq_num_used_get(k_msgq *q){return (unsigned)q->count;}
int64_t k_uptime_get();
void k_sleep(int);
