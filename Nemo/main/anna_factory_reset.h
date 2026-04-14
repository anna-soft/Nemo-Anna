#pragma once

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANNA_FACTORY_RESET_SOURCE_NONE = 0,
    ANNA_FACTORY_RESET_SOURCE_LAST_FABRIC = 1,
    ANNA_FACTORY_RESET_SOURCE_POWER_CYCLE = 2,
} anna_factory_reset_source_t;

typedef struct {
    bool feature_enabled;
    bool persistent_pending;
    anna_factory_reset_source_t pending_source;
    uint8_t power_cycle_count;
} anna_factory_reset_persistent_state_t;

const char *anna_factory_reset_source_name(anna_factory_reset_source_t source);

bool anna_factory_reset_is_pending(void);
bool anna_factory_reset_is_requested(void);
anna_factory_reset_source_t anna_factory_reset_pending_source(void);
bool anna_factory_reset_power_cycle_feature_enabled(void);

void anna_factory_reset_clear_pending(void);

esp_err_t anna_factory_reset_arm(anna_factory_reset_source_t source);
esp_err_t anna_factory_reset_request(anna_factory_reset_source_t source);
esp_err_t anna_factory_reset_request_pending(void);
esp_err_t anna_factory_reset_prepare_power_cycle_state(bool is_provisioned,
                                                       anna_factory_reset_persistent_state_t *out_state);
esp_err_t anna_factory_reset_store_power_cycle_count(uint8_t count);
esp_err_t anna_factory_reset_store_pending_source(anna_factory_reset_source_t source);
esp_err_t anna_factory_reset_clear_persistent_state(void);

#ifdef __cplusplus
}
#endif
