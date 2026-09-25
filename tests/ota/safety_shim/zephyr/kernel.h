#pragma once
/* No task API is supplied: entering maintenance must not depend on application
 * task creation, scheduling, or stopping its critical safety supervision. */
#define __weak __attribute__((weak))
