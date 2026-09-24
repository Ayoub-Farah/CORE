#pragma once
#include <stddef.h>
#include <stdint.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
struct smp_streamer;
struct mgmt_handler { int (*mh_read)(smp_streamer *); int (*mh_write)(smp_streamer *); };
struct mgmt_group { const mgmt_handler *mg_handlers; size_t mg_handlers_count; uint16_t mg_group_id; };
inline void mgmt_register_group(mgmt_group *) {}
