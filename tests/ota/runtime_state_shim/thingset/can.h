#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/atomic.h>
struct thingset_can_context {uint8_t node_addr;atomic_t ready,driver_started,init_error;};
typedef void (*thingset_can_state_callback_t)(void *);
void thingset_can_set_state_callback(thingset_can_state_callback_t,void *);
thingset_can_context *thingset_can_get_inst();
int thingset_can_announce_address(int);
int thingset_can_probe_address(uint8_t,int);
int thingset_can_send_raw_report(const uint8_t *,size_t,int);
