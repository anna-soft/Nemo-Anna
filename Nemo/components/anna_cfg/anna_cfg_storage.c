#include <nvs_flash.h>
#include <nvs.h>
#include <esp_check.h>
#include <esp_log.h>
#include "anna_cfg.h"
#include "anna_cfg_parse.h"
#include "freertos/FreeRTOS.h"


#define ANNA_RT_PARTITION_NAME "runtime_anna"
#define ANNA_CFG_NAMESPACE   "anna_cfg"
#define ANNA_CFG_KEY         "anna"
#define ANNA_CFG_STRUCT_KEY  "cfg_blob"
#define ANNA_CFG_SCHEMA_VERSION_KEY "schema_version"
#define ANNA_CFG_SW_VER_KEY        "sw_ver"
#define ANNA_CFG_MAX_SIZE    (32 * 1024)   // guard

anna_cfg_t g_anna_cfg; /* zero-init BSS */

static const char *TAG = "anna_cfg_storage";

static esp_err_t open_cfg(nvs_handle_t *out_handle, nvs_open_mode_t mode)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open_from_partition(ANNA_RT_PARTITION_NAME, ANNA_CFG_NAMESPACE, mode, out_handle);
}

static esp_err_t save_snapshot_internal(nvs_handle_t nvs_handle, const char *json, size_t json_len,
                                        const anna_cfg_t *cfg, const char *schema_version)
{
    if (!json || json_len == 0 || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (json_len > ANNA_CFG_MAX_SIZE) {
        ESP_LOGE(TAG, "config too large (%u)", (unsigned)json_len);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_set_blob(nvs_handle, ANNA_CFG_KEY, json, json_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(json) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, ANNA_CFG_STRUCT_KEY, cfg, sizeof(*cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(cfg_blob) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(nvs_handle, ANNA_CFG_SW_VER_KEY, cfg->product_info.sw_ver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(sw_ver=%lu) failed: %s",
                 (unsigned long)cfg->product_info.sw_ver, esp_err_to_name(err));
        return err;
    }

    if (schema_version && schema_version[0] != '\0') {
        err = nvs_set_str(nvs_handle, ANNA_CFG_SCHEMA_VERSION_KEY, schema_version);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_str(schema_version) failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    return nvs_commit(nvs_handle);
}

/* Call once, early in boot */
void anna_cfg_nvs_init(void)
{
    ESP_LOGI(TAG, "anna_cfg_nvs_init start");
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    err = nvs_flash_init_partition(CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init_partition(%s) failed: %s", CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL, esp_err_to_name(err));
    }
    err = nvs_flash_init_partition(ANNA_RT_PARTITION_NAME);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(ANNA_RT_PARTITION_NAME));
        ESP_ERROR_CHECK(nvs_flash_init_partition(ANNA_RT_PARTITION_NAME));
    } else {
        ESP_ERROR_CHECK(err);
    }
    
    ESP_LOGI(TAG, "anna_cfg_nvs_init end");
}

int anna_cfg_save_to_nvs(const char *json, size_t json_len)
{
    if (!json || json_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (json_len > ANNA_CFG_MAX_SIZE) {
        ESP_LOGE(TAG, "config too large (%u)", (unsigned)json_len);
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs_handle;
    ESP_RETURN_ON_ERROR(open_cfg(&nvs_handle, NVS_READWRITE), TAG, "open");

    esp_err_t err = nvs_set_blob(nvs_handle, ANNA_CFG_KEY, json, json_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, ANNA_CFG_STRUCT_KEY, &g_anna_cfg, sizeof(g_anna_cfg));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, ANNA_CFG_SW_VER_KEY, g_anna_cfg.product_info.sw_ver);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

int anna_cfg_save_snapshot_to_nvs(const char *json, size_t json_len, const anna_cfg_t *cfg, const char *schema_version)
{
    nvs_handle_t nvs_handle;
    ESP_RETURN_ON_ERROR(open_cfg(&nvs_handle, NVS_READWRITE), TAG, "open");
    esp_err_t err = save_snapshot_internal(nvs_handle, json, json_len, cfg, schema_version);
    nvs_close(nvs_handle);
    return err;
}

int anna_cfg_save_schema_version_to_nvs(const char *schema_version)
{
    if (!schema_version || schema_version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs_handle;
    ESP_RETURN_ON_ERROR(open_cfg(&nvs_handle, NVS_READWRITE),
                        TAG, "open");
    esp_err_t err = nvs_set_str(nvs_handle, ANNA_CFG_SCHEMA_VERSION_KEY, schema_version);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

int anna_cfg_load_raw_from_nvs(void *out, size_t *inout_len)
{
    if (!inout_len) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = open_cfg(&nvs_handle, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(nvs_handle, ANNA_CFG_KEY, out, inout_len);
    nvs_close(nvs_handle);
    return err;
}

int anna_cfg_load_schema_version_from_nvs(char *out, size_t *inout_len)
{
    if (!inout_len) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = open_cfg(&nvs_handle, NVS_READONLY);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(nvs_handle, ANNA_CFG_SCHEMA_VERSION_KEY, out, inout_len);
    nvs_close(nvs_handle);
    return err;
}

/* Load + parse */
int anna_cfg_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    size_t anna_len = 0;

    /* runtime_anna 하나만 사용한다. */
    esp_err_t err = open_cfg(&nvs_handle, NVS_READONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Open %s Failed: %s", ANNA_RT_PARTITION_NAME, esp_err_to_name(err));
        return err;
    }

    /* 1) 구조체 blob 우선 시도 (SoftwareVersion 키 존재 시) */
    uint32_t stored_sw_ver = 0;
    err = nvs_get_u32(nvs_handle, ANNA_CFG_SW_VER_KEY, &stored_sw_ver);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stored_sw_ver: %lu", (unsigned long)stored_sw_ver);
        size_t blob_sz = sizeof(g_anna_cfg);
        esp_err_t blob_err = nvs_get_blob(nvs_handle, ANNA_CFG_STRUCT_KEY, &g_anna_cfg, &blob_sz);
        ESP_LOGI(TAG, "blob_err: %s", esp_err_to_name(blob_err));
        ESP_LOGI(TAG, "blob_sz: %u, sizeof(g_anna_cfg): %u", (unsigned)blob_sz, (unsigned)sizeof(g_anna_cfg));
        if (blob_err == ESP_OK && blob_sz == sizeof(g_anna_cfg)) {
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Loaded anna_cfg struct blob (%u bytes)", (unsigned)blob_sz);
            return ESP_OK;
        }
    } else {
        /* 초기 부팅 시 키가 없을 수 있으므로 info 레벨로 완화 */
        ESP_LOGI(TAG, "nvs_get_u32(ANNA_CFG_SW_VER_KEY) not found/failed: %s", esp_err_to_name(err));
    }

    /* 2) JSON blob 로드 (runtime_anna) */
    err = nvs_get_blob(nvs_handle, ANNA_CFG_KEY, NULL, &anna_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "No JSON in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) Failed: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    if (anna_len == 0 || anna_len > ANNA_CFG_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid cfg anna_len %u", (unsigned)anna_len);
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    char *anna_buf = (char *)malloc(anna_len + 1);
    if (!anna_buf) {
        ESP_LOGE(TAG, "malloc(%u) failed", (unsigned)anna_len);
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(nvs_handle, ANNA_CFG_KEY, anna_buf, &anna_len);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(data) Failed: %s", esp_err_to_name(err));
        free(anna_buf);
        return err;
    }
    anna_buf[anna_len] = '\0';

    err = anna_cfg_parse_json(anna_buf, anna_len);
    if (err == ESP_OK) {
        size_t schema_len = 0;
        char *schema_buf = NULL;
        esp_err_t schema_err = anna_cfg_load_schema_version_from_nvs(NULL, &schema_len);
        if (schema_err == ESP_OK && schema_len > 0) {
            schema_buf = (char *)calloc(1, schema_len);
            if (!schema_buf) {
                free(anna_buf);
                return ESP_ERR_NO_MEM;
            }
            schema_err = anna_cfg_load_schema_version_from_nvs(schema_buf, &schema_len);
            if (schema_err != ESP_OK) {
                free(schema_buf);
                schema_buf = NULL;
            }
        }

        /* json, g_anna_cfg, sw_ver, schema_version 저장 */
        err = anna_cfg_save_snapshot_to_nvs(anna_buf, anna_len, &g_anna_cfg, schema_buf);
        free(schema_buf);
    }

    free(anna_buf);
    return err;
}
