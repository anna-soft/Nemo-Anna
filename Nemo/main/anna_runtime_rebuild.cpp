#include "anna_runtime_rebuild.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "anna_cfg_parse.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

namespace {
constexpr char TAG[] = "anna_runtime_rb";

static void clear_reuse_plan(anna_endpoint_reuse_plan_t *plan)
{
    if (!plan) {
        return;
    }
    memset(plan, ENDPOINT_ID_INVALID, sizeof(*plan));
}

static bool same_modes_topology(const anna_modes_t *lhs, const anna_modes_t *rhs)
{
    if (lhs->mode_count != rhs->mode_count || lhs->not_mode_mask != rhs->not_mode_mask) {
        return false;
    }
    return true;
}

static bool same_button_topology(const anna_button_t *lhs, const anna_button_t *rhs)
{
    return lhs->base.pin_no == rhs->base.pin_no && lhs->base.not_on_mask == rhs->base.not_on_mask &&
            lhs->base.not_off_mask == rhs->base.not_off_mask;
}

static bool same_switch_topology(const anna_switch_t *lhs, const anna_switch_t *rhs)
{
    return lhs->base.pin_no == rhs->base.pin_no && lhs->base.not_on_mask == rhs->base.not_on_mask &&
            lhs->base.not_off_mask == rhs->base.not_off_mask;
}

static bool same_con_btn_topology(const anna_con_btn_t *lhs, const anna_con_btn_t *rhs)
{
    return lhs->base.pin_no == rhs->base.pin_no && lhs->base.not_on_mask == rhs->base.not_on_mask &&
            lhs->base.not_off_mask == rhs->base.not_off_mask && lhs->mode_idx == rhs->mode_idx &&
            lhs->max_sec == rhs->max_sec && memcmp(lhs->bs_pin, rhs->bs_pin, sizeof(lhs->bs_pin)) == 0 &&
            memcmp(lhs->bs_evt, rhs->bs_evt, sizeof(lhs->bs_evt)) == 0 &&
            memcmp(lhs->as_pin, rhs->as_pin, sizeof(lhs->as_pin)) == 0 &&
            memcmp(lhs->as_tar, rhs->as_tar, sizeof(lhs->as_tar)) == 0 &&
            memcmp(lhs->as_evt, rhs->as_evt, sizeof(lhs->as_evt)) == 0;
}

static bool same_con_swt_topology(const anna_con_swt_t *lhs, const anna_con_swt_t *rhs)
{
    return lhs->base.pin_no == rhs->base.pin_no && lhs->base.not_on_mask == rhs->base.not_on_mask &&
            lhs->base.not_off_mask == rhs->base.not_off_mask && lhs->mode_idx == rhs->mode_idx &&
            memcmp(lhs->bs_pin, rhs->bs_pin, sizeof(lhs->bs_pin)) == 0 &&
            memcmp(lhs->bs_evt, rhs->bs_evt, sizeof(lhs->bs_evt)) == 0 &&
            memcmp(lhs->as_pin, rhs->as_pin, sizeof(lhs->as_pin)) == 0 &&
            memcmp(lhs->as_tar, rhs->as_tar, sizeof(lhs->as_tar)) == 0 &&
            memcmp(lhs->as_evt, rhs->as_evt, sizeof(lhs->as_evt)) == 0;
}

static bool same_fixed_labels(const anna_fixed_label_kv_t *lhs, uint8_t lhs_cnt, const anna_fixed_label_kv_t *rhs, uint8_t rhs_cnt)
{
    if (lhs_cnt != rhs_cnt) {
        return false;
    }
    return memcmp(lhs, rhs, sizeof(anna_fixed_label_kv_t) * lhs_cnt) == 0;
}

static bool same_labels_and_product_info(const anna_cfg_t *lhs, const anna_cfg_t *rhs, anna_cfg_diff_t *diff)
{
    bool label_changed = false;

    if (lhs->modes.mode_count == rhs->modes.mode_count) {
        for (int i = 0; i < lhs->modes.mode_count; ++i) {
            if (strncmp(lhs->modes.labels[i], rhs->modes.labels[i], sizeof(lhs->modes.labels[i])) != 0) {
                label_changed = true;
                break;
            }
        }
    } else {
        label_changed = true;
    }

    for (int i = 0; !label_changed && i < lhs->button_cnt && i < rhs->button_cnt; ++i) {
        if (strncmp(lhs->a_button[i].label, rhs->a_button[i].label, sizeof(lhs->a_button[i].label)) != 0 ||
            !same_fixed_labels(lhs->a_button[i].fixed_label, lhs->a_button[i].fixed_label_cnt,
                               rhs->a_button[i].fixed_label, rhs->a_button[i].fixed_label_cnt)) {
            label_changed = true;
        }
    }
    for (int i = 0; !label_changed && i < lhs->switch_cnt && i < rhs->switch_cnt; ++i) {
        if (strncmp(lhs->a_switch[i].label, rhs->a_switch[i].label, sizeof(lhs->a_switch[i].label)) != 0 ||
            !same_fixed_labels(lhs->a_switch[i].fixed_label, lhs->a_switch[i].fixed_label_cnt,
                               rhs->a_switch[i].fixed_label, rhs->a_switch[i].fixed_label_cnt)) {
            label_changed = true;
        }
    }
    for (int i = 0; !label_changed && i < lhs->con_btn_cnt && i < rhs->con_btn_cnt; ++i) {
        if (strncmp(lhs->con_btn[i].label, rhs->con_btn[i].label, sizeof(lhs->con_btn[i].label)) != 0 ||
            !same_fixed_labels(lhs->con_btn[i].fixed_label, lhs->con_btn[i].fixed_label_cnt,
                               rhs->con_btn[i].fixed_label, rhs->con_btn[i].fixed_label_cnt)) {
            label_changed = true;
        }
    }
    for (int i = 0; !label_changed && i < lhs->con_swt_cnt && i < rhs->con_swt_cnt; ++i) {
        if (strncmp(lhs->con_swt[i].label, rhs->con_swt[i].label, sizeof(lhs->con_swt[i].label)) != 0 ||
            !same_fixed_labels(lhs->con_swt[i].fixed_label, lhs->con_swt[i].fixed_label_cnt,
                               rhs->con_swt[i].fixed_label, rhs->con_swt[i].fixed_label_cnt)) {
            label_changed = true;
        }
    }

    diff->label_changed = label_changed;
    diff->product_info_changed = memcmp(&lhs->product_info, &rhs->product_info, sizeof(lhs->product_info)) != 0;
    return diff->label_changed || diff->product_info_changed;
}

static void collect_identity_reuse_plan(const anna_cfg_t *cfg, anna_endpoint_reuse_plan_t *plan)
{
    clear_reuse_plan(plan);
    if (!cfg || !plan) {
        return;
    }

    for (int i = 0; i < cfg->modes.mode_count && i < ANNA_MAX_MODE_COUNT; ++i) {
        plan->mode[i] = cfg->modes.endpoint_id[i];
    }
    for (int i = 0; i < cfg->button_cnt && i < ANNA_MAX_BUTTON; ++i) {
        plan->button[i] = cfg->a_button[i].base.endpoint_id;
    }
    for (int i = 0; i < cfg->switch_cnt && i < ANNA_MAX_SWITCH; ++i) {
        plan->swt[i] = cfg->a_switch[i].base.endpoint_id;
    }
    for (int i = 0; i < cfg->con_btn_cnt && i < ANNA_MAX_CON_ACT; ++i) {
        plan->con_btn[i] = cfg->con_btn[i].base.endpoint_id;
    }
    for (int i = 0; i < cfg->con_swt_cnt && i < ANNA_MAX_CON_SWT_ACT; ++i) {
        plan->con_swt[i] = cfg->con_swt[i].base.endpoint_id;
    }
}

static void collect_dynamic_endpoint_ids(const anna_cfg_t *cfg, uint16_t *ids, size_t *count)
{
    if (!ids || !count) {
        return;
    }
    *count = 0;
    if (!cfg) {
        return;
    }

    auto append_unique = [&](uint16_t endpoint_id) {
        if (endpoint_id == ENDPOINT_ID_INVALID || endpoint_id == 0) {
            return;
        }
        for (size_t i = 0; i < *count; ++i) {
            if (ids[i] == endpoint_id) {
                return;
            }
        }
        if (*count < ANNA_MAX_DYNAMIC_ENDPOINT_COUNT) {
            ids[(*count)++] = endpoint_id;
        }
    };

    for (int i = 0; i < cfg->modes.mode_count && i < ANNA_MAX_MODE_COUNT; ++i) {
        append_unique(cfg->modes.endpoint_id[i]);
    }
    for (int i = 0; i < cfg->button_cnt && i < ANNA_MAX_BUTTON; ++i) {
        append_unique(cfg->a_button[i].base.endpoint_id);
    }
    for (int i = 0; i < cfg->switch_cnt && i < ANNA_MAX_SWITCH; ++i) {
        append_unique(cfg->a_switch[i].base.endpoint_id);
    }
    for (int i = 0; i < cfg->con_btn_cnt && i < ANNA_MAX_CON_ACT; ++i) {
        append_unique(cfg->con_btn[i].base.endpoint_id);
    }
    for (int i = 0; i < cfg->con_swt_cnt && i < ANNA_MAX_CON_SWT_ACT; ++i) {
        append_unique(cfg->con_swt[i].base.endpoint_id);
    }
}

static esp_err_t destroy_dynamic_endpoints(const anna_cfg_t *cfg)
{
    node_t *node = app_settings_get_runtime_node();
    if (!node) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t ids[ANNA_MAX_DYNAMIC_ENDPOINT_COUNT] = {};
    size_t id_count = 0;
    collect_dynamic_endpoint_ids(cfg, ids, &id_count);
    for (size_t i = 0; i < id_count; ++i) {
        endpoint_t *endpoint = endpoint::get(node, ids[i]);
        if (!endpoint) {
            continue;
        }
        esp_err_t err = endpoint::destroy(node, endpoint);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "endpoint destroy failed: ep=%u err=%s", (unsigned)ids[i], esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t apply_root_node_label(const char *node_label)
{
    char label_buf[ANNA_MAX_LABEL_LEN] = { 0 };
    const char *effective = (node_label && node_label[0] != '\0') ? node_label : "Product Nemo";
    strlcpy(label_buf, effective, sizeof(label_buf));
    esp_matter_attr_val_t val = esp_matter_char_str(label_buf, strlen(label_buf));
    return attribute::update(0, BasicInformation::Id, BasicInformation::Attributes::NodeLabel::Id, &val);
}

static esp_err_t apply_non_topology_attributes(void)
{
    esp_err_t err = apply_root_node_label(g_anna_cfg.product_info.node_label);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "node label update failed: %s", esp_err_to_name(err));
        return err;
    }

    chip::ChipError chip_err = set_all_user_label();
    if (chip_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "user label update failed: %" CHIP_ERROR_FORMAT, chip_err.Format());
        return ESP_FAIL;
    }
    app_driver_boot_reconcile_mode_only();
    return ESP_OK;
}

static char *load_blob_copy(int (*loader)(void *, size_t *))
{
    size_t len = 0;
    int rc = loader(NULL, &len);
    if (rc != ESP_OK || len == 0) {
        return NULL;
    }

    char *buf = static_cast<char *>(calloc(1, len + 1));
    if (!buf) {
        return NULL;
    }

    size_t inout = len;
    rc = loader(buf, &inout);
    if (rc != ESP_OK || inout == 0) {
        free(buf);
        return NULL;
    }
    buf[inout] = '\0';
    return buf;
}

static char *load_schema_copy()
{
    size_t len = 0;
    int rc = anna_cfg_load_schema_version_from_nvs(NULL, &len);
    if (rc != ESP_OK || len == 0) {
        return NULL;
    }

    char *buf = static_cast<char *>(calloc(1, len + 1));
    if (!buf) {
        return NULL;
    }

    rc = anna_cfg_load_schema_version_from_nvs(buf, &len);
    if (rc != ESP_OK || len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static esp_err_t rollback_rebuild(const anna_cfg_t *old_cfg, const char *failure_stage)
{
    if (!old_cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "rollback rebuild start: stage=%s", failure_stage ? failure_stage : "unknown");
    (void)destroy_dynamic_endpoints(&g_anna_cfg);
    app_settings_clear_runtime_state();
    app_driver_runtime_clear_state();

    g_anna_cfg = *old_cfg;
    anna_endpoint_reuse_plan_t rollback_plan = {};
    collect_identity_reuse_plan(old_cfg, &rollback_plan);
    esp_err_t err = app_settings_rebuild_from_current_cfg(&rollback_plan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback rebuild failed: %s", esp_err_to_name(err));
        return err;
    }
    err = apply_non_topology_attributes();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback attribute apply failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
} // namespace

extern "C" esp_err_t anna_cfg_diff_build(const anna_cfg_t *current, const anna_cfg_t *candidate, anna_cfg_diff_t *out_diff,
                                          anna_endpoint_reuse_plan_t *out_reuse_plan)
{
    if (!current || !candidate || !out_diff) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_diff, 0, sizeof(*out_diff));
    clear_reuse_plan(out_reuse_plan);

    out_diff->slot_layout_changed = current->modes.mode_count != candidate->modes.mode_count ||
            current->button_cnt != candidate->button_cnt || current->switch_cnt != candidate->switch_cnt ||
            current->con_btn_cnt != candidate->con_btn_cnt || current->con_swt_cnt != candidate->con_swt_cnt;

    if (!same_modes_topology(&current->modes, &candidate->modes)) {
        out_diff->topology_changing = true;
    } else if (out_reuse_plan) {
        for (int i = 0; i < current->modes.mode_count && i < candidate->modes.mode_count && i < ANNA_MAX_MODE_COUNT; ++i) {
            out_reuse_plan->mode[i] = current->modes.endpoint_id[i];
        }
    }

    for (int i = 0; i < ANNA_MAX_BUTTON; ++i) {
        bool current_present = i < current->button_cnt;
        bool candidate_present = i < candidate->button_cnt;
        if (current_present != candidate_present) {
            out_diff->topology_changing = true;
            continue;
        }
        if (!current_present) {
            continue;
        }
        if (!same_button_topology(&current->a_button[i], &candidate->a_button[i])) {
            out_diff->topology_changing = true;
        } else if (out_reuse_plan) {
            out_reuse_plan->button[i] = current->a_button[i].base.endpoint_id;
        }
    }

    for (int i = 0; i < ANNA_MAX_SWITCH; ++i) {
        bool current_present = i < current->switch_cnt;
        bool candidate_present = i < candidate->switch_cnt;
        if (current_present != candidate_present) {
            out_diff->topology_changing = true;
            continue;
        }
        if (!current_present) {
            continue;
        }
        if (!same_switch_topology(&current->a_switch[i], &candidate->a_switch[i])) {
            out_diff->topology_changing = true;
        } else if (out_reuse_plan) {
            out_reuse_plan->swt[i] = current->a_switch[i].base.endpoint_id;
        }
    }

    for (int i = 0; i < ANNA_MAX_CON_ACT; ++i) {
        bool current_present = i < current->con_btn_cnt;
        bool candidate_present = i < candidate->con_btn_cnt;
        if (current_present != candidate_present) {
            out_diff->topology_changing = true;
            continue;
        }
        if (!current_present) {
            continue;
        }
        if (!same_con_btn_topology(&current->con_btn[i], &candidate->con_btn[i])) {
            out_diff->topology_changing = true;
        } else if (out_reuse_plan) {
            out_reuse_plan->con_btn[i] = current->con_btn[i].base.endpoint_id;
        }
    }

    for (int i = 0; i < ANNA_MAX_CON_SWT_ACT; ++i) {
        bool current_present = i < current->con_swt_cnt;
        bool candidate_present = i < candidate->con_swt_cnt;
        if (current_present != candidate_present) {
            out_diff->topology_changing = true;
            continue;
        }
        if (!current_present) {
            continue;
        }
        if (!same_con_swt_topology(&current->con_swt[i], &candidate->con_swt[i])) {
            out_diff->topology_changing = true;
        } else if (out_reuse_plan) {
            out_reuse_plan->con_swt[i] = current->con_swt[i].base.endpoint_id;
        }
    }

    same_labels_and_product_info(current, candidate, out_diff);
    out_diff->non_topology_only = !out_diff->topology_changing && (out_diff->label_changed || out_diff->product_info_changed);
    return ESP_OK;
}

extern "C" esp_err_t anna_runtime_apply_candidate(const char *candidate_raw_json, const char *candidate_schema_version,
                                                   anna_runtime_pre_apply_guard_fn pre_apply_guard, void *pre_apply_guard_ctx,
                                                   anna_runtime_apply_result_t *out_result)
{
    esp_err_t status = ESP_OK;

    if (!candidate_raw_json || !candidate_schema_version || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    anna_cfg_t *old_cfg = static_cast<anna_cfg_t *>(calloc(1, sizeof(*old_cfg)));
    anna_cfg_t *candidate_cfg = static_cast<anna_cfg_t *>(calloc(1, sizeof(*candidate_cfg)));
    anna_cfg_diff_t diff = {};
    anna_endpoint_reuse_plan_t reuse_plan = {};
    char *old_raw_json = load_blob_copy(anna_cfg_load_raw_from_nvs);
    char *old_schema_version = load_schema_copy();
    int64_t apply_started_at_us = 0;
    int64_t parse_done_at_us = 0;
    int64_t diff_done_at_us = 0;
    int64_t destroy_done_at_us = 0;
    int64_t rebuild_done_at_us = 0;
    int64_t attrs_done_at_us = 0;
    int64_t save_done_at_us = 0;
    const char *blocked_result_code = nullptr;
    esp_err_t err = ESP_OK;

    out_result->result_code = ANNA_RESULT_CODE_BAD_FORMAT;
    out_result->applied_software_version = 0;
    memset(&out_result->diff, 0, sizeof(out_result->diff));

    if (!old_cfg || !candidate_cfg) {
        ESP_LOGE(TAG, "candidate apply heap alloc failed: old_cfg=%p candidate_cfg=%p", old_cfg, candidate_cfg);
        out_result->result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
        goto cleanup;
    }

    *old_cfg = g_anna_cfg;

    apply_started_at_us = esp_timer_get_time();
    err = (esp_err_t)anna_cfg_parse_json_into(candidate_raw_json, strlen(candidate_raw_json), candidate_cfg);
    parse_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "candidate staged parse failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = anna_cfg_diff_build(old_cfg, candidate_cfg, &diff, &reuse_plan);
    diff_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        out_result->result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
        status = err;
        goto cleanup;
    }
    out_result->diff = diff;

    ESP_LOGI(TAG,
             "diff ready: old_sw=%" PRIu32 " candidate_sw=%" PRIu32 " parse_ms=%lld diff_ms=%lld diff[topology=%d non_topology_only=%d "
             "slot_layout=%d label=%d product_info=%d]",
             old_cfg->product_info.sw_ver, candidate_cfg->product_info.sw_ver,
             static_cast<long long>((parse_done_at_us - apply_started_at_us) / 1000),
             static_cast<long long>((diff_done_at_us - parse_done_at_us) / 1000), diff.topology_changing ? 1 : 0,
             diff.non_topology_only ? 1 : 0, diff.slot_layout_changed ? 1 : 0, diff.label_changed ? 1 : 0,
             diff.product_info_changed ? 1 : 0);

    if (candidate_cfg->product_info.sw_ver == old_cfg->product_info.sw_ver) {
        out_result->result_code = ANNA_RESULT_CODE_NOOP;
        out_result->applied_software_version = old_cfg->product_info.sw_ver;
        ESP_LOGI(TAG,
                 "apply timing: result=NOOP old_sw=%" PRIu32 " candidate_sw=%" PRIu32 " parse_ms=%lld diff_ms=%lld total_ms=%lld "
                 "diff[topology=%d non_topology_only=%d slot_layout=%d label=%d product_info=%d]",
                 old_cfg->product_info.sw_ver, candidate_cfg->product_info.sw_ver,
                 static_cast<long long>((parse_done_at_us - apply_started_at_us) / 1000),
                 static_cast<long long>((diff_done_at_us - parse_done_at_us) / 1000),
                 static_cast<long long>((diff_done_at_us - apply_started_at_us) / 1000), diff.topology_changing ? 1 : 0,
                 diff.non_topology_only ? 1 : 0, diff.slot_layout_changed ? 1 : 0, diff.label_changed ? 1 : 0,
                 diff.product_info_changed ? 1 : 0);
        goto cleanup;
    }

    if (pre_apply_guard && !pre_apply_guard(&diff, pre_apply_guard_ctx, &blocked_result_code)) {
        out_result->result_code = blocked_result_code ? blocked_result_code : ANNA_RESULT_CODE_LATE_IP_WINDOW_MISSED;
        ESP_LOGW(TAG,
                 "apply commit blocked: result=%s old_sw=%" PRIu32 " candidate_sw=%" PRIu32 " total_ms=%lld diff[topology=%d "
                 "non_topology_only=%d slot_layout=%d label=%d product_info=%d]",
                 out_result->result_code, old_cfg->product_info.sw_ver, candidate_cfg->product_info.sw_ver,
                 static_cast<long long>((diff_done_at_us - apply_started_at_us) / 1000), diff.topology_changing ? 1 : 0,
                 diff.non_topology_only ? 1 : 0, diff.slot_layout_changed ? 1 : 0, diff.label_changed ? 1 : 0,
                 diff.product_info_changed ? 1 : 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "atomic apply start: old_sw=%" PRIu32 " candidate_sw=%" PRIu32 " diff[topology=%d non_topology_only=%d "
             "slot_layout=%d label=%d product_info=%d]",
             old_cfg->product_info.sw_ver, candidate_cfg->product_info.sw_ver, diff.topology_changing ? 1 : 0,
             diff.non_topology_only ? 1 : 0, diff.slot_layout_changed ? 1 : 0, diff.label_changed ? 1 : 0,
             diff.product_info_changed ? 1 : 0);

    app_driver_boot_safe_off_sync_non_mode();
    app_settings_clear_runtime_state();
    app_driver_runtime_clear_state();

    err = destroy_dynamic_endpoints(old_cfg);
    destroy_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        out_result->result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
        if (rollback_rebuild(old_cfg, "destroy-old") != ESP_OK) {
            out_result->result_code = ANNA_RESULT_CODE_ROLLBACK_FAIL;
        }
        goto cleanup;
    }

    g_anna_cfg = *candidate_cfg;
    err = app_settings_rebuild_from_current_cfg(&reuse_plan);
    rebuild_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        out_result->result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
        if (rollback_rebuild(old_cfg, "build-new") != ESP_OK) {
            out_result->result_code = ANNA_RESULT_CODE_ROLLBACK_FAIL;
        }
        goto cleanup;
    }

    err = apply_non_topology_attributes();
    attrs_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        out_result->result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
        if (rollback_rebuild(old_cfg, "apply-attrs") != ESP_OK) {
            out_result->result_code = ANNA_RESULT_CODE_ROLLBACK_FAIL;
        }
        goto cleanup;
    }

    err = (esp_err_t)anna_cfg_save_snapshot_to_nvs(candidate_raw_json, strlen(candidate_raw_json), candidate_cfg,
                                                   candidate_schema_version);
    save_done_at_us = esp_timer_get_time();
    if (err != ESP_OK) {
        out_result->result_code = ANNA_RESULT_CODE_SAVE_FAIL;
        if (old_raw_json && old_schema_version) {
            (void)anna_cfg_save_snapshot_to_nvs(old_raw_json, strlen(old_raw_json), old_cfg, old_schema_version);
        }
        if (rollback_rebuild(old_cfg, "save-new") != ESP_OK) {
            out_result->result_code = ANNA_RESULT_CODE_ROLLBACK_FAIL;
        }
        goto cleanup;
    }

    out_result->result_code = ANNA_RESULT_CODE_APPLIED;
    out_result->applied_software_version = candidate_cfg->product_info.sw_ver;
    ESP_LOGI(TAG,
             "apply timing: result=APPLIED old_sw=%" PRIu32 " candidate_sw=%" PRIu32 " parse_ms=%lld diff_ms=%lld "
             "destroy_ms=%lld rebuild_ms=%lld attrs_ms=%lld save_ms=%lld total_ms=%lld diff[topology=%d non_topology_only=%d "
             "slot_layout=%d label=%d product_info=%d]",
             old_cfg->product_info.sw_ver, candidate_cfg->product_info.sw_ver,
             static_cast<long long>((parse_done_at_us - apply_started_at_us) / 1000),
             static_cast<long long>((diff_done_at_us - parse_done_at_us) / 1000),
             static_cast<long long>((destroy_done_at_us - diff_done_at_us) / 1000),
             static_cast<long long>((rebuild_done_at_us - destroy_done_at_us) / 1000),
             static_cast<long long>((attrs_done_at_us - rebuild_done_at_us) / 1000),
             static_cast<long long>((save_done_at_us - attrs_done_at_us) / 1000),
             static_cast<long long>((save_done_at_us - apply_started_at_us) / 1000), diff.topology_changing ? 1 : 0,
             diff.non_topology_only ? 1 : 0, diff.slot_layout_changed ? 1 : 0, diff.label_changed ? 1 : 0,
             diff.product_info_changed ? 1 : 0);

cleanup:
    free(old_cfg);
    free(candidate_cfg);
    free(old_raw_json);
    free(old_schema_version);
    return status;
}
