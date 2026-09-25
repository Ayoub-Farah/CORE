#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define K_FOREVER 0
struct k_mutex { int depth; };
#define K_MUTEX_DEFINE(name) struct k_mutex name = {0}
int k_mutex_lock(struct k_mutex *, int);
int k_mutex_unlock(struct k_mutex *);
int printk(const char *, ...);
