#pragma once
#include <stdint.h>
#include <stddef.h>
enum mgmt_cb_return { MGMT_CB_OK, MGMT_CB_ERROR_RC, MGMT_CB_ERROR_ERR };
struct mgmt_evt_op_cmd_arg { uint16_t group; uint8_t id; uint8_t op; };
typedef mgmt_cb_return (*mgmt_cb)(uint32_t,mgmt_cb_return,int32_t *,uint16_t *,bool *,void *,size_t);
struct mgmt_callback { mgmt_cb callback; uint32_t event_id; };
#define MGMT_EVT_OP_CMD_RECV 1U
inline void mgmt_callback_register(mgmt_callback *) {}
