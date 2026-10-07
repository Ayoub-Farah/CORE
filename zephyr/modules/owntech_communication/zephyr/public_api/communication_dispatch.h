/* SPDX-License-Identifier: LGPL-2.1 */
#ifndef COMMUNICATION_DISPATCH_H_
#define COMMUNICATION_DISPATCH_H_

#include <stdint.h>

/* Internal hook called before the user critical task. No-op in software mode. */
void communication_dispatch();
void communication_dispatch_configure(uint8_t *ring, uint8_t *snapshot,
                                      uint16_t size);
void communication_dispatch_reset();
uint16_t communication_dispatch_received_size();
uint32_t communication_dispatch_errors();

#endif
