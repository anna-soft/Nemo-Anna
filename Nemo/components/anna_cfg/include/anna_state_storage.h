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
 * - cs_test (u8): optional cloud sync test-only harness intent
 */

typedef enum {
    ANNA_CLOUD_SYNC_TEST_INTENT_NONE = 0,
    ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PULL_ON_NEXT_IP = 1,
    ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP = 2,
    ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_LATE_WINDOW_CLOSE = 3,
    ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_POST_FABRIC_CLOSE = 4,
} anna_cloud_sync_test_intent_t;

bool anna_state_is_provisioned(void);
int  anna_state_set_provisioned(uint8_t v); /* v: 0 or 1 */

/* Best-effort storage for unit-info. buf may be raw JSON bytes (UTF-8). */
int  anna_state_set_unit_blob(const void *buf, size_t len);
bool anna_state_has_unit_blob(void);
int  anna_state_get_unit_blob(void *out, size_t *inout_len); /* out may be NULL to query size */

int  anna_state_set_cloud_sync_test_intent(anna_cloud_sync_test_intent_t intent);
int  anna_state_get_cloud_sync_test_intent(anna_cloud_sync_test_intent_t *out_intent);
int  anna_state_clear_cloud_sync_test_intent(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
