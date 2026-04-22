#pragma once

#include <esp_err.h>

#include "anna_cfg.h"
#include "app_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANNA_RESULT_CODE_APPLIED "APPLIED"
#define ANNA_RESULT_CODE_NOOP "NOOP"
#define ANNA_RESULT_CODE_SKIPPED_NO_BOOTSTRAP "SKIPPED_NO_BOOTSTRAP"
#define ANNA_RESULT_CODE_SKIPPED_POST_FABRIC "SKIPPED_POST_FABRIC"
#define ANNA_RESULT_CODE_NETWORK_TIMEOUT "NETWORK_TIMEOUT"
#define ANNA_RESULT_CODE_AUTH_FAILED "AUTH_FAILED"
#define ANNA_RESULT_CODE_BAD_FORMAT "BAD_FORMAT"
#define ANNA_RESULT_CODE_SAVE_FAIL "SAVE_FAIL"
#define ANNA_RESULT_CODE_REBUILD_FAIL "REBUILD_FAIL"
#define ANNA_RESULT_CODE_ROLLBACK_FAIL "ROLLBACK_FAIL"
#define ANNA_RESULT_CODE_LATE_IP_WINDOW_MISSED "LATE_IP_WINDOW_MISSED"

typedef struct {
    bool topology_changing;
    bool non_topology_only;
    bool slot_layout_changed;
    bool label_changed;
    bool product_info_changed;
} anna_cfg_diff_t;

typedef struct {
    const char *result_code;
    uint32_t applied_software_version;
    anna_cfg_diff_t diff;
} anna_runtime_apply_result_t;

typedef bool (*anna_runtime_pre_apply_guard_fn)(const anna_cfg_diff_t *diff, void *ctx, const char **out_block_result_code);

esp_err_t anna_cfg_diff_build(const anna_cfg_t *current, const anna_cfg_t *candidate, anna_cfg_diff_t *out_diff,
                              anna_endpoint_reuse_plan_t *out_reuse_plan);
esp_err_t anna_runtime_apply_candidate(const char *candidate_raw_json, const char *candidate_schema_version,
                                       anna_runtime_pre_apply_guard_fn pre_apply_guard, void *pre_apply_guard_ctx,
                                       anna_runtime_apply_result_t *out_result);

#ifdef __cplusplus
}
#endif
