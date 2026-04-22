#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h> 
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits (tune via Kconfig if needed) ---- */
#define ANNA_MAX_LABEL_LEN           32   /* long labels observed. */
#define ANNA_MAX_ACT                 10    /* OneAct + NorAct total <= 10 */
#define ANNA_MAX_BUTTON              10
#define ANNA_MAX_SWITCH             10
#define ANNA_MAX_CON_ACT             10   /* ConButton */
#define ANNA_MAX_CON_SWT_ACT         10   /* ConSwitchAct */
#define ANNA_MAX_MODE_COUNT           4   /* ModeNo 2~4 */
#define ANNA_MAX_DYNAMIC_ENDPOINT_COUNT (ANNA_MAX_MODE_COUNT + ANNA_MAX_BUTTON + ANNA_MAX_SWITCH + ANNA_MAX_CON_ACT + ANNA_MAX_CON_SWT_ACT)
#define ANNA_MAX_PROTECT_PINS        10   /* observed arrays up to 10 */
#define ANNA_PIN_UNDEF               -1

#define ANNA_MAX_NAME_LEN            32
#define ANNA_MAX_LOCATION_LEN        32
#define ANNA_MAX_VER_STR_LEN         64
#define ANNA_MAX_SERIAL_NUMBER_LEN   32
#define ANNA_MAX_UNIQUE_ID_LEN       32
#define ANNA_MAX_CON_BS_COUNT         2
#define ANNA_MAX_CON_AS_COUNT         2

/* ---- FixedLabel (Matter Fixed Label cluster) ----
 * Server schema:
 *  - key: FixedLabel (case-sensitive)
 *  - type: Array<{ Label: string; Value: string }>
 *  - max items: 5
 *  - string rules: 2~16 chars, [A-Za-z0-9_<>!=,:/+- ] only
 *                  (stored as lowercase, space-normalized, trimmed)
 */
#define ANNA_MAX_FIXED_LABEL          5
#define ANNA_MAX_FIXED_LABEL_STR_LEN  17  /* 16 + NUL */
/* UserLabel cluster stores each label/value up to 16 bytes (plus NUL in local C buffer). */
#define ANNA_MAX_USER_LABEL_STR_LEN   17  /* 16 + NUL */

typedef struct {
    char label[ANNA_MAX_FIXED_LABEL_STR_LEN];
    char value[ANNA_MAX_FIXED_LABEL_STR_LEN];
} anna_fixed_label_kv_t;

/*--------- 1) 공통 정의 ----------*/
typedef enum {
    FT_CLASS_PRODUCT_INFO = 0x10,
    FT_CLASS_BOOL_SENS   = 0x20,
    FT_CLASS_ADC_SENS    = 0x30,
    FT_CLASS_I2C_SENS    = 0x40,
    FT_CLASS_SET_TARGET  = 0x50,
    FT_CLASS_BUTTON      = 0x60,
    FT_CLASS_SWITCH      = 0x70,
    FT_CLASS_MODES       = 0x80,
    FT_CLASS_CON_BTN_ACT = 0x90,
    FT_CLASS_CON_SWT_ACT = 0xA0,
} func_class_t;

typedef enum {
    FT_PRODUCT_INFO      = FT_CLASS_PRODUCT_INFO | 0x01,
    FT_BOOL_SENS         = FT_CLASS_BOOL_SENS   | 0x01,
    FT_ADC_SENS          = FT_CLASS_ADC_SENS    | 0x01,
    FT_I2C_SENS          = FT_CLASS_I2C_SENS    | 0x01,
    FT_SET_TARGET        = FT_CLASS_SET_TARGET  | 0x01,
    FT_BUTTON            = FT_CLASS_BUTTON      | 0x01,
    FT_SWITCH            = FT_CLASS_SWITCH      | 0x01,
    FT_MODES             = FT_CLASS_MODES       | 0x01,
    FT_CON_BTN_ACT       = FT_CLASS_CON_BTN_ACT | 0x01,
    FT_CON_SWT_ACT       = FT_CLASS_CON_SWT_ACT | 0x01,
} func_type_t;

/* 보호 핀 →  0~31번이면 32-bit 마스크로 충분 */
typedef uint32_t pin_mask_t;

/*--------- 2) 개별 구조체 ----------*/
/* ---- Nemo Info ---- */
typedef struct {
    uint16_t data_model_revision;
    uint8_t  capability_minima_subscriptions_per_client;
    uint8_t  capability_minima_fabrics;
    uint32_t specification_version;
    uint8_t  max_paths_per_invoke;
    char     unique_id[ANNA_MAX_UNIQUE_ID_LEN];
} nemo_info_t;

/* ---- Product Info ---- */
typedef struct {
    char     vendor_name[ANNA_MAX_NAME_LEN];
    uint16_t vendor_id;
    char     product_name[ANNA_MAX_NAME_LEN];
    uint16_t product_id;
    char     node_label[ANNA_MAX_LABEL_LEN];
    char     product_label[ANNA_MAX_LABEL_LEN]; /* json: null 입력 가능, "" */
    char     location[ANNA_MAX_LOCATION_LEN];   /* json: null 입력 가능, "" */
    uint16_t hw_ver;
    char     hw_ver_str[ANNA_MAX_VER_STR_LEN];
    uint32_t sw_ver;
    char     sw_ver_str[ANNA_MAX_VER_STR_LEN];
    /* guideline.md product-info.json 예시의 ProductInfo.isCertified */
    bool     is_certified;
} anna_product_info_t;

/* 공통 출력 베이스 */
typedef struct {
    pin_mask_t not_on_mask;  /* json: null 입력 가능, 0 */
    pin_mask_t not_off_mask; /* json: null 입력 가능, 0 */
    uint8_t    pin_no;
    uint16_t   endpoint_id;
} anna_base_act_t;

/* ---- Simple Actuator: OneAct (pulse) ---- */
typedef struct {
    char            label[ANNA_MAX_LABEL_LEN];
    anna_base_act_t base;
    uint8_t         fixed_label_cnt;
    anna_fixed_label_kv_t fixed_label[ANNA_MAX_FIXED_LABEL];
} anna_button_t;

/* ---- Latched Actuator: NorAct ---- */
typedef struct {
    char            label[ANNA_MAX_LABEL_LEN];
    anna_base_act_t base;
    uint8_t         fixed_label_cnt;
    anna_fixed_label_kv_t fixed_label[ANNA_MAX_FIXED_LABEL];
} anna_switch_t;

/* ---- Modes ---- */
typedef struct {
    int8_t     mode_count;                                      /* json: null 입력 가능, -1 */
    char       labels[ANNA_MAX_MODE_COUNT][ANNA_MAX_LABEL_LEN]; /* json: null 입력 가능, "" */
    pin_mask_t not_mode_mask;                                   /* json: null 입력 가능, 0 */
    uint16_t   endpoint_id[ANNA_MAX_MODE_COUNT];
} anna_modes_t;

/* ---- Conditional Actions (pulse) ---- */
typedef struct {
    char            label[ANNA_MAX_LABEL_LEN];
    bool            is_on;
    anna_base_act_t base;
    uint8_t         fixed_label_cnt;
    anna_fixed_label_kv_t fixed_label[ANNA_MAX_FIXED_LABEL];
    int8_t          mode_idx;                      /* json: null 입력 가능, -1 */
    int8_t          bs_pin[ANNA_MAX_CON_BS_COUNT]; /* json: null 입력 가능, -1 */
    bool            bs_evt[ANNA_MAX_CON_BS_COUNT]; /* json: null 입력 가능, false */ 
    int8_t          as_pin[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, -1 */
    int16_t         as_tar[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, -1 */
    bool            as_evt[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, false */
    int16_t         max_sec;                       /* json: null 입력 가능, -1 */
} anna_con_btn_t;

typedef struct {
    char            label[ANNA_MAX_LABEL_LEN];
    anna_base_act_t base;
    uint8_t         fixed_label_cnt;
    anna_fixed_label_kv_t fixed_label[ANNA_MAX_FIXED_LABEL];
    int8_t          mode_idx;                      /* json: null 입력 가능, -1 */
    int8_t          bs_pin[ANNA_MAX_CON_BS_COUNT]; /* json: null 입력 가능, -1 */
    bool            bs_evt[ANNA_MAX_CON_BS_COUNT]; /* json: null 입력 가능, false */ 
    int8_t          as_pin[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, -1 */
    int16_t         as_tar[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, -1 */
    bool            as_evt[ANNA_MAX_CON_AS_COUNT]; /* json: null 입력 가능, false */
} anna_con_swt_t;

/* ---- Full config ---- */
typedef struct {
    nemo_info_t        nemo_info;
    anna_product_info_t product_info;

    /* arrays */
    anna_button_t      a_button[ANNA_MAX_BUTTON];
    uint8_t            button_cnt;                    /* button_cnt + switch_cnt <= 8 */

    anna_switch_t      a_switch[ANNA_MAX_SWITCH];
    uint8_t            switch_cnt;                    /* button_cnt + switch_cnt <= 8 */

    anna_modes_t       modes;                         /* at most 1 */

    anna_con_btn_t     con_btn[ANNA_MAX_CON_ACT];
    uint8_t            con_btn_cnt;                   /* at most 10 */

    anna_con_swt_t     con_swt[ANNA_MAX_CON_SWT_ACT];
    uint8_t            con_swt_cnt;                   /* at most 10 */

    int8_t             on_mode;                       /* -1 ~ 3 */
    pin_mask_t         on_pin;                        /* pinNo: 10, 11, 18, 19, 20, 21, 22, 23 */
} anna_cfg_t;

/* Global runtime instance */
extern anna_cfg_t g_anna_cfg;

/* API */
int  anna_cfg_load_from_nvs(void);             /* read + parse */
int  anna_cfg_save_to_nvs(const char *json, size_t len);
int  anna_cfg_save_snapshot_to_nvs(const char *json, size_t len, const anna_cfg_t *cfg, const char *schema_version);
int  anna_cfg_save_schema_version_to_nvs(const char *schema_version);
int  anna_cfg_load_raw_from_nvs(void *out, size_t *inout_len); /* out may be NULL to query size */
int  anna_cfg_load_schema_version_from_nvs(char *out, size_t *inout_len); /* out may be NULL to query size */
void anna_cfg_nvs_init(void);

#ifdef __cplusplus
} /* extern "C" */

chip::ChipError set_all_user_label(void);
chip::ChipError set_user_label(chip::EndpointId ep, const char * label);

/* Build matter node & endpoints */
int  anna_cfg_apply_to_matter(const anna_cfg_t *cfg);

#endif
