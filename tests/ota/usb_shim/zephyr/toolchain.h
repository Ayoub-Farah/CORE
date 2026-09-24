#pragma once
/* zcbor_bulk.h only needs STRINGIFY from the Zephyr toolchain boundary. */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
