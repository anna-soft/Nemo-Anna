/*
 * led_controller.h
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED Color Modes
 */
typedef enum {
    LED_MODE_1_BLUE = 0,
    LED_MODE_2_YELLOW,
    LED_MODE_3_PURPLE,
    LED_MODE_4_RED,
    LED_MODE_LEN,
} led_mode_t;

// Forward declaration of the implementation struct
typedef struct led_controller_s* led_controller_handle_t;

/**
 * @brief Initialize the LED controller
 *
 * @param gpio_num GPIO number for the LED strip
 * @param p_handle Pointer to receive the handle of the LED controller
 * @return ESP_OK on success
 */
esp_err_t led_controller_init(int gpio_num, led_controller_handle_t* p_handle);

/**
 * @brief Deinitialize the LED controller
 *
 * @param handle Handle of the LED controller
 * @return ESP_OK on success
 */
esp_err_t led_controller_deinit(led_controller_handle_t handle);

/**
 * @brief Set the LED to a specific mode (color)
 *
 * @param handle Handle of the LED controller
 * @param mode The color mode to set
 * @return ESP_OK on success
 */
esp_err_t led_controller_set_mode(led_controller_handle_t handle, led_mode_t mode);

/**
 * @brief Perform a button action: turn on for a short duration then turn off
 *
 * @param handle Handle of the LED controller
 * @param mode The color mode to use for the blink
 * @param duration_ms Duration in milliseconds to stay on
 * @return ESP_OK on success
 */
esp_err_t led_controller_button_action(led_controller_handle_t handle, led_mode_t mode, uint32_t duration_ms);

/**
 * @brief Turn the LED off
 *
 * @param handle Handle of the LED controller
 * @return ESP_OK on success
 */
esp_err_t led_controller_turn_off(led_controller_handle_t handle);


/* === 신규 편의 API (깜빡임 없이 “그 색을 계속 켜두기/끄기”) === */

/* 팔레트 인덱스로 색 선택 + 즉시 켜기 (timer 사용 안 함) */
esp_err_t led_controller_set_color_idx(led_controller_handle_t handle, uint8_t color_idx);

/* 현재 선택된 색을 켤지/끌지 설정 (true=켜기, false=끄기) */
esp_err_t led_controller_set_on(led_controller_handle_t handle, bool on);

/* 현재 on/off 토글 */
esp_err_t led_controller_toggle(led_controller_handle_t handle);

/* 현재 on/off 와 모드 조회 */
esp_err_t led_controller_get_state(led_controller_handle_t handle, bool *on, led_mode_t *mode);

/* 혹시 이전에 걸어둔 자동-OFF 타이머가 살아있다면 즉시 취소 */
esp_err_t led_controller_cancel_auto_off(led_controller_handle_t handle);


#ifdef __cplusplus
}
#endif 