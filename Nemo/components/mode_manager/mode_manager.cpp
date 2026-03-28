#include "mode_manager.h"

#include <esp_log.h>
// Phase-1: keep this component independent from the application component

// We forward to existing app-side functions to keep behavior identical in Phase-1

static const char * s_mode_mgr_tag = "mode_manager";

esp_err_t mode_manager_init(const mode_manager_config_t * config)
{
    if (config && config->log_tag) {
        s_mode_mgr_tag = config->log_tag;
    }
    ESP_LOGI(s_mode_mgr_tag, "mode_manager initialized");
    return ESP_OK;
}

esp_err_t mode_manager_pre_update(uint16_t /*endpoint_id*/,
                                  uint32_t /*cluster_id*/,
                                  uint32_t /*attribute_id*/,
                                  esp_matter_attr_val_t * /*val*/)
{
    // Phase-1: thin boundary only; let app_main call the real driver
    return ESP_OK;
}

void mode_manager_on_post_update(uint16_t /*endpoint_id*/,
                                 uint32_t /*cluster_id*/,
                                 uint32_t /*attribute_id*/)
{
    // Phase-1: no-op; app_main retains its original POST handling
}


