#pragma once
#include <stddef.h>
#include <stdint.h>
#define K_FOREVER 0
#define K_MSEC(n) (n)
struct k_mutex {};
struct k_work {};
struct k_work_delayable { void (*handler)(struct k_work *); };
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_WORK_DELAYABLE_DEFINE(name, callback) struct k_work_delayable name = {callback}
inline void k_mutex_lock(k_mutex *, int) {}
inline void k_mutex_unlock(k_mutex *) {}
int k_work_schedule(k_work_delayable *, uint32_t);
