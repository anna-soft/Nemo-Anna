#pragma once

#include <esp_err.h>

#include "anna_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANNA_CLOUD_MAC_STRING_LEN 18
#define ANNA_MAX_DEVICE_TOKEN_LEN 128

typedef struct {
    char unique_id[ANNA_MAX_UNIQUE_ID_LEN];
    char device_token[ANNA_MAX_DEVICE_TOKEN_LEN];
    bool has_unique_id;
    bool has_device_token;
} anna_cloud_bootstrap_t;

esp_err_t anna_cloud_get_runtime_mac(char out[ANNA_CLOUD_MAC_STRING_LEN]);
esp_err_t anna_cloud_load_bootstrap(anna_cloud_bootstrap_t *out_bootstrap);

#ifdef __cplusplus
}
#endif
