#include "anna_state_storage.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_log.h>

#define ANNA_RT_PARTITION_NAME "runtime_anna"
#define ANNA_STATE_NAMESPACE   "anna_state"
#define ANNA_STATE_KEY_PROV    "provisioned"
#define ANNA_STATE_KEY_UNIT    "unit_blob"

static const char *TAG = "anna_state_storage";

static esp_err_t open_state(nvs_handle_t *out, nvs_open_mode_t mode)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    return nvs_open_from_partition(ANNA_RT_PARTITION_NAME, ANNA_STATE_NAMESPACE, mode, out);
}

bool anna_state_is_provisioned(void)
{
    nvs_handle_t h;
    esp_err_t err = open_state(&h, NVS_READONLY);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, ANNA_STATE_KEY_PROV, &v);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    return v == 1;
}

int anna_state_set_provisioned(uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = open_state(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open_state failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t vv = (v ? 1 : 0);
    err = nvs_set_u8(h, ANNA_STATE_KEY_PROV, vv);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int anna_state_set_unit_blob(const void *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_state(&h, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open_state failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, ANNA_STATE_KEY_UNIT, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool anna_state_has_unit_blob(void)
{
    nvs_handle_t h;
    esp_err_t err = open_state(&h, NVS_READONLY);
    if (err != ESP_OK) return false;
    size_t sz = 0;
    err = nvs_get_blob(h, ANNA_STATE_KEY_UNIT, NULL, &sz);
    nvs_close(h);
    return err == ESP_OK && sz > 0;
}

int anna_state_get_unit_blob(void *out, size_t *inout_len)
{
    if (!inout_len) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_state(&h, NVS_READONLY);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, ANNA_STATE_KEY_UNIT, out, inout_len);
    nvs_close(h);
    return err;
}


