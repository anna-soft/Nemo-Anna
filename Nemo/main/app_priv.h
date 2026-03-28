/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>
#include <driver/gpio.h>
#include "led_controller.h"
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

#define GPIO_RGB_CONTROLLER 8 /* Using GPIO 45 for the addressable led_strip */
extern led_controller_handle_t g_led_handle;

enum { 
    ENDPOINT_ID_INVALID = 0xFFFF,
};


void settings_init(esp_matter::node_t *node);

void settings_post_esp_start_init(void);

esp_err_t app_driver_led_init();

typedef void *app_driver_handle_t;

/** Driver Update
 *
 * This API should be called to update the driver for the attribute being updated.
 * This is usually called from the common `app_attribute_update_cb()`.
 *
 * @param[in] endpoint_id Endpoint ID of the attribute.
 * @param[in] cluster_id Cluster ID of the attribute.
 * @param[in] attribute_id Attribute ID of the attribute.
 * @param[in] val Pointer to `esp_matter_attr_val_t`. Use appropriate elements as per the value type.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val);

/** Set defaults for light driver
 *
 * Set the attribute drivers to their default values from the created data model.
 *
 * @param[in] endpoint_id Endpoint ID of the driver.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id);

/* Post-update handler to enforce mode invariants after attributes are committed */
esp_err_t app_driver_attribute_post_update(uint16_t endpoint_id, uint32_t cluster_id,
                                           uint32_t attribute_id, esp_matter_attr_val_t *val);

/* Immediate apply (PlatformMgr work queue) */
void app_driver_queue_mode_apply(void);
#ifdef __cplusplus
extern "C" void app_driver_boot_safe_off_sync_non_mode(void);
extern "C" {
#else
void app_driver_boot_safe_off_sync_non_mode(void);
#endif

#ifdef __cplusplus
}
#endif

/* Mark button GPIO index as initialized (called from settings) */
#ifdef __cplusplus
extern "C" void app_driver_mark_button_gpio_inited(int idx);
extern "C" {
#else
void app_driver_mark_button_gpio_inited(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* Mark switch GPIO index as initialized (called from settings) */
#ifdef __cplusplus
extern "C" void app_driver_mark_switch_gpio_inited(int idx);
extern "C" {
#else
void app_driver_mark_switch_gpio_inited(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* Mark con_btn GPIO index as initialized (called from settings) */
#ifdef __cplusplus
extern "C" void app_driver_mark_con_btn_gpio_inited(int idx);
extern "C" {
#else
void app_driver_mark_con_btn_gpio_inited(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* Mark con_swt GPIO index as initialized (called from settings) */
#ifdef __cplusplus
extern "C" void app_driver_mark_con_swt_gpio_inited(int idx);
extern "C" {
#else
void app_driver_mark_con_swt_gpio_inited(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* ConBtn helper: force OFF (used by mode change & bs IRQ work) */
#ifdef __cplusplus
extern "C" void app_driver_con_btn_force_off_by_index(int idx);
extern "C" {
#else
void app_driver_con_btn_force_off_by_index(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* ConSwt helper: force OFF (used by mode change & bs IRQ work) */
#ifdef __cplusplus
extern "C" void app_driver_con_swt_force_off_by_index(int idx);
extern "C" {
#else
void app_driver_con_swt_force_off_by_index(int idx);
#endif

#ifdef __cplusplus
}
#endif

/* ConSwt helper: maintain-only group ON by endpoint (no auto-ON creation) */
#ifdef __cplusplus
extern "C" bool app_driver_con_swt_maintain_group_on_if_satisfied(uint16_t endpoint_id);
extern "C" {
#else
bool app_driver_con_swt_maintain_group_on_if_satisfied(uint16_t endpoint_id);
#endif

#ifdef __cplusplus
}
#endif

/* ConBtn helper: maintain-only group ON by endpoint (no auto-ON creation) */
#ifdef __cplusplus
extern "C" bool app_driver_con_btn_maintain_group_on_if_satisfied(uint16_t endpoint_id);
extern "C" {
#else
bool app_driver_con_btn_maintain_group_on_if_satisfied(uint16_t endpoint_id);
#endif

#ifdef __cplusplus
}
#endif

/* ConSwt group: schedule group reconcile by endpoint (Label-level OR evaluation) */
#ifdef __cplusplus
extern "C" void app_driver_con_swt_group_reconcile_by_endpoint(uint16_t endpoint_id);
extern "C" {
#else
void app_driver_con_swt_group_reconcile_by_endpoint(uint16_t endpoint_id);
#endif

#ifdef __cplusplus
}
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif

/* Cross-TU hooks implemented in app_settings.cpp */
#ifdef __cplusplus
extern "C" {
#endif
bool app_read_analog_scaled(uint8_t gpio, int * out_scaled);
int  app_bs_get_debounced_level(int pin);
#ifdef __cplusplus
}
#endif
