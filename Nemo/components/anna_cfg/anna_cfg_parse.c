/* cJSON 기반 anna_cfg_parse_json() 구현 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <esp_err.h>
#include <esp_log.h>

#include <cJSON.h>

#include "anna_cfg.h"

static const char *TAG = "anna_cfg_parse";


/* -------------------------------------------------------------------------- */
/* cJSON 기반 신규 파서 구현                                                   */
/* -------------------------------------------------------------------------- */

static void str_copy_or_empty(char *dst, size_t dst_sz, const cJSON *j_str)
{
    if (!dst || dst_sz == 0) return;
    if (!cJSON_IsString(j_str) || !j_str->valuestring) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(j_str->valuestring);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, j_str->valuestring, n);
    dst[n] = '\0';
}

static void str_copy_or_empty_limit(char *dst, size_t dst_sz, const cJSON *j_str, size_t max_visible_len)
{
    if (!dst || dst_sz == 0) return;
    if (!cJSON_IsString(j_str) || !j_str->valuestring) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(j_str->valuestring);
    if (n > max_visible_len) n = max_visible_len;
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, j_str->valuestring, n);
    dst[n] = '\0';
}

static bool json_get_u32_flexible(const cJSON *obj, const char *key, uint32_t *out)
{
    if (!cJSON_IsObject(obj) || !key || !out) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!v) return false;
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0) return false;
        *out = (uint32_t)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        errno = 0;
        char *end = NULL;
        unsigned long n = strtoul(v->valuestring, &end, 10);
        if (errno != 0 || end == v->valuestring || (end && *end != '\0')) return false;
        *out = (uint32_t)n;
        return true;
    }
    return false;
}

static bool json_get_i32_nullable(const cJSON *obj, const char *key, int32_t null_value, int32_t *out)
{
    if (!cJSON_IsObject(obj) || !key || !out) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!v) return false;
    if (cJSON_IsNull(v)) {
        *out = null_value;
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = (int32_t)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        errno = 0;
        char *end = NULL;
        long n = strtol(v->valuestring, &end, 10);
        if (errno != 0 || end == v->valuestring || (end && *end != '\0')) return false;
        *out = (int32_t)n;
        return true;
    }
    return false;
}

static pin_mask_t build_pin_mask_cjson(const cJSON *arr)
{
    pin_mask_t mask = 0;
    if (cJSON_IsNull(arr) || !arr) return 0;
    if (!cJSON_IsArray(arr)) return 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        int32_t pin = -1;
        if (cJSON_IsNumber(it)) {
            pin = (int32_t)it->valuedouble;
        } else if (cJSON_IsString(it) && it->valuestring) {
            errno = 0;
            char *end = NULL;
            long n = strtol(it->valuestring, &end, 10);
            if (errno == 0 && end != it->valuestring && end && *end == '\0') {
                pin = (int32_t)n;
            }
        }
        if (pin >= 0 && pin < 32) {
            mask |= (1u << (uint32_t)pin);
        }
    }
    return mask;
}

/* -------------------------------------------------------------------------- */
/* FixedLabel helpers (server schema: FixedLabel: Array<{Label,Value}>, max 5) */
/* -------------------------------------------------------------------------- */

static inline bool fixed_label_is_allowed_char(char c)
{
    /* Allowed: [a-z0-9_<>!=,:/+- ] (space handled by normalizer) */
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '_') return true;
    if (c == '<') return true;
    if (c == '>') return true;
    if (c == '=') return true;
    if (c == '!') return true;
    if (c == ',') return true;
    if (c == ':') return true;
    if (c == '/') return true;
    if (c == '+') return true;
    if (c == '-') return true;
    if (c == ' ') return true;
    return false;
}

/* Normalize according to server rule:
 * - trim
 * - collapse internal spaces
 * - lowercase
 * - validate allowed charset
 * - validate length 2..16 AFTER normalization
 */
static bool fixed_label_normalize(const char *in, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!in) return false;

    size_t w = 0;
    bool pending_space = false;

    /* Skip leading spaces */
    const char *p = in;
    while (*p == ' ') p++;

    for (; *p != '\0'; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }

        if (c == ' ') {
            pending_space = true;
            continue;
        }

        if (!fixed_label_is_allowed_char(c)) {
            return false;
        }

        if (pending_space && w > 0) {
            if (w + 1 >= out_sz) return false;
            out[w++] = ' ';
        }
        pending_space = false;

        if (w + 1 >= out_sz) return false;
        out[w++] = c;
    }

    /* trailing spaces are naturally trimmed by not flushing pending_space */
    if (w < 2 || w > 16) {
        out[0] = '\0';
        return false;
    }
    out[w] = '\0';
    return true;
}

static void fixed_label_clear(anna_fixed_label_kv_t *out, uint8_t *out_cnt)
{
    if (out_cnt) *out_cnt = 0;
    if (!out) return;
    for (int i = 0; i < ANNA_MAX_FIXED_LABEL; ++i) {
        out[i].label[0] = '\0';
        out[i].value[0] = '\0';
    }
}

static void parse_fixed_label_array_cjson(const cJSON *parent_obj, anna_fixed_label_kv_t *out, uint8_t *out_cnt)
{
    fixed_label_clear(out, out_cnt);
    if (!cJSON_IsObject(parent_obj)) return;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive((cJSON *)parent_obj, "FixedLabel");
    if (!arr || cJSON_IsNull(arr)) {
        /* legacy: key missing OR explicitly null -> treat as absent/empty */
        return;
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "FixedLabel ignored: not array");
        return;
    }

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) {
            ESP_LOGW(TAG, "FixedLabel item ignored: not object");
            continue;
        }
        if (!out_cnt || *out_cnt >= ANNA_MAX_FIXED_LABEL) {
            ESP_LOGW(TAG, "FixedLabel overflow: max=%d (extra items ignored)", (int)ANNA_MAX_FIXED_LABEL);
            return;
        }

        const cJSON *j_l = cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Label");
        const cJSON *j_v = cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Value");
        if (!cJSON_IsString(j_l) || !j_l->valuestring || !cJSON_IsString(j_v) || !j_v->valuestring) {
            ESP_LOGW(TAG, "FixedLabel item ignored: missing Label/Value strings");
            continue;
        }

        char nl[ANNA_MAX_FIXED_LABEL_STR_LEN] = {0};
        char nv[ANNA_MAX_FIXED_LABEL_STR_LEN] = {0};
        if (!fixed_label_normalize(j_l->valuestring, nl, sizeof(nl)) ||
            !fixed_label_normalize(j_v->valuestring, nv, sizeof(nv))) {
            ESP_LOGW(TAG, "FixedLabel item ignored: invalid string rule");
            continue;
        }

        uint8_t idx = *out_cnt;
        strlcpy(out[idx].label, nl, sizeof(out[idx].label));
        strlcpy(out[idx].value, nv, sizeof(out[idx].value));
        (*out_cnt)++;
    }
}

static void cfg_reset_defaults(void)
{
    memset(&g_anna_cfg, 0, sizeof(g_anna_cfg));
    g_anna_cfg.on_mode = -1;
    g_anna_cfg.modes.mode_count = -1;
    /* NOTE:
     * - 과거(legacy)에는 AnnaCount를 NVS 버전 키로 저장했으나, 현재는 사용하지 않는다.
     * - 비교를 위해 필드는 struct에 남겨두되, 로직은 주석 처리한다(삭제 금지).
     */
    // g_anna_cfg.product_info.anna_count = 1; /* legacy */
}

static esp_err_t parse_product_info_like_object(const cJSON *info_obj, bool is_product_info)
{
    if (!cJSON_IsObject(info_obj)) return ESP_ERR_INVALID_ARG;

    /* 키 매핑: ProductInfo는 가이드라인 스키마(AnnaJson.v1), DeviceInfo는 레거시 */
    const char *k_vendor_name = is_product_info ? "VendorName" : "VendorName";
    const char *k_vendor_id   = is_product_info ? "VendorID"   : "VendorID";
    const char *k_prod_name   = is_product_info ? "ProductName" : "ProductName";
    const char *k_prod_id     = is_product_info ? "ProductID"   : "ProductID";
    const char *k_node_label  = is_product_info ? "NodeLabel"   : "NodeLabel";
    const char *k_prod_label  = is_product_info ? "ProductLabel" : "ProductLabel";
    const char *k_location    = is_product_info ? "Location"    : "Location";
    const char *k_hw_ver      = is_product_info ? "HardwareVersion" : "HardwareVersion";
    const char *k_hw_ver_str  = is_product_info ? "HardwareVersionString" : "HardwareVersionString";
    const char *k_sw_ver      = is_product_info ? "SoftwareVersion" : "SoftwareVersion";
    const char *k_sw_ver_str  = is_product_info ? "SoftwareVersionString" : "SoftwareVersionString";
    // const char *k_serial      = is_product_info ? "SerialNumber" : "SerialNumber"; /* legacy */

    str_copy_or_empty(g_anna_cfg.product_info.vendor_name,
                      sizeof(g_anna_cfg.product_info.vendor_name),
                      cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_vendor_name));

    uint32_t u32 = 0;
    if (json_get_u32_flexible(info_obj, k_vendor_id, &u32)) g_anna_cfg.product_info.vendor_id = (uint16_t)u32;

    str_copy_or_empty(g_anna_cfg.product_info.product_name,
                      sizeof(g_anna_cfg.product_info.product_name),
                      cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_prod_name));
    if (json_get_u32_flexible(info_obj, k_prod_id, &u32)) g_anna_cfg.product_info.product_id = (uint16_t)u32;

    str_copy_or_empty(g_anna_cfg.product_info.node_label,
                      sizeof(g_anna_cfg.product_info.node_label),
                      cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_node_label));

    /* null 허용 문자열: ProductLabel/Location */
    const cJSON *j_pl = cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_prod_label);
    if (cJSON_IsNull(j_pl)) g_anna_cfg.product_info.product_label[0] = '\0';
    else str_copy_or_empty(g_anna_cfg.product_info.product_label, sizeof(g_anna_cfg.product_info.product_label), j_pl);

    const cJSON *j_loc = cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_location);
    if (cJSON_IsNull(j_loc)) g_anna_cfg.product_info.location[0] = '\0';
    else str_copy_or_empty(g_anna_cfg.product_info.location, sizeof(g_anna_cfg.product_info.location), j_loc);

    if (json_get_u32_flexible(info_obj, k_hw_ver, &u32)) g_anna_cfg.product_info.hw_ver = (uint16_t)u32;
    str_copy_or_empty(g_anna_cfg.product_info.hw_ver_str,
                      sizeof(g_anna_cfg.product_info.hw_ver_str),
                      cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_hw_ver_str));

    if (json_get_u32_flexible(info_obj, k_sw_ver, &u32)) g_anna_cfg.product_info.sw_ver = (uint32_t)u32;
    str_copy_or_empty(g_anna_cfg.product_info.sw_ver_str,
                      sizeof(g_anna_cfg.product_info.sw_ver_str),
                      cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_sw_ver_str));

    /* guideline.md 예시: ProductInfo.isCertified (optional, default=false) */
    const cJSON *j_cert = cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, "isCertified");
    g_anna_cfg.product_info.is_certified = cJSON_IsTrue(j_cert);

    /* SerialNumber는 product-info에서 제거됨: 비교를 위해 로직만 주석 처리 */
    // str_copy_or_empty(g_anna_cfg.product_info.serial_number,
    //                   sizeof(g_anna_cfg.product_info.serial_number),
    //                   cJSON_GetObjectItemCaseSensitive((cJSON *)info_obj, k_serial));

    /* AnnaCount는 product-info에서 제거됨: 비교를 위해 로직만 주석 처리 */
    // uint32_t ac = 0;
    // if (json_get_u32_flexible(info_obj, "AnnaCount", &ac)) {
    //     g_anna_cfg.product_info.anna_count = (uint8_t)ac;
    // }

    return ESP_OK;
}

static esp_err_t parse_button_array_cjson(const cJSON *arr, bool is_legacy_oneact)
{
    if (!arr) return ESP_OK; /* optional */
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        if (g_anna_cfg.button_cnt >= ANNA_MAX_BUTTON) break;
        anna_button_t *dst = &g_anna_cfg.a_button[g_anna_cfg.button_cnt++];
        memset(dst, 0, sizeof(*dst));
        /* IMPORTANT: PinNo가 누락되면 0이 아니라 UNDEF(0xFF)로 남겨야 한다.
         * (GPIO0이 유효한 보드에서는 “누락”이 “핀0”으로 오해되어 치명적 오동작 가능)
         */
        dst->base.pin_no = (uint8_t)ANNA_PIN_UNDEF;

        str_copy_or_empty_limit(dst->label, sizeof(dst->label),
                                cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Label"),
                                ANNA_MAX_USER_LABEL_STR_LEN - 1);

        uint32_t pin = 0;
        if (json_get_u32_flexible(it, "PinNo", &pin)) dst->base.pin_no = (uint8_t)pin;

        parse_fixed_label_array_cjson(it, dst->fixed_label, &dst->fixed_label_cnt);
        dst->base.not_on_mask  = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOnPin"));
        dst->base.not_off_mask = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOffPin"));
        (void)is_legacy_oneact;
    }
    return ESP_OK;
}

static esp_err_t parse_switch_array_cjson(const cJSON *arr, bool is_legacy_noract)
{
    if (!arr) return ESP_OK; /* optional */
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        if (g_anna_cfg.switch_cnt >= ANNA_MAX_SWITCH) break;
        anna_switch_t *dst = &g_anna_cfg.a_switch[g_anna_cfg.switch_cnt++];
        memset(dst, 0, sizeof(*dst));
        /* PinNo 누락 시 안전하게 UNDEF(0xFF)로 유지 */
        dst->base.pin_no = (uint8_t)ANNA_PIN_UNDEF;

        str_copy_or_empty_limit(dst->label, sizeof(dst->label),
                                cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Label"),
                                ANNA_MAX_USER_LABEL_STR_LEN - 1);

        uint32_t pin = 0;
        if (json_get_u32_flexible(it, "PinNo", &pin)) dst->base.pin_no = (uint8_t)pin;

        parse_fixed_label_array_cjson(it, dst->fixed_label, &dst->fixed_label_cnt);
        dst->base.not_on_mask  = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOnPin"));
        dst->base.not_off_mask = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOffPin"));
        (void)is_legacy_noract;
    }
    return ESP_OK;
}

static esp_err_t parse_modes_object_cjson(const cJSON *obj)
{
    if (!obj) return ESP_OK; /* optional */
    if (!cJSON_IsObject(obj)) return ESP_ERR_INVALID_ARG;

    int32_t mode_no = -1;
    (void)json_get_i32_nullable(obj, "ModeNo", -1, &mode_no);
    if (mode_no > ANNA_MAX_MODE_COUNT) mode_no = ANNA_MAX_MODE_COUNT;

    const cJSON *labels = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "Label");
    if (cJSON_IsArray(labels)) {
        int idx = 0;
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, labels) {
            if (idx >= ANNA_MAX_MODE_COUNT) break;
            if (cJSON_IsNull(it)) {
                g_anna_cfg.modes.labels[idx][0] = '\0';
            } else {
                str_copy_or_empty_limit(g_anna_cfg.modes.labels[idx], sizeof(g_anna_cfg.modes.labels[idx]), it,
                                        ANNA_MAX_USER_LABEL_STR_LEN - 1);
            }
            idx++;
        }
    }

    g_anna_cfg.modes.not_mode_mask = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "NotModePin"));
    g_anna_cfg.modes.mode_count = (int8_t)mode_no;

    if (mode_no <= 0) g_anna_cfg.on_mode = -1;
    else g_anna_cfg.on_mode = 0;

    return ESP_OK;
}

static void init_con_btn_defaults(anna_con_btn_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    /* IMPORTANT:
     * - PinNo 누락 시 0(GPIO0)로 오해되는 것을 방지하기 위해 UNDEF로 초기화한다.
     * - Button/Switch는 별도 파서에서 동일 정책을 이미 적용 중.
     */
    dst->base.pin_no = (uint8_t)ANNA_PIN_UNDEF;
    dst->mode_idx = -1;
    dst->bs_pin[0] = dst->bs_pin[1] = -1;
    dst->as_pin[0] = dst->as_pin[1] = -1;
    dst->as_tar[0] = dst->as_tar[1] = -1;
    dst->max_sec = -1;
}

static void init_con_swt_defaults(anna_con_swt_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    /* PinNo 누락 시 GPIO0 오동작 방지 */
    dst->base.pin_no = (uint8_t)ANNA_PIN_UNDEF;
    dst->mode_idx = -1;
    dst->bs_pin[0] = dst->bs_pin[1] = -1;
    dst->as_pin[0] = dst->as_pin[1] = -1;
    dst->as_tar[0] = dst->as_tar[1] = -1;
}

static esp_err_t parse_con_btn_array_cjson(const cJSON *arr)
{
    if (!arr) return ESP_OK; /* optional */
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        if (g_anna_cfg.con_btn_cnt >= ANNA_MAX_CON_ACT) break;

        anna_con_btn_t *dst = &g_anna_cfg.con_btn[g_anna_cfg.con_btn_cnt++];
        init_con_btn_defaults(dst);

        str_copy_or_empty_limit(dst->label, sizeof(dst->label),
                                cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Label"),
                                ANNA_MAX_USER_LABEL_STR_LEN - 1);

        parse_fixed_label_array_cjson(it, dst->fixed_label, &dst->fixed_label_cnt);
        uint32_t u32 = 0;
        if (json_get_u32_flexible(it, "PinNo", &u32)) dst->base.pin_no = (uint8_t)u32;

        int32_t i32 = -1;
        if (json_get_i32_nullable(it, "Mode", -1, &i32)) dst->mode_idx = (int8_t)i32;
        if (json_get_i32_nullable(it, "BSPinNo1", -1, &i32)) dst->bs_pin[0] = (int8_t)i32;
        dst->bs_evt[0] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "BSEvent1"));
        if (json_get_i32_nullable(it, "BSPinNo2", -1, &i32)) dst->bs_pin[1] = (int8_t)i32;
        dst->bs_evt[1] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "BSEvent2"));

        if (json_get_i32_nullable(it, "ASPinNo1", -1, &i32)) dst->as_pin[0] = (int8_t)i32;
        if (json_get_i32_nullable(it, "TarValue1", -1, &i32)) dst->as_tar[0] = (int16_t)i32;
        dst->as_evt[0] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "ASEvent1"));

        if (json_get_i32_nullable(it, "ASPinNo2", -1, &i32)) dst->as_pin[1] = (int8_t)i32;
        if (json_get_i32_nullable(it, "TarValue2", -1, &i32)) dst->as_tar[1] = (int16_t)i32;
        dst->as_evt[1] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "ASEvent2"));

        dst->base.not_on_mask  = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOnPin"));
        dst->base.not_off_mask = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOffPin"));

        if (json_get_i32_nullable(it, "MaxSec", -1, &i32)) dst->max_sec = (int16_t)i32;
    }
    return ESP_OK;
}

static esp_err_t parse_con_swt_array_cjson(const cJSON *arr)
{
    if (!arr) return ESP_OK; /* optional */
    if (!cJSON_IsArray(arr)) return ESP_ERR_INVALID_ARG;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;
        if (g_anna_cfg.con_swt_cnt >= ANNA_MAX_CON_SWT_ACT) break;

        anna_con_swt_t *dst = &g_anna_cfg.con_swt[g_anna_cfg.con_swt_cnt++];
        init_con_swt_defaults(dst);

        str_copy_or_empty_limit(dst->label, sizeof(dst->label),
                                cJSON_GetObjectItemCaseSensitive((cJSON *)it, "Label"),
                                ANNA_MAX_USER_LABEL_STR_LEN - 1);

        parse_fixed_label_array_cjson(it, dst->fixed_label, &dst->fixed_label_cnt);
        uint32_t u32 = 0;
        if (json_get_u32_flexible(it, "PinNo", &u32)) dst->base.pin_no = (uint8_t)u32;

        int32_t i32 = -1;
        if (json_get_i32_nullable(it, "Mode", -1, &i32)) dst->mode_idx = (int8_t)i32;
        if (json_get_i32_nullable(it, "BSPinNo1", -1, &i32)) dst->bs_pin[0] = (int8_t)i32;
        dst->bs_evt[0] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "BSEvent1"));
        if (json_get_i32_nullable(it, "BSPinNo2", -1, &i32)) dst->bs_pin[1] = (int8_t)i32;
        dst->bs_evt[1] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "BSEvent2"));

        if (json_get_i32_nullable(it, "ASPinNo1", -1, &i32)) dst->as_pin[0] = (int8_t)i32;
        if (json_get_i32_nullable(it, "TarValue1", -1, &i32)) dst->as_tar[0] = (int16_t)i32;
        dst->as_evt[0] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "ASEvent1"));

        if (json_get_i32_nullable(it, "ASPinNo2", -1, &i32)) dst->as_pin[1] = (int8_t)i32;
        if (json_get_i32_nullable(it, "TarValue2", -1, &i32)) dst->as_tar[1] = (int16_t)i32;
        dst->as_evt[1] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "ASEvent2"));

        dst->base.not_on_mask  = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOnPin"));
        dst->base.not_off_mask = build_pin_mask_cjson(cJSON_GetObjectItemCaseSensitive((cJSON *)it, "NotOffPin"));
    }
    return ESP_OK;
}

int anna_cfg_parse_json(const char *js, size_t len)
{
    if (!js || len == 0) return ESP_ERR_INVALID_ARG;

    /* 안전을 위해 무조건 len+1 버퍼로 복사하여 NUL-terminate 보장 */
    char *buf = (char *)malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, js, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "cJSON_Parse failed");
        free(buf);
        return ESP_FAIL;
    }
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "root is not object");
        cJSON_Delete(root);
        free(buf);
        return ESP_FAIL;
    }

    cfg_reset_defaults();

    /* 제품 정보: ProductInfo(신) 우선, DeviceInfo(구) fallback */
    const cJSON *j_pi = cJSON_GetObjectItemCaseSensitive(root, "ProductInfo");
    const cJSON *j_di = cJSON_GetObjectItemCaseSensitive(root, "DeviceInfo");
    esp_err_t derr = ESP_OK;
    if (cJSON_IsObject(j_pi)) {
        derr = parse_product_info_like_object(j_pi, true);
    } else if (cJSON_IsObject(j_di)) {
        derr = parse_product_info_like_object(j_di, false);
    } else {
        ESP_LOGE(TAG, "missing ProductInfo/DeviceInfo");
        cJSON_Delete(root);
        free(buf);
        return ESP_FAIL;
    }
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "device info parse failed");
        cJSON_Delete(root);
        free(buf);
        return ESP_FAIL;
    }

    /* AnnaCount는 더 이상 사용하지 않음(SoftwareVersion이 버전 체크 역할 수행) */
    // if (g_anna_cfg.product_info.anna_count == 0) {
    //     ESP_LOGW(TAG, "AnnaCount missing -> defaulting to 1");
    //     g_anna_cfg.product_info.anna_count = 1;
    // }

    /* 섹션 파싱: 신 스키마 우선 키, 레거시 키 fallback */
    const cJSON *j_btn = cJSON_GetObjectItemCaseSensitive(root, "Button");
    if (!cJSON_IsArray(j_btn)) j_btn = cJSON_GetObjectItemCaseSensitive(root, "OneAct");
    if (j_btn) {
        if (parse_button_array_cjson(j_btn, !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "Button"))) != ESP_OK) {
            ESP_LOGE(TAG, "Button/OneAct parse failed");
            cJSON_Delete(root);
            free(buf);
            return ESP_FAIL;
        }
    }

    const cJSON *j_swt = cJSON_GetObjectItemCaseSensitive(root, "Switch");
    if (!cJSON_IsArray(j_swt)) j_swt = cJSON_GetObjectItemCaseSensitive(root, "NorAct");
    if (j_swt) {
        if (parse_switch_array_cjson(j_swt, !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "Switch"))) != ESP_OK) {
            ESP_LOGE(TAG, "Switch/NorAct parse failed");
            cJSON_Delete(root);
            free(buf);
            return ESP_FAIL;
        }
    }

    const cJSON *j_modes = cJSON_GetObjectItemCaseSensitive(root, "Modes");
    if (j_modes) {
        if (parse_modes_object_cjson(j_modes) != ESP_OK) {
            ESP_LOGE(TAG, "Modes parse failed");
            cJSON_Delete(root);
            free(buf);
            return ESP_FAIL;
        }
    }

    const cJSON *j_con_btn = cJSON_GetObjectItemCaseSensitive(root, "ConButton");
    if (j_con_btn) {
        if (parse_con_btn_array_cjson(j_con_btn) != ESP_OK) {
            ESP_LOGE(TAG, "ConButton parse failed");
            cJSON_Delete(root);
            free(buf);
            return ESP_FAIL;
        }
    }

    const cJSON *j_con_swt = cJSON_GetObjectItemCaseSensitive(root, "ConSwitch");
    if (j_con_swt) {
        if (parse_con_swt_array_cjson(j_con_swt) != ESP_OK) {
            ESP_LOGE(TAG, "ConSwitch parse failed");
            cJSON_Delete(root);
            free(buf);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}
