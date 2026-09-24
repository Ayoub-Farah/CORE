#pragma once
#include <stdint.h>
static inline void sys_put_le16(uint16_t v,uint8_t *p){p[0]=v;p[1]=v>>8;}
static inline void sys_put_le32(uint32_t v,uint8_t *p){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static inline void sys_put_le64(uint64_t v,uint8_t *p){for(unsigned i=0;i<8;i++)p[i]=v>>(8*i);}
static inline uint16_t sys_get_le16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static inline uint32_t sys_get_le32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static inline uint64_t sys_get_le64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
