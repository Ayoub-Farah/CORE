/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Zephyr owns process lifetime and allocation. In particular Newlib's process
 * exit/_fini machinery is unavailable in this bare-metal PlatformIO toolchain.
 * SHA-256 uses stack contexts; Zephyr initializes the Mbed TLS heap if enabled. */
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
