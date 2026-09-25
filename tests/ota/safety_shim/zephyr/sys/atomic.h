#pragma once
typedef int atomic_t;
#define ATOMIC_INIT(value) (value)
inline int atomic_get(const atomic_t *value) { return *value; }
inline int atomic_set(atomic_t *value, int next)
{ int previous = *value; *value = next; return previous; }
