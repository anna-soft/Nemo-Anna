/*
 * led_controller.c
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "driver/rmt.h"

#include "led_controller.h"

static const char *TAG = "led_controller";

// Struct to hold controller's internal data
struct led_controller_s {
    led_strip_t *strip_handle;
    TimerHandle_t timer_handle;
    led_mode_t current_mode;
    bool is_on;
};

// Array of RGB colors corresponding to led_mode_t
// Using low brightness values as in the example
static const uint8_t led_colors[LED_MODE_LEN][3] = {
    {0, 0, 30},   // Blue   (조금 더 밝게)
    {30, 30, 0},  // Yellow
    {30, 0, 30},  // Purple
    {30, 0, 0},   // Red
};

esp_err_t led_controller_init(int gpio_num, led_controller_handle_t* p_handle)
{
    if (!p_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    led_controller_handle_t handle = calloc(1, sizeof(struct led_controller_s));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initializing WS2812 LED on GPIO %d", gpio_num);

    // 1) Configure RMT TX channel (channel 0 fixed)
    rmt_config_t rmt_cfg = {
        .rmt_mode = RMT_MODE_TX,
        .channel = RMT_CHANNEL_0,
        .gpio_num = gpio_num,
        .clk_div = 2,                 // 40MHz/2 = 20MHz -> fine for WS2812 timings
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_output_en = true,
            .idle_level = RMT_IDLE_LEVEL_LOW,
        },
    };
    ESP_ERROR_CHECK(rmt_config(&rmt_cfg));
    ESP_ERROR_CHECK(rmt_driver_install(rmt_cfg.channel, 0, 0));

    // 2) Create led_strip driver instance (max 1 LED)
    led_strip_config_t strip_cfg = LED_STRIP_DEFAULT_CONFIG(1, (void *)rmt_cfg.channel);
    handle->strip_handle = led_strip_new_rmt_ws2812(&strip_cfg);
    if (!handle->strip_handle) {
        ESP_LOGE(TAG, "Failed to create led strip instance");
        return ESP_FAIL;
    }

    handle->strip_handle->clear(handle->strip_handle, 100);

    // 3) Timer for auto-off
    // handle->timer_handle = xTimerCreate("led_off_timer", pdMS_TO_TICKS(100), pdFALSE, handle, turn_off_timer_callback);
    // if (!handle->timer_handle) {
    //     handle->strip_handle->del(handle->strip_handle);
    //     return ESP_FAIL;
    // }

    handle->is_on = false;
    handle->current_mode = LED_MODE_1_BLUE;

    *p_handle = handle;
    return ESP_OK;
}

esp_err_t led_controller_deinit(led_controller_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    xTimerDelete(handle->timer_handle, portMAX_DELAY);
    handle->strip_handle->del(handle->strip_handle);
    free(handle);
    return ESP_OK;
}

// esp_err_t led_controller_set_mode(led_controller_handle_t handle, led_mode_t mode)
// {
//     if (!handle || mode >= LED_MODE_LEN) return ESP_ERR_INVALID_ARG;
    
//     ESP_LOGI(TAG, "Setting mode to %d", mode);
//     const uint8_t* color = led_colors[mode];
//     ESP_ERROR_CHECK(handle->strip_handle->set_pixel(handle->strip_handle, 0, color[0], color[1], color[2]));
//     ESP_ERROR_CHECK(handle->strip_handle->refresh(handle->strip_handle, 100));
//     handle->current_mode = mode;
//     handle->is_on = true;
//     return ESP_OK;
// }

// esp_err_t led_controller_button_action(led_controller_handle_t handle, led_mode_t mode, uint32_t duration_ms)
// {
//     if (!handle) return ESP_ERR_INVALID_ARG;

//     esp_err_t err = led_controller_set_mode(handle, mode);
//     if (err == ESP_OK) {
//         xTimerChangePeriod(handle->timer_handle, pdMS_TO_TICKS(duration_ms), portMAX_DELAY);
//         xTimerStart(handle->timer_handle, portMAX_DELAY);
//     }
//     return err;
// }

esp_err_t led_controller_turn_off(led_controller_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Turning off LED");
    ESP_ERROR_CHECK(handle->strip_handle->clear(handle->strip_handle, 100));
    handle->is_on = false;
    return ESP_OK;
} 

/* --- set_mode 안에서 혹시 돌고 있던 auto-off 타이머는 정지 --- */
esp_err_t led_controller_set_mode(led_controller_handle_t handle, led_mode_t mode)
{
    if (!handle || mode >= LED_MODE_LEN) return ESP_ERR_INVALID_ARG;

    /* 깜빡임 방지: 이전에 걸어둔 auto-off 타이머를 반드시 멈춘다 */
    // xTimerStop(handle->timer_handle, portMAX_DELAY);

    const uint8_t *color = led_colors[mode];
    ESP_ERROR_CHECK(handle->strip_handle->set_pixel(handle->strip_handle, 0, color[0], color[1], color[2]));
    ESP_ERROR_CHECK(handle->strip_handle->refresh(handle->strip_handle, 100));

    handle->current_mode = mode;
    handle->is_on = true;
    return ESP_OK;
}

// /* --- turn_off 에서도 타이머 정지 (안전) --- */
// esp_err_t led_controller_turn_off(led_controller_handle_t handle)
// {
//     if (!handle) return ESP_ERR_INVALID_ARG;
//     xTimerStop(handle->timer_handle, portMAX_DELAY);
//     ESP_ERROR_CHECK(handle->strip_handle->clear(handle->strip_handle, 100));
//     handle->is_on = false;
//     return ESP_OK;
// }




/* === 신규: 색 인덱스로 바로 켜기 === */
esp_err_t led_controller_set_color_idx(led_controller_handle_t handle, uint8_t color_idx)
{
    if (!handle || color_idx >= LED_MODE_LEN) return ESP_ERR_INVALID_ARG;
    return led_controller_set_mode(handle, (led_mode_t)color_idx);
}

/* === 신규: on/off 설정 === */
esp_err_t led_controller_set_on(led_controller_handle_t handle, bool on)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (on) {
        /* 현재 모드로 점등 (현재 모드가 유효하지 않을 일은 없지만, 방어적으로 보정) */
        led_mode_t m = handle->current_mode;
        if (m >= LED_MODE_LEN) m = LED_MODE_1_BLUE;
        return led_controller_set_mode(handle, m);
    } else {
        return led_controller_turn_off(handle);
    }
}

/* === 신규: 토글 === */
esp_err_t led_controller_toggle(led_controller_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return led_controller_set_on(handle, !handle->is_on);
}

/* === 신규: 상태 조회 === */
esp_err_t led_controller_get_state(led_controller_handle_t handle, bool *on, led_mode_t *mode)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (on)   *on   = handle->is_on;
    if (mode) *mode = handle->current_mode;
    return ESP_OK;
}

/* === 신규: auto-off 타이머 취소 === */
// esp_err_t led_controller_cancel_auto_off(led_controller_handle_t handle)
// {
//     if (!handle) return ESP_ERR_INVALID_ARG;
//     xTimerStop(handle->timer_handle, portMAX_DELAY);
//     return ESP_OK;
// }
