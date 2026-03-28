#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char * log_tag; // optional log tag
} mode_manager_config_t;

esp_err_t mode_manager_init(const mode_manager_config_t * config);

// Thin wrapper for Phase-1: delegate to existing app logic
esp_err_t mode_manager_pre_update(uint16_t endpoint_id,
                                  uint32_t cluster_id,
                                  uint32_t attribute_id,
                                  esp_matter_attr_val_t * val);

void mode_manager_on_post_update(uint16_t endpoint_id,
                                 uint32_t cluster_id,
                                 uint32_t attribute_id);

#ifdef __cplusplus
}
#endif


