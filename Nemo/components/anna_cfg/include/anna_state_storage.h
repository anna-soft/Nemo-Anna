#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Provisioning runtime state (stored in NVS partition "runtime_anna", namespace "anna_state")
 *
 * Keys:
 * - provisioned (u8): 0=not provisioned, 1=provisioned
 * - unit_blob (blob): best-effort unit-info payload storage
 */

bool anna_state_is_provisioned(void);
int  anna_state_set_provisioned(uint8_t v); /* v: 0 or 1 */

/* Best-effort storage for unit-info. buf may be raw JSON bytes (UTF-8). */
int  anna_state_set_unit_blob(const void *buf, size_t len);
bool anna_state_has_unit_blob(void);
int  anna_state_get_unit_blob(void *out, size_t *inout_len); /* out may be NULL to query size */

#ifdef __cplusplus
} /* extern "C" */
#endif


