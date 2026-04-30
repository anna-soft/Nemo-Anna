#include "anna_cloud_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs.h>

#include "anna_state_storage.h"

namespace {
constexpr char TAG[] = "anna_cloud_id";
constexpr char kRuntimePartition[] = "runtime_anna";
constexpr char kCfgNamespace[] = "anna_cfg";
constexpr char kMacKey[] = "mac_addr";

static esp_err_t read_mac_string_fallback(char out[ANNA_CLOUD_MAC_STRING_LEN])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        err = esp_efuse_mac_get_default(mac);
    }
    if (err != ESP_OK) {
        return err;
    }

    snprintf(out, ANNA_CLOUD_MAC_STRING_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return ESP_OK;
}

static void trim_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (isspace(static_cast<unsigned char>(*src))) {
        ++src;
    }

    size_t src_len = strlen(src);
    while (src_len > 0 && isspace(static_cast<unsigned char>(src[src_len - 1]))) {
        --src_len;
    }
    if (src_len >= dst_len) {
        src_len = dst_len - 1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}
} // namespace

extern "C" esp_err_t anna_cloud_get_runtime_mac(char out[ANNA_CLOUD_MAC_STRING_LEN])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kRuntimePartition, kCfgNamespace, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t mac_len = ANNA_CLOUD_MAC_STRING_LEN;
        err = nvs_get_str(handle, kMacKey, out, &mac_len);
        nvs_close(handle);
        if (err == ESP_OK) {
            for (size_t i = 0; i < strlen(out); ++i) {
                out[i] = static_cast<char>(toupper(static_cast<unsigned char>(out[i])));
            }
            return ESP_OK;
        }
    }

    err = read_mac_string_fallback(out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime mac fallback failed: %s", esp_err_to_name(err));
    }
    return err;
}

extern "C" esp_err_t anna_cloud_load_bootstrap(anna_cloud_bootstrap_t *out_bootstrap)
{
    if (!out_bootstrap) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_bootstrap, 0, sizeof(*out_bootstrap));
    if (!anna_state_has_unit_blob()) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t raw_len = 0;
    int rc = anna_state_get_unit_blob(NULL, &raw_len);
    if (rc != ESP_OK || raw_len == 0) {
        return rc == ESP_OK ? ESP_ERR_NOT_FOUND : rc;
    }

    char *raw = static_cast<char *>(calloc(1, raw_len + 1));
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    size_t inout = raw_len;
    rc = anna_state_get_unit_blob(raw, &inout);
    if (rc != ESP_OK || inout == 0) {
        free(raw);
        return rc == ESP_OK ? ESP_ERR_NOT_FOUND : rc;
    }
    raw[inout] = '\0';

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }

    const cJSON *unit_info = cJSON_GetObjectItemCaseSensitive(root, "UnitInfo");
    const cJSON *unique_id = cJSON_GetObjectItemCaseSensitive(unit_info, "UniqueID");
    const cJSON *cloud_sync = cJSON_GetObjectItemCaseSensitive(unit_info, "CloudSync");
    const cJSON *device_token = cJSON_GetObjectItemCaseSensitive(cloud_sync, "DeviceToken");

    if (cJSON_IsString(unique_id) && unique_id->valuestring) {
        trim_copy(out_bootstrap->unique_id, sizeof(out_bootstrap->unique_id), unique_id->valuestring);
        out_bootstrap->has_unique_id = out_bootstrap->unique_id[0] != '\0';
    }
    if (cJSON_IsString(device_token) && device_token->valuestring) {
        trim_copy(out_bootstrap->device_token, sizeof(out_bootstrap->device_token), device_token->valuestring);
        out_bootstrap->has_device_token = out_bootstrap->device_token[0] != '\0';
    }

    cJSON_Delete(root);
    if (!out_bootstrap->has_unique_id && !out_bootstrap->has_device_token) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
