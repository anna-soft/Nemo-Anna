#include "anna_factory_reset.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <nvs.h>

namespace {

constexpr const char *kTag = "anna_factory_reset";
constexpr const char *kResetNamespace = "anna_reset";
constexpr const char *kKeyPowerCycleCount = "pwr_cycle_cnt";
constexpr const char *kKeyPendingReset = "pending_reset";
constexpr const char *kKeyPendingSource = "pending_source";

static_assert(sizeof("pwr_cycle_cnt") <= NVS_KEY_NAME_MAX_SIZE, "power-cycle key exceeds NVS max key length");

struct AnnaFactoryResetState {
    bool pending = false;
    bool requested = false;
    anna_factory_reset_source_t source = ANNA_FACTORY_RESET_SOURCE_NONE;
    bool power_cycle_feature_enabled = false;
};

AnnaFactoryResetState s_factory_reset_state;

void clear_pending_locked()
{
    s_factory_reset_state.pending = false;
    s_factory_reset_state.source = ANNA_FACTORY_RESET_SOURCE_NONE;
}

esp_err_t open_reset_namespace(nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(kResetNamespace, mode, out_handle);
}

esp_err_t read_u8_or_default(nvs_handle_t handle, const char *key, uint8_t default_value, uint8_t *out_value)
{
    if (!key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = default_value;
    esp_err_t err = nvs_get_u8(handle, key, &value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = default_value;
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        *out_value = value;
    }
    return err;
}

bool is_valid_persistent_source(uint8_t raw_source)
{
    return raw_source == static_cast<uint8_t>(ANNA_FACTORY_RESET_SOURCE_NONE) ||
           raw_source == static_cast<uint8_t>(ANNA_FACTORY_RESET_SOURCE_LAST_FABRIC) ||
           raw_source == static_cast<uint8_t>(ANNA_FACTORY_RESET_SOURCE_POWER_CYCLE);
}

} // namespace

extern "C" const char *anna_factory_reset_source_name(anna_factory_reset_source_t source)
{
    switch (source) {
    case ANNA_FACTORY_RESET_SOURCE_LAST_FABRIC:
        return "last_fabric";
    case ANNA_FACTORY_RESET_SOURCE_POWER_CYCLE:
        return "power_cycle";
    case ANNA_FACTORY_RESET_SOURCE_NONE:
    default:
        return "none";
    }
}

extern "C" bool anna_factory_reset_is_pending(void)
{
    return s_factory_reset_state.pending;
}

extern "C" bool anna_factory_reset_is_requested(void)
{
    return s_factory_reset_state.requested;
}

extern "C" anna_factory_reset_source_t anna_factory_reset_pending_source(void)
{
    return s_factory_reset_state.source;
}

extern "C" bool anna_factory_reset_power_cycle_feature_enabled(void)
{
    return s_factory_reset_state.power_cycle_feature_enabled;
}

extern "C" void anna_factory_reset_clear_pending(void)
{
    clear_pending_locked();
}

extern "C" esp_err_t anna_factory_reset_arm(anna_factory_reset_source_t source)
{
    if (source == ANNA_FACTORY_RESET_SOURCE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_factory_reset_state.requested) {
        ESP_LOGW(kTag, "factory reset already requested, ignore arm: source=%s",
                 anna_factory_reset_source_name(source));
        return ESP_ERR_INVALID_STATE;
    }

    if (s_factory_reset_state.pending) {
        ESP_LOGW(kTag, "factory reset already pending: current=%s new=%s",
                 anna_factory_reset_source_name(s_factory_reset_state.source),
                 anna_factory_reset_source_name(source));
        return (s_factory_reset_state.source == source) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_factory_reset_state.pending = true;
    s_factory_reset_state.source = source;
    ESP_LOGI(kTag, "factory reset armed: source=%s", anna_factory_reset_source_name(source));
    return ESP_OK;
}

extern "C" esp_err_t anna_factory_reset_request(anna_factory_reset_source_t source)
{
    if (source == ANNA_FACTORY_RESET_SOURCE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_factory_reset_state.requested) {
        ESP_LOGW(kTag, "factory reset already requested, ignore request: source=%s",
                 anna_factory_reset_source_name(source));
        return ESP_OK;
    }

    esp_err_t err = anna_factory_reset_clear_persistent_state();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "factory reset cleanup failed: source=%s err=%s",
                 anna_factory_reset_source_name(source), esp_err_to_name(err));
        clear_pending_locked();
        return err;
    }

    ESP_LOGW(kTag, "factory reset request start: source=%s", anna_factory_reset_source_name(source));
    s_factory_reset_state.requested = true;
    clear_pending_locked();

    err = esp_matter::factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "factory reset request failed: source=%s err=%s",
                 anna_factory_reset_source_name(source), esp_err_to_name(err));
        s_factory_reset_state.requested = false;
        return err;
    }

    return ESP_OK;
}

extern "C" esp_err_t anna_factory_reset_request_pending(void)
{
    if (!s_factory_reset_state.pending) {
        return ESP_OK;
    }
    return anna_factory_reset_request(s_factory_reset_state.source);
}

extern "C" esp_err_t anna_factory_reset_prepare_power_cycle_state(
    bool is_provisioned, anna_factory_reset_persistent_state_t *out_state)
{
    if (out_state) {
        *out_state = {};
        out_state->pending_source = ANNA_FACTORY_RESET_SOURCE_NONE;
    }

    s_factory_reset_state.power_cycle_feature_enabled = false;

    if (!is_provisioned) {
        clear_pending_locked();
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = open_reset_namespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "anna_reset namespace unavailable, disable power-cycle feature: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t power_cycle_count = 0;
    uint8_t pending_reset = 0;
    uint8_t pending_source_raw = static_cast<uint8_t>(ANNA_FACTORY_RESET_SOURCE_NONE);

    err = read_u8_or_default(handle, kKeyPowerCycleCount, 0, &power_cycle_count);
    if (err == ESP_OK) {
        err = read_u8_or_default(handle, kKeyPendingReset, 0, &pending_reset);
    }
    if (err == ESP_OK) {
        err = read_u8_or_default(handle, kKeyPendingSource, pending_source_raw, &pending_source_raw);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "anna_reset state load failed, disable power-cycle feature: %s", esp_err_to_name(err));
        return err;
    }

    s_factory_reset_state.power_cycle_feature_enabled = true;

    anna_factory_reset_source_t pending_source = ANNA_FACTORY_RESET_SOURCE_NONE;
    if (pending_reset != 0) {
        if (!is_valid_persistent_source(pending_source_raw) ||
            pending_source_raw == static_cast<uint8_t>(ANNA_FACTORY_RESET_SOURCE_NONE)) {
            ESP_LOGW(kTag, "Ignore invalid persistent pending source=%u", static_cast<unsigned>(pending_source_raw));
        } else {
            pending_source = static_cast<anna_factory_reset_source_t>(pending_source_raw);
            s_factory_reset_state.pending = true;
            s_factory_reset_state.source = pending_source;
        }
    }

    if (out_state) {
        out_state->feature_enabled = true;
        out_state->persistent_pending = (pending_source != ANNA_FACTORY_RESET_SOURCE_NONE);
        out_state->pending_source = pending_source;
        out_state->power_cycle_count = power_cycle_count;
    }

    ESP_LOGI(kTag, "power-cycle state loaded: count=%u pending=%u source=%s",
             static_cast<unsigned>(power_cycle_count),
             (pending_source != ANNA_FACTORY_RESET_SOURCE_NONE) ? 1U : 0U,
             anna_factory_reset_source_name(pending_source));
    return ESP_OK;
}

extern "C" esp_err_t anna_factory_reset_store_power_cycle_count(uint8_t count)
{
    if (!s_factory_reset_state.power_cycle_feature_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = open_reset_namespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, kKeyPowerCycleCount, count);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t anna_factory_reset_store_pending_source(anna_factory_reset_source_t source)
{
    if (!s_factory_reset_state.power_cycle_feature_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (source == ANNA_FACTORY_RESET_SOURCE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = open_reset_namespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, kKeyPendingReset, 1);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kKeyPendingSource, static_cast<uint8_t>(source));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t anna_factory_reset_clear_persistent_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = open_reset_namespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, kKeyPowerCycleCount, 0);
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(handle, kKeyPendingReset);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(handle, kKeyPendingSource);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
