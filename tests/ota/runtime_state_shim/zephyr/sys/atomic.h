#pragma once
typedef int atomic_t;
#define ATOMIC_INIT(v) (v)
inline int atomic_get(const atomic_t *p){return *p;}
inline int atomic_set(atomic_t *p,int value){int old=*p;*p=value;return old;}
inline void atomic_clear(atomic_t *p){*p=0;}
inline void atomic_inc(atomic_t *p){++*p;}
inline bool atomic_cas(atomic_t *p,int old,int value){if(*p!=old)return false;*p=value;return true;}
