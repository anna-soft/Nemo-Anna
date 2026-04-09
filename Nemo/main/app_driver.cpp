/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <esp_matter.h>
#include <app_priv.h>
// #include <esp_timer.h>
// #include <button_gpio.h>

#include "driver/gpio.h"
#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/PlatformManager.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemLayer.h>

#include "anna_cfg.h"
#include "led_controller.h"
using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace chip::DeviceLayer;
using namespace chip::app::Clusters::OnOff;

static const char *TAG = "app_driver";
// Hold window for the first-press lock (ms)
static constexpr uint32_t  k_first_lock_hold_ms = 350;
// Button pulse width (ms)
static constexpr uint32_t  k_button_pulse_ms    = 100;
// Button cooldown (ms) after one press during which new ON is rejected
static constexpr uint32_t  k_button_cooldown_ms = 350;

// Forward declaration for first-press lock release timer handler
static void first_lock_release_timer_handler(chip::System::Layer *, void *);
// Auto-OFF timer callback for button endpoints
static void button_auto_off_timer_handler(chip::System::Layer *, void *);
// Cooldown release timer for buttons
static void button_cooldown_release_timer_handler(chip::System::Layer *, void *);
// ConBtn endpoint timers
static void con_btn_auto_off_timer_handler(chip::System::Layer *, void *);
static void con_btn_pulse_timer_handler(chip::System::Layer *, void *);
// ConSwt periodic eval timers/helpers
static void con_swt_as_eval_timer_handler(chip::System::Layer *, void *);
static void con_swt_as_debounce_timer_handler(chip::System::Layer *, void *);
static bool any_con_swt_on();
static bool any_con_swt_logically_on_with_as();
static bool con_swt_item_is_logically_on(int idx);
static bool con_swt_endpoint_is_logically_on(uint16_t endpoint_id);
static void con_swt_refresh_as_eval_timer();
static bool con_swt_force_gpio_off_by_index(int idx);
static bool con_swt_drive_gpio_on_by_index(int idx);
static bool con_swt_request_gpio_off_by_index(int idx);
static void con_swt_group_reconcile_work(intptr_t);

led_controller_handle_t g_led_handle = NULL;

/* Debounce state for mode selection */
namespace {
    // Last ON request index captured from controller
    static int      s_last_requested_mode_idx = -1;
    // Monotonic sequence to detect new intents during apply loop
    static uint32_t s_last_request_seq        = 0;
    // Apply state flags
    static bool     s_in_apply                = false;
    static bool     s_apply_queued            = false;
    // First-press wins lock state
    static bool     s_first_lock_active       = false;
    static int      s_first_lock_idx          = -1;
    // Bypass PRE rejection for internal Set() writes
    static bool     s_internal_write          = false;
    // First-press hold timer armed flag
    static bool     s_first_lock_timer_armed  = false;
    // Per-button cooldown flags (indexed by g_anna_cfg.a_button index)
    static bool     s_button_cooldown_active[ANNA_MAX_BUTTON] = {false};
    // Count how many presses were blocked by cooldown per button
    static uint32_t s_button_cooldown_blocked_count[ANNA_MAX_BUTTON] = {0};
    // Track per-button GPIO initialization
    static bool     s_button_gpio_inited[ANNA_MAX_BUTTON] = {false};
    // Switch: cooldown and init tracking
    static bool     s_switch_cooldown_active[ANNA_MAX_SWITCH] = {false};
    static uint32_t s_switch_cooldown_blocked_count[ANNA_MAX_SWITCH] = {0};
    static bool     s_switch_gpio_inited[ANNA_MAX_SWITCH] = {false};
    // ConBtn: cooldown, init, eval
    static bool     s_con_btn_cooldown_active[ANNA_MAX_CON_ACT] = {false};
    static uint32_t s_con_btn_cooldown_blocked_count[ANNA_MAX_CON_ACT] = {0};
    static bool     s_con_btn_gpio_inited[ANNA_MAX_CON_ACT] = {false};
    // UNUSED after switching to event-driven evaluation (mode change & ISR for bs)
    // static bool     s_con_btn_eval_running[ANNA_MAX_CON_ACT] = {false};
    // Analog sensor evaluation (periodic)
    static constexpr uint32_t k_as_eval_ms = 100; // periodic evaluation interval (AS measurement)
    static bool     s_con_btn_as_timer_armed = false;
    static bool     s_as_db_timer_armed[ANNA_MAX_CON_ACT] = {false};
    // ConSwt: cooldown, init, AS debounce/eval
    static bool     s_con_swt_cooldown_active[ANNA_MAX_CON_SWT_ACT] = {false};
    static uint32_t s_con_swt_cooldown_blocked_count[ANNA_MAX_CON_SWT_ACT] = {0};
    static bool     s_con_swt_gpio_inited[ANNA_MAX_CON_SWT_ACT] = {false};
    static bool     s_con_swt_as_timer_armed = false;
    static bool     s_con_swt_as_db_timer_armed[ANNA_MAX_CON_SWT_ACT] = {false};
    // Pending OFF when NotOffPin blocks; suppress repeated debounce until on_pin changes or conditions recover
    static bool     s_con_swt_off_pending[ANNA_MAX_CON_SWT_ACT] = {false};
    static bool     s_con_swt_off_block_logged[ANNA_MAX_CON_SWT_ACT] = {false};

    // ConBtn: optional robustness flag to block maintain after a group timeout
    static uint16_t s_con_btn_abort_ep[ANNA_MAX_CON_ACT] = {0};
    static bool     s_con_btn_abort_active[ANNA_MAX_CON_ACT] = {false};
}
// ---- ConBtn group abort helpers (optional robustness) ----
static void con_btn_group_abort_mark(uint16_t ep)
{
    for (int i = 0; i < ANNA_MAX_CON_ACT; ++i) {
        if (!s_con_btn_abort_active[i]) {
            s_con_btn_abort_ep[i] = ep;
            s_con_btn_abort_active[i] = true;
            return;
        }
        if (s_con_btn_abort_ep[i] == ep) {
            s_con_btn_abort_active[i] = true;
            return;
        }
    }
}

static void con_btn_group_abort_clear(uint16_t ep)
{
    for (int i = 0; i < ANNA_MAX_CON_ACT; ++i) {
        if (s_con_btn_abort_active[i] && s_con_btn_abort_ep[i] == ep) {
            s_con_btn_abort_active[i] = false;
            s_con_btn_abort_ep[i] = ENDPOINT_ID_INVALID;
            return;
        }
    }
}

static bool con_btn_group_abort_is_active(uint16_t ep)
{
    for (int i = 0; i < ANNA_MAX_CON_ACT; ++i) {
        if (s_con_btn_abort_active[i] && s_con_btn_abort_ep[i] == ep) {
            return true;
        }
    }
    return false;
}

// ---- ConBtn AS presence helpers ----
enum class con_btn_group_run_mode : uint8_t {
    pulse = 0,
    conditional_hold,
};

static bool con_btn_item_has_bs_config(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return false;
    }
    for (int bi = 0; bi < ANNA_MAX_CON_BS_COUNT; ++bi) {
        if (g_anna_cfg.con_btn[idx].bs_pin[bi] >= 0) {
            return true;
        }
    }
    return false;
}

static bool con_btn_item_has_as_config(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return false;
    }
    for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
        if (g_anna_cfg.con_btn[idx].as_pin[ai] >= 0) {
            return true;
        }
    }
    return false;
}

static const char * con_btn_group_mode_name(con_btn_group_run_mode mode)
{
    switch (mode) {
    case con_btn_group_run_mode::pulse:
        return "pulse";
    case con_btn_group_run_mode::conditional_hold:
        return "conditional_hold";
    default:
        return "unknown";
    }
}

static bool con_btn_group_has_as(uint16_t ep)
{
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) { continue; }
        for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
            if (g_anna_cfg.con_btn[i].as_pin[ai] >= 0) { return true; }
        }
    }
    return false;
}

static bool con_btn_any_on_in_group(uint16_t ep)
{
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) { continue; }
        if (g_anna_cfg.con_btn[i].is_on) { return true; }
    }
    return false;
}

static bool any_con_btn_on_with_as()
{
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (!g_anna_cfg.con_btn[i].is_on) { continue; }
        for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
            if (g_anna_cfg.con_btn[i].as_pin[ai] >= 0) { return true; }
        }
    }
    return false;
}

static void con_btn_as_eval_timer_handler(chip::System::Layer *, void *);
static bool con_btn_as_conditions_ok(int idx);
static bool con_btn_bs_conditions_ok(int idx);
// Hook declarations are provided by app_priv.h
static void con_btn_as_debounce_timer_handler(chip::System::Layer *, void *);
static void con_btn_pulse_timer_handler(chip::System::Layer *, void *);

static bool con_btn_item_hold_conditions_ok(int idx)
{
    return con_btn_bs_conditions_ok(idx) && con_btn_as_conditions_ok(idx);
}

static bool con_btn_item_pre_on_conditions_ok(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return false;
    }
    int req_mode = g_anna_cfg.con_btn[idx].mode_idx;
    bool mode_ok = (req_mode < 0) || (g_anna_cfg.on_mode == req_mode);
    return mode_ok && con_btn_item_hold_conditions_ok(idx);
}

static con_btn_group_run_mode con_btn_group_mode_for_endpoint(uint16_t ep)
{
    bool has_hold_source = false;

    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) {
            continue;
        }
        if (con_btn_item_has_bs_config(i) || con_btn_item_has_as_config(i)) {
            has_hold_source = true;
            break;
        }
    }

    if (has_hold_source) {
        return con_btn_group_run_mode::conditional_hold;
    }
    return con_btn_group_run_mode::pulse;
}
// ConSwt forward decls
static void con_swt_cooldown_release_timer_handler(chip::System::Layer *, void *);
static void con_swt_as_eval_timer_handler(chip::System::Layer *, void *);
static void con_swt_as_debounce_timer_handler(chip::System::Layer *, void *);
static void con_swt_execute_off(int idx);
// ConSwt periodic-eval helper (defined later)
static bool any_con_swt_on();
// Worker to retry pending OFF when on_pin changes or conditions recover
static void con_swt_try_pending_offs_work(intptr_t);
// ConSwt group reconcile (endpoint-level OR across items)
extern "C" void app_driver_con_swt_group_reconcile_by_endpoint(uint16_t endpoint_id)
{
    if (endpoint_id == ENDPOINT_ID_INVALID) {
        return;
    }
    uintptr_t packed = static_cast<uintptr_t>(endpoint_id);
    chip::DeviceLayer::PlatformMgr().ScheduleWork(con_swt_group_reconcile_work, (intptr_t)packed);
}
/* Maintain-only group ON for ConBtn: if the endpoint group is already physically/logically ON
 * (any item is_on=true) and any sibling still satisfies AND conditions, re-assert all group pins ON
 * and keep is_on=true. Do NOT re-pulse the endpoint Attribute.
 */
extern "C" bool app_driver_con_btn_maintain_group_on_if_satisfied(uint16_t endpoint_id)
{
    if (con_btn_group_mode_for_endpoint(endpoint_id) != con_btn_group_run_mode::conditional_hold) {
        return false;
    }
    // Optional robustness: if a recent group timeout occurred, block maintain until next PRE ON
    if (con_btn_group_abort_is_active(endpoint_id)) {
        return false;
    }
    // Check if any item in the group is currently ON (is_on == true)
    bool any_on_in_group = false;
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != endpoint_id) { continue; }
        if (g_anna_cfg.con_btn[i].is_on) { any_on_in_group = true; break; }
    }
    if (!any_on_in_group) {
        return false; // do not create ON from fully-OFF state
    }
    // Evaluate if any item still satisfies hold conditions (BS/AS only).
    bool any_satisfied = false;
    pin_mask_t group_not_on_mask = 0;
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != endpoint_id) { continue; }
        group_not_on_mask |= g_anna_cfg.con_btn[i].base.not_on_mask;
        if (con_btn_item_hold_conditions_ok(i)) { any_satisfied = true; }
    }
    if (!any_satisfied) {
        return false;
    }
    // Respect group not_on gate; if blocked, do not modify pins
    if ((g_anna_cfg.on_pin & group_not_on_mask) != 0) {
        ESP_LOGW(TAG, "ConBtn maintain blocked by group not_on_mask: ep=%u on_pin=0x%08x", (unsigned)endpoint_id, (unsigned)g_anna_cfg.on_pin);
        return false;
    }
    // Re-assert all group pins ON and keep is_on=true (no attribute changes here)
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != endpoint_id) { continue; }
        int pin_no = (int)g_anna_cfg.con_btn[i].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            if (!s_con_btn_gpio_inited[i]) {
                gpio_reset_pin(gpio);
                gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
                s_con_btn_gpio_inited[i] = true;
                ESP_LOGW(TAG, "ConBtn GPIO late-init(maintain): idx=%d pin=%d", i, pin_no);
            }
            gpio_set_level(gpio, 1);
        }
        g_anna_cfg.con_btn[i].is_on = true;
    }
    // Ensure AS periodic evaluation armed only if this group has AS configured
    if (!s_con_btn_as_timer_armed && con_btn_group_has_as(endpoint_id)) {
        s_con_btn_as_timer_armed = true;
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_as_eval_ms),
            con_btn_as_eval_timer_handler,
            nullptr);
        ESP_LOGI(TAG, "ConBtn AS periodic eval armed (maintain): %u ms", (unsigned)k_as_eval_ms);
    }
    ESP_LOGI(TAG, "ConBtn maintain group ON: ep=%u", (unsigned)endpoint_id);
    return true;
}

static void con_btn_cancel_endpoint_auto_timers(uint16_t ep)
{
    void * ep_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(ep));
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_auto_off_timer_handler, ep_ctx);
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_pulse_timer_handler, ep_ctx);
}


/* Expose a small API to mark button GPIO as initialized (called from settings_init) */
extern "C" void app_driver_mark_button_gpio_inited(int idx)
{
    if (idx >= 0 && idx < ANNA_MAX_BUTTON) {
        s_button_gpio_inited[idx] = true;
    }
}

/* Expose API for switch GPIO init mark */
extern "C" void app_driver_mark_switch_gpio_inited(int idx)
{
    if (idx >= 0 && idx < ANNA_MAX_SWITCH) {
        s_switch_gpio_inited[idx] = true;
    }
}

/* Expose API for con_btn GPIO init mark */
extern "C" void app_driver_mark_con_btn_gpio_inited(int idx)
{
    if (idx >= 0 && idx < ANNA_MAX_CON_ACT) {
        s_con_btn_gpio_inited[idx] = true;
    }
}

/* Expose API for con_swt GPIO init mark */
extern "C" void app_driver_mark_con_swt_gpio_inited(int idx)
{
    if (idx >= 0 && idx < ANNA_MAX_CON_SWT_ACT) {
        s_con_swt_gpio_inited[idx] = true;
    }
}

// Auto-OFF timer callback for button endpoints
static void button_auto_off_timer_handler(chip::System::Layer *, void * ctx)
{
    chip::EndpointId ep = static_cast<chip::EndpointId>(reinterpret_cast<uintptr_t>(ctx));
    // Internal write: bypass PRE rejections
    s_internal_write = true;
    (void) OnOff::Attributes::OnOff::Set(ep, false);
    s_internal_write = false;

    // Also drive the associated button GPIO LOW (OFF)
    int btn_index = -1;
    for (int b = 0; b < g_anna_cfg.button_cnt; ++b) {
        if (g_anna_cfg.a_button[b].base.endpoint_id == ep) { 
            btn_index = b; 
            break; 
        }
    }
    if (btn_index >= 0) {
        int pin_no = (int)g_anna_cfg.a_button[btn_index].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            if (!s_button_gpio_inited[btn_index]) {
                // Fallback one-time init if not initialized during startup
                gpio_reset_pin(gpio);
                gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
                s_button_gpio_inited[btn_index] = true;
                ESP_LOGW(TAG, "Button GPIO late-init in timer: idx=%d pin=%d", btn_index, pin_no);
            }
            gpio_set_level(gpio, 0);
            ESP_LOGI(TAG, "Button GPIO OFF by timer: idx=%d ep=%u pin=%d", btn_index, (unsigned)ep, pin_no);
        } else {
            ESP_LOGW(TAG, "Invalid GPIO in timer for button idx=%d pin=%d", btn_index, pin_no);
        }
    }
}

static void button_cooldown_release_timer_handler(chip::System::Layer *, void * ctx)
{
    uintptr_t packed = reinterpret_cast<uintptr_t>(ctx);
    uint16_t endpoint_id = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t btn_index   = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    if (btn_index < ANNA_MAX_BUTTON) {
        s_button_cooldown_active[btn_index] = false;
        ESP_LOGI(TAG, "Button cooldown released: idx=%u ep=%u blockedTotal=%u", (unsigned)btn_index, (unsigned)endpoint_id, (unsigned)s_button_cooldown_blocked_count[btn_index]);
    }
}

// Cooldown release for switches (packed: [31:16]=switchIndex, [15:0]=endpointId)
static void switch_cooldown_release_timer_handler(chip::System::Layer *, void * ctx)
{
    uintptr_t packed = reinterpret_cast<uintptr_t>(ctx);
    uint16_t endpoint_id = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t sw_index   = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    if (sw_index < ANNA_MAX_SWITCH) {
        s_switch_cooldown_active[sw_index] = false;
        ESP_LOGI(TAG, "Switch cooldown released: idx=%u ep=%u blockedTotal=%u", (unsigned)sw_index, (unsigned)endpoint_id, (unsigned)s_switch_cooldown_blocked_count[sw_index]);
    }
}

// Helper: update g_anna_cfg.on_pin bitmask for a given pin number
static inline void update_on_pin_mask(uint8_t pin, bool on)
{
    if (pin < 32) {
        if (on) {
            g_anna_cfg.on_pin |= (1u << pin);
        } else {
            g_anna_cfg.on_pin &= ~(1u << pin);
        }
        // on_pin changed: retry any pending ConSwt OFF on CHIP thread
        chip::DeviceLayer::PlatformMgr().ScheduleWork(con_swt_try_pending_offs_work, 0);
    }
}

// Forward declarations for con_btn timers and helpers
static void con_btn_auto_off_timer_handler(chip::System::Layer *, void *);
static void con_btn_cooldown_release_timer_handler(chip::System::Layer *, void *);
static void con_btn_timeout_timer_handler(chip::System::Layer *, void *);
// Group timeout updater (endpoint-level max_sec, scheduled on CHIP thread)
static void con_btn_group_timeout_update_work(intptr_t arg);
// static void con_btn_eval_timer_handler(chip::System::Layer *, void *); // UNUSED (event-driven)
static void con_btn_execute_on(int idx, chip::EndpointId ep);
static void con_btn_execute_on_work(intptr_t arg);
static void con_btn_execute_off(int idx);
static void con_btn_execute_off_work(intptr_t arg);

static void boot_safe_off_button_idx(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.button_cnt) {
        return;
    }
    uint16_t ep = g_anna_cfg.a_button[idx].base.endpoint_id;
    uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(idx)) << 16) |
                       static_cast<uintptr_t>(static_cast<uint16_t>(ep));
    void * cd_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(button_cooldown_release_timer_handler, cd_ctx);
    s_button_cooldown_active[idx] = false;
    s_button_cooldown_blocked_count[idx] = 0;

    int pin_no = (int)g_anna_cfg.a_button[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_button_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_button_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "Button GPIO late-init(boot off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
    }

    s_internal_write = true;
    (void) OnOff::Attributes::OnOff::Set(ep, false);
    s_internal_write = false;
}

static void boot_safe_off_switch_idx(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.switch_cnt) {
        return;
    }
    uint16_t ep = g_anna_cfg.a_switch[idx].base.endpoint_id;
    uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(idx)) << 16) |
                       static_cast<uintptr_t>(static_cast<uint16_t>(ep));
    void * sw_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(switch_cooldown_release_timer_handler, sw_ctx);
    s_switch_cooldown_active[idx] = false;
    s_switch_cooldown_blocked_count[idx] = 0;

    int pin_no = (int)g_anna_cfg.a_switch[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_switch_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_switch_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "Switch GPIO late-init(boot off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
    }
    update_on_pin_mask((uint8_t)pin_no, false);

    s_internal_write = true;
    (void) OnOff::Attributes::OnOff::Set(ep, false);
    s_internal_write = false;
}

static void boot_safe_off_con_btn_idx(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return;
    }
    uint16_t ep = g_anna_cfg.con_btn[idx].base.endpoint_id;
    g_anna_cfg.con_btn[idx].is_on = false;

    uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(idx)) << 16) |
                       static_cast<uintptr_t>(static_cast<uint16_t>(ep));
    void * cd_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_cooldown_release_timer_handler, cd_ctx);
    s_con_btn_cooldown_active[idx] = false;
    s_con_btn_cooldown_blocked_count[idx] = 0;

    void * db_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(idx)));
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_debounce_timer_handler, db_ctx);
    s_as_db_timer_armed[idx] = false;

    int pin_no = (int)g_anna_cfg.con_btn[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_btn_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_btn_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConBtn GPIO late-init(boot off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
    }
}

static void boot_safe_off_con_swt_idx(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return;
    }
    uint16_t ep = g_anna_cfg.con_swt[idx].base.endpoint_id;
    uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(idx)) << 16) |
                       static_cast<uintptr_t>(static_cast<uint16_t>(ep));
    void * cs_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_cooldown_release_timer_handler, cs_ctx);
    s_con_swt_cooldown_active[idx] = false;
    s_con_swt_cooldown_blocked_count[idx] = 0;

    void * db_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(idx)));
    chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_debounce_timer_handler, db_ctx);
    s_con_swt_as_db_timer_armed[idx] = false;

    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_swt_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_swt_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConSwt GPIO late-init(boot off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
    }
    update_on_pin_mask((uint8_t)pin_no, false);
    s_con_swt_off_pending[idx] = false;
    s_con_swt_off_block_logged[idx] = false;
}


// --- Analog sensor helpers and periodic evaluation ---
static bool con_btn_as_conditions_ok(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return false;
    }
    for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
        int apin = g_anna_cfg.con_btn[idx].as_pin[ai];
        if (apin < 0) {
            continue; // not configured
        }
        int target = (int)g_anna_cfg.con_btn[idx].as_tar[ai];
        if (target < 0) {
            target = 0;
        }
        if (target > 100) {
            target = 100;
        }
        bool evt_ge = g_anna_cfg.con_btn[idx].as_evt[ai];
        int value_scaled = -1;
        bool ok_read = app_read_analog_scaled((uint8_t)apin, &value_scaled);
        if (!ok_read) {
            // Conservative: treat as mismatch
            return false;
        }
        if (value_scaled < 0) {
            value_scaled = 0;
        }
        if (value_scaled > 100) {
            value_scaled = 100;
        }
        // Apply 1% hysteresis when maintaining ON state
        bool maintaining = g_anna_cfg.con_btn[idx].is_on;
        int effective = target;
        if (maintaining) {
            effective = evt_ge ? std::max(0, target - 1)
                               : std::min(100, target + 1);
        }
        ESP_LOGI(TAG, "AS check: idx=%d pin=%d value=%d target=%d effective=%d evt_ge=%d maintain=%d",
                 idx, apin, value_scaled, target, effective, evt_ge ? 1 : 0, maintaining ? 1 : 0);
        if (evt_ge) {
            if (!(value_scaled >= effective)) {
                ESP_LOGI(TAG, "effective value_scaled=%d < target=%d", value_scaled, effective);
                return false;
            }
        } else {
            if (!(value_scaled <= effective)) {
                return false;
            }
        }
    }
    return true;
}

static bool con_btn_bs_conditions_ok(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return false;
    }
    for (int bi = 0; bi < ANNA_MAX_CON_BS_COUNT; ++bi) {
        int bpin = g_anna_cfg.con_btn[idx].bs_pin[bi];
        if (bpin < 0) {
            continue;
        }
        if (bpin >= 0 && bpin < 32 && GPIO_IS_VALID_GPIO((gpio_num_t)bpin)) {
            int cur = app_bs_get_debounced_level(bpin);
            if (cur < 0) {
                cur = gpio_get_level((gpio_num_t)bpin);
            }
            if (((bool)cur) != g_anna_cfg.con_btn[idx].bs_evt[bi]) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool any_con_btn_on()
{
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        if (g_anna_cfg.con_btn[c].is_on) {
            return true;
        }
    }
    return false;
}

static void con_btn_as_eval_timer_handler(chip::System::Layer *, void *)
{
    bool has_active = false;
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        if (!g_anna_cfg.con_btn[c].is_on) {
            continue;
        }
        has_active = true;
        bool hold_ok = con_btn_item_hold_conditions_ok(c);
        if (!hold_ok) {
            if (!s_as_db_timer_armed[c]) {
                s_as_db_timer_armed[c] = true;
                void * ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(c))); 
                chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_debounce_timer_handler, ctx);
                chip::DeviceLayer::SystemLayer().StartTimer(
                    chip::System::Clock::Milliseconds32(50),
                    con_btn_as_debounce_timer_handler,
                    ctx);
                ESP_LOGI(TAG, "ConBtn AS debounce timer armed: idx=%d 50ms hold_ok=0", c);
            }
        } else {
            if (s_as_db_timer_armed[c]) {
                void * ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(c)));
                chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_debounce_timer_handler, ctx);
                s_as_db_timer_armed[c] = false;
                ESP_LOGI(TAG, "ConBtn AS debounce timer canceled: idx=%d hold_ok=1", c);
            }
        }
    }
    if (has_active && any_con_btn_on()) {
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_as_eval_ms),
            con_btn_as_eval_timer_handler,
            nullptr);
    } else {
        s_con_btn_as_timer_armed = false;
    }
}

/* One-shot AS debounce timer: confirm violation persists for 50ms before forcing OFF */
static void con_btn_as_debounce_timer_handler(chip::System::Layer *, void * ctx)
{
    uint16_t idx = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ctx) & 0xFFFF);
    if (idx >= g_anna_cfg.con_btn_cnt) {
        return;
    }
    // Clear armed flag first to allow re-arming if needed by the evaluator
    s_as_db_timer_armed[idx] = false;
    // If already off, nothing to do
    if (!g_anna_cfg.con_btn[idx].is_on) {
        return;
    }
    bool hold_ok = con_btn_item_hold_conditions_ok((int)idx);
    if (!hold_ok) {
        // Maintain-only: if any sibling still satisfied and group is already ON, keep group ON (no attribute re-pulse)
        uint16_t ep = g_anna_cfg.con_btn[idx].base.endpoint_id;
        if (app_driver_con_btn_maintain_group_on_if_satisfied(ep)) {
            return;
        }
        ESP_LOGW(TAG, "ConBtn AS debounce -> FORCE OFF: idx=%d hold_ok=0", (int)idx);
        con_btn_execute_off((int)idx);
    }
}

// Execute accepted con_btn ON
static void con_btn_execute_on(int idx, chip::EndpointId ep)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return;
    }
    con_btn_group_run_mode group_mode = con_btn_group_mode_for_endpoint((uint16_t)ep);
    // Clear abort flag for this endpoint on new PRE ON
    con_btn_group_abort_clear((uint16_t)ep);
    int pin_no = (int)g_anna_cfg.con_btn[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_btn_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_btn_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConBtn GPIO late-init: idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 1);
        ESP_LOGI(TAG, "ConBtn GPIO ON: idx=%d ep=%u pin=%d", idx, (unsigned)ep, pin_no);
    } else {
        ESP_LOGW(TAG, "Invalid GPIO for conBtn idx=%d pin=%d", idx, pin_no);
    }
    con_btn_cancel_endpoint_auto_timers((uint16_t)ep);
    if (group_mode == con_btn_group_run_mode::pulse) {
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_button_pulse_ms),
            con_btn_pulse_timer_handler,
            reinterpret_cast<void *>(static_cast<uintptr_t>(ep)));
        ESP_LOGI(TAG, "ConBtn pulse timer armed: ep=%u hold=%u ms mode=%s", (unsigned)ep, (unsigned)k_button_pulse_ms, con_btn_group_mode_name(group_mode));
    } else {
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_button_pulse_ms),
            con_btn_auto_off_timer_handler,
            reinterpret_cast<void *>(static_cast<uintptr_t>(ep)));
        ESP_LOGI(TAG, "ConBtn endpoint auto-off armed: ep=%u hold=%u ms mode=%s", (unsigned)ep, (unsigned)k_button_pulse_ms, con_btn_group_mode_name(group_mode));
    }
    // Set is_on; evaluation is handled by events (mode change & bs ISR)
    g_anna_cfg.con_btn[idx].is_on = true;
    ESP_LOGI(TAG, "ConBtn is_on set On: idx=%d ep=%u", idx, (unsigned)ep);

    // NOTE:
    // - Group max_sec timeout is now armed once per endpoint (label group) in PRE path,
    //   via con_btn_group_timeout_update_work(). Do NOT arm per-index timeouts here.
    // Cooldown
    s_con_btn_cooldown_active[idx] = true;
    uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(idx)) << 16) |
                       static_cast<uintptr_t>(static_cast<uint16_t>(ep));
    void * cd_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_cooldown_release_timer_handler, cd_ctx);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(k_button_cooldown_ms),
        con_btn_cooldown_release_timer_handler,
        cd_ctx);

    // Arm AS periodic evaluation only if group has AS
    if (group_mode == con_btn_group_run_mode::conditional_hold &&
        !s_con_btn_as_timer_armed &&
        con_btn_group_has_as((uint16_t)ep)) {
        s_con_btn_as_timer_armed = true;
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_as_eval_ms),
            con_btn_as_eval_timer_handler,
            nullptr);
        ESP_LOGI(TAG, "ConBtn AS periodic eval armed: %u ms", (unsigned)k_as_eval_ms);
    }
}

// Work queue entrypoint for con_btn_execute_on (arg packs idx and ep)
static void con_btn_execute_on_work(intptr_t arg)
{
    uintptr_t packed = static_cast<uintptr_t>(arg);
    uint16_t ep = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t idx = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    con_btn_execute_on((int)idx, (chip::EndpointId)ep);
}

// Execute con_btn OFF: set is_on=false, stop eval, drive GPIO LOW
static void con_btn_execute_off(int idx)
{
    uint16_t ep = g_anna_cfg.con_btn[idx].base.endpoint_id;
    g_anna_cfg.con_btn[idx].is_on = false;
    ESP_LOGI(TAG, "ConBtn is_on set Off: idx=%d", idx);
    // s_con_btn_eval_running[idx] = false; // UNUSED
    int pin_no = (int)g_anna_cfg.con_btn[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_btn_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_btn_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConBtn GPIO late-init(off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
        ESP_LOGI(TAG, "ConBtn GPIO OFF: idx=%d pin=%d", idx, pin_no);
    } else {
        ESP_LOGW(TAG, "Invalid GPIO for conBtn(off) idx=%d pin=%d", idx, pin_no);
    }

    // If debounce timer armed for this idx, cancel it
    if (idx >= 0 && idx < ANNA_MAX_CON_ACT) {
        void * db_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(idx)));
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_debounce_timer_handler, db_ctx);
        s_as_db_timer_armed[idx] = false;
    }

    // If no more active con_btn WITH AS, disarm AS periodic evaluation
    if (s_con_btn_as_timer_armed && !any_con_btn_on_with_as()) {
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_eval_timer_handler, nullptr);
        s_con_btn_as_timer_armed = false;
        ESP_LOGI(TAG, "ConBtn AS periodic eval disarmed (no active)");
    }

    // If no more active con_btn in this endpoint group, cancel group timeout timer
    if (!con_btn_any_on_in_group(ep)) {
        void * ep_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(ep));
        con_btn_cancel_endpoint_auto_timers(ep);
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_timeout_timer_handler, ep_ctx);
        ESP_LOGI(TAG, "ConBtn endpoint timers canceled (group off): ep=%u", (unsigned)ep);
    }
}

extern "C" void app_driver_con_btn_force_off_by_index(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_btn_cnt) {
        return;
    }
    // Run OFF path on CHIP thread to avoid SystemLayer timer/lock contention (older flow called directly)
    uintptr_t packed = static_cast<uintptr_t>(static_cast<uint16_t>(idx));
    chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_execute_off_work, (intptr_t)packed);
}

// ConSwt helpers
static bool con_swt_bs_conditions_ok(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    for (int bi = 0; bi < ANNA_MAX_CON_BS_COUNT; ++bi) {
        int bpin = g_anna_cfg.con_swt[idx].bs_pin[bi];
        if (bpin < 0) { continue; }
        if (bpin >= 0 && bpin < 32 && GPIO_IS_VALID_GPIO((gpio_num_t)bpin)) {
            int cur = app_bs_get_debounced_level(bpin);
            if (cur < 0) { cur = gpio_get_level((gpio_num_t)bpin); }
            if (((bool)cur) != g_anna_cfg.con_swt[idx].bs_evt[bi]) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool con_swt_item_has_as_config(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
        if (g_anna_cfg.con_swt[idx].as_pin[ai] >= 0) {
            return true;
        }
    }
    return false;
}

static bool con_swt_endpoint_is_logically_on(uint16_t endpoint_id)
{
    if (endpoint_id == ENDPOINT_ID_INVALID) {
        return false;
    }
    bool logical_on = false;
    (void) OnOff::Attributes::OnOff::Get(endpoint_id, &logical_on);
    return logical_on;
}

static bool con_swt_item_is_logically_on(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    return con_swt_endpoint_is_logically_on(g_anna_cfg.con_swt[idx].base.endpoint_id);
}

static bool con_swt_is_on_by_pin(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    if (pin_no < 0 || pin_no >= 32) {
        return false;
    }
    return (g_anna_cfg.on_pin & (1u << pin_no)) != 0;
}

// ---- ConSwt periodic-eval helpers ----
static bool any_con_swt_on()
{
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (con_swt_is_on_by_pin(i)) {
            return true;
        }
    }
    return false;
}

static bool any_con_swt_logically_on_with_as()
{
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (!con_swt_item_has_as_config(i)) {
            continue;
        }
        if (con_swt_item_is_logically_on(i)) {
            return true;
        }
    }
    return false;
}

static void con_swt_refresh_as_eval_timer()
{
    bool need_eval = any_con_swt_logically_on_with_as();
    if (need_eval) {
        if (!s_con_swt_as_timer_armed) {
            s_con_swt_as_timer_armed = true;
            chip::DeviceLayer::SystemLayer().StartTimer(
                chip::System::Clock::Milliseconds32(k_as_eval_ms),
                con_swt_as_eval_timer_handler,
                nullptr);
            ESP_LOGI(TAG, "ConSwt AS periodic eval armed: %u ms", (unsigned)k_as_eval_ms);
        }
        return;
    }
    if (s_con_swt_as_timer_armed) {
        chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_eval_timer_handler, nullptr);
        s_con_swt_as_timer_armed = false;
        ESP_LOGI(TAG, "ConSwt AS periodic eval disarmed (no logical ON with AS)");
    }
}

static bool con_swt_force_gpio_off_by_index(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    if (idx < ANNA_MAX_CON_SWT_ACT) {
        void * db_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(idx)));
        chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_debounce_timer_handler, db_ctx);
        s_con_swt_as_db_timer_armed[idx] = false;
        s_con_swt_off_pending[idx] = false;
        s_con_swt_off_block_logged[idx] = false;
    }
    bool was_on = con_swt_is_on_by_pin(idx);
    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_swt_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_swt_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConSwt GPIO late-init(force off): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 0);
    }
    if (pin_no >= 0 && pin_no < 32) {
        update_on_pin_mask((uint8_t)pin_no, false);
    }
    return was_on;
}

static bool con_swt_drive_gpio_on_by_index(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    if (idx < ANNA_MAX_CON_SWT_ACT) {
        void * db_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(idx)));
        chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_debounce_timer_handler, db_ctx);
        s_con_swt_as_db_timer_armed[idx] = false;
        s_con_swt_off_pending[idx] = false;
        s_con_swt_off_block_logged[idx] = false;
    }
    bool was_on = con_swt_is_on_by_pin(idx);
    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
    if (GPIO_IS_VALID_GPIO(gpio)) {
        if (!s_con_swt_gpio_inited[idx]) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            s_con_swt_gpio_inited[idx] = true;
            ESP_LOGW(TAG, "ConSwt GPIO late-init(reconcile on): idx=%d pin=%d", idx, pin_no);
        }
        gpio_set_level(gpio, 1);
    }
    if (pin_no >= 0 && pin_no < 32) {
        update_on_pin_mask((uint8_t)pin_no, true);
    }
    return !was_on;
}

static bool con_swt_request_gpio_off_by_index(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    if (!con_swt_is_on_by_pin(idx)) {
        if (idx < ANNA_MAX_CON_SWT_ACT) {
            s_con_swt_off_pending[idx] = false;
            s_con_swt_off_block_logged[idx] = false;
        }
        return false;
    }
    if ((g_anna_cfg.on_pin & g_anna_cfg.con_swt[idx].base.not_off_mask) != 0) {
        if (idx < ANNA_MAX_CON_SWT_ACT) {
            s_con_swt_off_pending[idx] = true;
            if (!s_con_swt_off_block_logged[idx]) {
                ESP_LOGW(TAG,
                         "ConSwt runtime OFF blocked by not_off_mask: idx=%d on_pin=0x%08x mask=0x%08x",
                         idx,
                         (unsigned)g_anna_cfg.on_pin,
                         (unsigned)g_anna_cfg.con_swt[idx].base.not_off_mask);
                s_con_swt_off_block_logged[idx] = true;
            }
        }
        return false;
    }
    return con_swt_force_gpio_off_by_index(idx);
}

static bool con_swt_as_conditions_ok(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) {
        return false;
    }
    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    bool maintaining = (pin_no >= 0 && pin_no < 32) ? ((g_anna_cfg.on_pin & (1u << pin_no)) != 0) : false;
    for (int ai = 0; ai < ANNA_MAX_CON_AS_COUNT; ++ai) {
        int apin = g_anna_cfg.con_swt[idx].as_pin[ai];
        if (apin < 0) { continue; }
        int target = (int)g_anna_cfg.con_swt[idx].as_tar[ai];
        if (target < 0) target = 0;
        if (target > 100) target = 100;
        bool evt_ge = g_anna_cfg.con_swt[idx].as_evt[ai];
        int value_scaled = -1;
        bool ok_read = app_read_analog_scaled((uint8_t)apin, &value_scaled);
        if (!ok_read) { return false; }
        if (value_scaled < 0) value_scaled = 0;
        if (value_scaled > 100) value_scaled = 100;
        int effective = target;
        if (maintaining) {
            effective = evt_ge ? std::max(0, target - 1) : std::min(100, target + 1);
        }
        if (evt_ge) {
            if (!(value_scaled >= effective)) { return false; }
        } else {
            if (!(value_scaled <= effective)) { return false; }
        }
    }
    return true;
}

static bool con_swt_all_conditions_ok(int idx, bool * mode_ok_out, bool * bs_ok_out, bool * as_ok_out)
{
    bool mode_ok = false;
    bool bs_ok = false;
    bool as_ok = false;
    if (idx >= 0 && idx < g_anna_cfg.con_swt_cnt) {
        mode_ok = (g_anna_cfg.con_swt[idx].mode_idx < 0) || (g_anna_cfg.on_mode == g_anna_cfg.con_swt[idx].mode_idx);
        bs_ok = con_swt_bs_conditions_ok(idx);
        as_ok = con_swt_as_conditions_ok(idx);
    }
    if (mode_ok_out != nullptr) {
        *mode_ok_out = mode_ok;
    }
    if (bs_ok_out != nullptr) {
        *bs_ok_out = bs_ok;
    }
    if (as_ok_out != nullptr) {
        *as_ok_out = as_ok;
    }
    return mode_ok && bs_ok && as_ok;
}

static bool con_swt_group_any_satisfied(uint16_t endpoint_id, pin_mask_t * group_not_on_mask_out,
                                        pin_mask_t * group_not_off_mask_out)
{
    pin_mask_t group_not_on_mask = 0;
    pin_mask_t group_not_off_mask = 0;
    bool any_satisfied = false;
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) {
            continue;
        }
        group_not_on_mask |= g_anna_cfg.con_swt[i].base.not_on_mask;
        group_not_off_mask |= g_anna_cfg.con_swt[i].base.not_off_mask;
        if (con_swt_all_conditions_ok(i, nullptr, nullptr, nullptr)) {
            any_satisfied = true;
        }
    }
    if (group_not_on_mask_out != nullptr) {
        *group_not_on_mask_out = group_not_on_mask;
    }
    if (group_not_off_mask_out != nullptr) {
        *group_not_off_mask_out = group_not_off_mask;
    }
    return any_satisfied;
}

static void con_swt_group_reconcile_work(intptr_t arg)
{
    uint16_t endpoint_id = static_cast<uint16_t>(static_cast<uintptr_t>(arg) & 0xFFFF);
    if (endpoint_id == ENDPOINT_ID_INVALID) {
        return;
    }

    bool logical_on = con_swt_endpoint_is_logically_on(endpoint_id);
    if (!logical_on) {
        bool any_forced_off = false;
        for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
            if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) {
                continue;
            }
            any_forced_off |= con_swt_force_gpio_off_by_index(i);
        }
        con_swt_refresh_as_eval_timer();
        ESP_LOGI(TAG, "ConSwt reconcile: ep=%u logical_on=0 forced_gpio_off=%d",
                 (unsigned)endpoint_id, any_forced_off ? 1 : 0);
        return;
    }

    pin_mask_t group_not_on_mask = 0;
    pin_mask_t group_not_off_mask = 0;
    bool any_satisfied = con_swt_group_any_satisfied(endpoint_id, &group_not_on_mask, &group_not_off_mask);
    bool blocked_by_not_on = ((g_anna_cfg.on_pin & group_not_on_mask) != 0);

    if (any_satisfied && !blocked_by_not_on) {
        bool any_gpio_on = false;
        for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
            if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) {
                continue;
            }
            any_gpio_on |= con_swt_drive_gpio_on_by_index(i);
        }
        con_swt_refresh_as_eval_timer();
        ESP_LOGI(TAG, "ConSwt reconcile: ep=%u logical_on=1 any_satisfied=1 gpio_on=%d",
                 (unsigned)endpoint_id, any_gpio_on ? 1 : 0);
        return;
    }

    bool any_gpio_off = false;
    bool pending_off = false;
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) {
            continue;
        }
        any_gpio_off |= con_swt_request_gpio_off_by_index(i);
        if (i < ANNA_MAX_CON_SWT_ACT && s_con_swt_off_pending[i]) {
            pending_off = true;
        }
    }
    con_swt_refresh_as_eval_timer();
    ESP_LOGI(TAG,
             "ConSwt reconcile: ep=%u logical_on=1 any_satisfied=%d blocked_by_not_on=%d pending_off=%d gpio_off=%d group_not_off_mask=0x%08x",
             (unsigned)endpoint_id,
             any_satisfied ? 1 : 0,
             blocked_by_not_on ? 1 : 0,
             pending_off ? 1 : 0,
             any_gpio_off ? 1 : 0,
             (unsigned)group_not_off_mask);
}

static void con_swt_as_eval_timer_handler(chip::System::Layer *, void *)
{
    uint16_t ep_list[ANNA_MAX_CON_SWT_ACT] = {};
    int ep_count = 0;
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        if (!con_swt_item_is_logically_on(c)) { continue; }
        if (!con_swt_item_has_as_config(c)) { continue; }
        bool mode_ok = false;
        bool bs_ok = false;
        bool as_ok = false;
        bool cond_ok = con_swt_all_conditions_ok(c, &mode_ok, &bs_ok, &as_ok);
        if (!cond_ok) {
            if (s_con_swt_off_pending[c]) {
                // Pending OFF due to not_off_mask; skip re-arming debounce until on_pin changes
            } else if (!s_con_swt_as_db_timer_armed[c]) {
                s_con_swt_as_db_timer_armed[c] = true;
                void * ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(c)));
                chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_debounce_timer_handler, ctx);
                chip::DeviceLayer::SystemLayer().StartTimer(
                    chip::System::Clock::Milliseconds32(50),
                    con_swt_as_debounce_timer_handler,
                    ctx);
                ESP_LOGI(TAG,
                         "ConSwt AS debounce timer armed: idx=%d 50ms cond_ok=%d mode_ok=%d bs_ok=%d as_ok=%d",
                         c,
                         cond_ok ? 1 : 0,
                         mode_ok ? 1 : 0,
                         bs_ok ? 1 : 0,
                         as_ok ? 1 : 0);
            }
        } else {
            if (s_con_swt_as_db_timer_armed[c]) {
                void * ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<uint16_t>(c)));
                chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_debounce_timer_handler, ctx);
                s_con_swt_as_db_timer_armed[c] = false;
                ESP_LOGI(TAG,
                         "ConSwt AS debounce timer canceled: idx=%d cond_ok=%d mode_ok=%d bs_ok=%d as_ok=%d",
                         c,
                         cond_ok ? 1 : 0,
                         mode_ok ? 1 : 0,
                         bs_ok ? 1 : 0,
                         as_ok ? 1 : 0);
            }
            uint16_t ep = g_anna_cfg.con_swt[c].base.endpoint_id;
            bool seen = false;
            for (int i = 0; i < ep_count; ++i) {
                if (ep_list[i] == ep) {
                    seen = true;
                    break;
                }
            }
            if (!seen && ep != ENDPOINT_ID_INVALID && ep_count < ANNA_MAX_CON_SWT_ACT) {
                ep_list[ep_count++] = ep;
            }
        }
    }
    for (int i = 0; i < ep_count; ++i) {
        app_driver_con_swt_group_reconcile_by_endpoint(ep_list[i]);
    }
    if (any_con_swt_logically_on_with_as()) {
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_as_eval_ms),
            con_swt_as_eval_timer_handler,
            nullptr);
    } else {
        s_con_swt_as_timer_armed = false;
    }
}

static void con_swt_as_debounce_timer_handler(chip::System::Layer *, void * ctx)
{
    uint16_t idx = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ctx) & 0xFFFF);
    if (idx >= g_anna_cfg.con_swt_cnt) { return; }
    s_con_swt_as_db_timer_armed[idx] = false;
    if (!con_swt_item_is_logically_on(idx)) { return; }
    bool mode_ok = false;
    bool bs_ok = false;
    bool as_ok = false;
    bool cond_ok = con_swt_all_conditions_ok((int)idx, &mode_ok, &bs_ok, &as_ok);
    if (!cond_ok) {
        uint16_t ep = g_anna_cfg.con_swt[idx].base.endpoint_id;
        ESP_LOGW(TAG,
                 "ConSwt AS debounce -> reconcile OFF path: idx=%d cond_ok=%d mode_ok=%d bs_ok=%d as_ok=%d",
                 (int)idx,
                 cond_ok ? 1 : 0,
                 mode_ok ? 1 : 0,
                 bs_ok ? 1 : 0,
                 as_ok ? 1 : 0);
        app_driver_con_swt_group_reconcile_by_endpoint(ep);
    }
}

// ConSwt cooldown release (packed: [31:16]=conSwtIndex, [15:0]=endpointId)
static void con_swt_cooldown_release_timer_handler(chip::System::Layer *, void * ctx)
{
    uintptr_t packed = reinterpret_cast<uintptr_t>(ctx);
    uint16_t endpoint_id = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t idx   = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    if (idx < ANNA_MAX_CON_SWT_ACT) {
        s_con_swt_cooldown_active[idx] = false;
        ESP_LOGI(TAG, "ConSwt cooldown released: idx=%u ep=%u blockedTotal=%u", (unsigned)idx, (unsigned)endpoint_id, (unsigned)s_con_swt_cooldown_blocked_count[idx]);
    }
}

// Endpoint auto-off for con_btn: attribute OFF only (GPIO handled by is_on)
static void con_btn_auto_off_timer_handler(chip::System::Layer *, void * ctx)
{
    chip::EndpointId ep = static_cast<chip::EndpointId>(reinterpret_cast<uintptr_t>(ctx));
    s_internal_write = true;
    (void) OnOff::Attributes::OnOff::Set(ep, false);
    s_internal_write = false;
}

static void con_btn_pulse_timer_handler(chip::System::Layer *, void * ctx)
{
    uint16_t ep = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ctx) & 0xFFFF);
    s_internal_write = true;
    (void) OnOff::Attributes::OnOff::Set(ep, false);
    s_internal_write = false;
    ESP_LOGI(TAG, "ConBtn pulse timer fired: ep=%u", (unsigned)ep);
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) {
            continue;
        }
        if (!g_anna_cfg.con_btn[i].is_on) {
            continue;
        }
        con_btn_execute_off(i);
    }
}

/* ConBtn cooldown release (packed: [31:16]=idx, [15:0]=endpointId) */
static void con_btn_cooldown_release_timer_handler(chip::System::Layer *, void * ctx)
{
    uintptr_t packed = reinterpret_cast<uintptr_t>(ctx);
    uint16_t endpoint_id = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t idx   = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    if (idx < ANNA_MAX_CON_ACT) {
        s_con_btn_cooldown_active[idx] = false;
        ESP_LOGI(TAG, "ConBtn cooldown released: idx=%u ep=%u blockedTotal=%u", (unsigned)idx, (unsigned)endpoint_id, (unsigned)s_con_btn_cooldown_blocked_count[idx]);
    }
}

/* ConBtn max_sec timeout handler: force OFF regardless of current conditions */
static void con_btn_timeout_timer_handler(chip::System::Layer *, void * ctx)
{
    // Endpoint-level group timeout (ctx holds endpoint_id).
    uint16_t ep = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(ctx) & 0xFFFF);
    if (!con_btn_any_on_in_group(ep)) {
        return;
    }
    ESP_LOGW(TAG, "ConBtn group timeout fired -> FORCE OFF GROUP: ep=%u", (unsigned)ep);
    // Mark abort to block maintain until next PRE ON
    con_btn_group_abort_mark(ep);
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) { continue; }
        // force OFF
        con_btn_execute_off(i);
    }
}

/* ConBtn group timeout update worker (packed: [31:16]=sec_plus1, [15:0]=endpointId)
 * - sec_plus1 == 0: cancel group timer
 * - sec_plus1 > 0 : arm one-shot timer for (sec_plus1-1) seconds
 */
static void con_btn_group_timeout_update_work(intptr_t arg)
{
    uintptr_t packed = static_cast<uintptr_t>(arg);
    uint16_t ep = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t sec_plus1 = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    void * ep_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(ep));

    // Always cancel previous group timer for this endpoint.
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_timeout_timer_handler, ep_ctx);

    if (sec_plus1 == 0) {
        ESP_LOGI(TAG, "ConBtn group timeout canceled: ep=%u", (unsigned)ep);
        return;
    }
    uint32_t sec = (uint32_t)(sec_plus1 - 1u);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(sec * 1000u),
        con_btn_timeout_timer_handler,
        ep_ctx);
    ESP_LOGI(TAG, "ConBtn group timeout armed: ep=%u sec=%u", (unsigned)ep, (unsigned)sec);
}

// Work queue entrypoint to handle external OFF writes
static void con_btn_execute_off_work(intptr_t arg)
{
    uintptr_t packed = static_cast<uintptr_t>(arg);
    uint16_t idx = static_cast<uint16_t>(packed & 0xFFFF);
    if (idx < g_anna_cfg.con_btn_cnt) {
        con_btn_execute_off((int)idx);
    }
}

/* ---- Workers to arm timers on CHIP thread (avoid stack lock assert) ---- */
static void switch_cooldown_arm_work(intptr_t arg)
{
    uintptr_t packed_sw = static_cast<uintptr_t>(arg);
    // uint16_t endpoint_id = static_cast<uint16_t>(packed_sw & 0xFFFF);
    uint16_t s = static_cast<uint16_t>((packed_sw >> 16) & 0xFFFF);
    void * sw_ctx = reinterpret_cast<void *>(packed_sw);
    chip::DeviceLayer::SystemLayer().CancelTimer(switch_cooldown_release_timer_handler, sw_ctx);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(k_button_cooldown_ms),
        switch_cooldown_release_timer_handler,
        sw_ctx);
    ESP_LOGI(TAG, "Switch cooldown armed: idx=%d ms=%u blockedSoFar=%u", s, (unsigned)k_button_cooldown_ms, (unsigned)s_switch_cooldown_blocked_count[s]);
}

static void con_swt_cooldown_arm_work(intptr_t arg)
{
    uintptr_t packed_cs = static_cast<uintptr_t>(arg);
    // uint16_t endpoint_id = static_cast<uint16_t>(packed_cs & 0xFFFF);
    uint16_t c = static_cast<uint16_t>((packed_cs >> 16) & 0xFFFF);
    void * cs_ctx = reinterpret_cast<void *>(packed_cs);
    chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_cooldown_release_timer_handler, cs_ctx);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(k_button_cooldown_ms),
        con_swt_cooldown_release_timer_handler,
        cs_ctx);
    ESP_LOGI(TAG, "ConSwt cooldown armed: idx=%d ms=%u blockedSoFar=%u", c, (unsigned)k_button_cooldown_ms, (unsigned)s_con_swt_cooldown_blocked_count[c]);
}

/* Worker: arm first-press lock hold timer on CHIP thread */
static void first_lock_hold_arm_work(intptr_t)
{
    if (!s_first_lock_timer_armed) {
        s_first_lock_timer_armed = true;
        chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Milliseconds32(k_first_lock_hold_ms),
            first_lock_release_timer_handler,
            nullptr);
        ESP_LOGI(TAG, "First-lock hold timer armed %u ms", (unsigned)k_first_lock_hold_ms);
    }
}

/* Button: arm pulse auto-off and cooldown on CHIP thread */
static void button_arm_pulse_and_cooldown_work(intptr_t arg)
{
    uintptr_t packed = static_cast<uintptr_t>(arg);
    uint16_t endpoint_id = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t b = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    // Pulse auto-off (attribute OFF after hold)
    void * ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(endpoint_id));
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(k_button_pulse_ms),
        button_auto_off_timer_handler,
        ctx);
    ESP_LOGI(TAG, "Button pulse armed: ep=%u hold=%u ms", (unsigned)endpoint_id, (unsigned)k_button_pulse_ms);
    // Cooldown release
    void * cd_ctx = reinterpret_cast<void *>(packed);
    chip::DeviceLayer::SystemLayer().CancelTimer(button_cooldown_release_timer_handler, cd_ctx);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(k_button_cooldown_ms),
        button_cooldown_release_timer_handler,
        cd_ctx);
    ESP_LOGI(TAG, "Button cooldown armed: idx=%d ms=%u blockedSoFar=%u", b, (unsigned)k_button_cooldown_ms, (unsigned)s_button_cooldown_blocked_count[b]);
}

/* ConSwt: worker to execute OFF on CHIP thread */
static void con_swt_execute_off_work(intptr_t arg)
{
    uintptr_t packed = static_cast<uintptr_t>(arg);
    uint16_t idx = static_cast<uint16_t>(packed & 0xFFFF);
    if (idx < g_anna_cfg.con_swt_cnt) {
        con_swt_execute_off((int)idx);
    }
}

static void con_swt_try_pending_offs_work(intptr_t)
{
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        if (!s_con_swt_off_pending[c]) { 
            continue; 
        }
        uint16_t ep = g_anna_cfg.con_swt[c].base.endpoint_id;
        bool logical_on = con_swt_item_is_logically_on(c);
        if (!logical_on) {
            s_con_swt_off_pending[c] = false;
            s_con_swt_off_block_logged[c] = false;
            continue;
        }
        bool blocked = ((g_anna_cfg.on_pin & g_anna_cfg.con_swt[c].base.not_off_mask) != 0);
        bool cond_ok = con_swt_all_conditions_ok(c, nullptr, nullptr, nullptr);
        if (!blocked && !cond_ok) {
            ESP_LOGI(TAG, "ConSwt pending OFF retry: idx=%d (not_off cleared, cond still false)", c);
            app_driver_con_swt_group_reconcile_by_endpoint(ep);
        } else if (cond_ok) {
            ESP_LOGI(TAG, "ConSwt pending OFF canceled: idx=%d (conditions recovered)", c);
            s_con_swt_off_pending[c] = false;
            s_con_swt_off_block_logged[c] = false;
            app_driver_con_swt_group_reconcile_by_endpoint(ep);
        }
    }
}

static void con_swt_execute_off(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) { 
        return; 
    }
    int pin_no = (int)g_anna_cfg.con_swt[idx].base.pin_no;
    bool gpio_off = con_swt_request_gpio_off_by_index(idx);
    if (gpio_off) {
        ESP_LOGI(TAG, "ConSwt runtime GPIO OFF: idx=%d ep=%u pin=%d on_pin=0x%08x",
                 idx,
                 (unsigned)g_anna_cfg.con_swt[idx].base.endpoint_id,
                 pin_no,
                 (unsigned)g_anna_cfg.on_pin);
    }
    con_swt_refresh_as_eval_timer();
}

extern "C" void app_driver_con_swt_force_off_by_index(int idx)
{
    if (idx < 0 || idx >= g_anna_cfg.con_swt_cnt) { return; }
    // Run OFF path on CHIP thread to avoid SystemLayer timer/lock contention (older flow called directly)
    uintptr_t packed = static_cast<uintptr_t>(static_cast<uint16_t>(idx));
    chip::DeviceLayer::PlatformMgr().ScheduleWork(con_swt_execute_off_work, (intptr_t)packed);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    ESP_LOGI(TAG, "app_driver_attribute_update: Start");

    /* mode 관련 endpoint인지 확인 */
    /* 방법1: for문으로 endpoint 확인, 방법2: 시작하는 값이 뭔지로 파악하기(아직 ep id 변경은 고려 중. 따라서 for문 사용)*/
    /* set() 함수를 사용할 경우, pre_update()가 아닌 post_update()에서 처리하기 때문에 set() 함수 사용시 문제없음*/
    for (int i = 0; i < g_anna_cfg.modes.mode_count; i++) {
        if (endpoint_id != g_anna_cfg.modes.endpoint_id[i]) {
            continue;
        }
        ESP_LOGI(TAG, "Mode[%d] endpoint: %d", i, g_anna_cfg.modes.endpoint_id[i]);
        /* 1) not_mode_mask에 해당하는 핀이 on 상태라면 mode 변경 안함 */
        bool has_not_mode_overlap = (g_anna_cfg.on_pin & g_anna_cfg.modes.not_mode_mask) != 0;
        if (has_not_mode_overlap) {
            return ESP_ERR_NOT_ALLOWED;
        }
            /* 2) not_mode_mask에 해당하는 핀이 모두 off 상태라면 모드 변경 처리 */
        /* 아래 로직은 OnOff 클러스터의 OnOff 속성에만 적용 */
        if (!(cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)) {
            return ESP_OK;
        }

        // // 2) 요청값(컨트롤러가 쓰려는 값)
        // 컨트롤러가 ON을 요청한 모드만 디바운스 대상에 기록
        bool desired = val->val.b;

        // Allow internal apply writes to pass through PRE unconditionally
        if (s_internal_write) {
            return ESP_OK;
        }

        // First-press lock: while first mode is being applied, reject others
        if (s_first_lock_active) {
            if (i != s_first_lock_idx) {
                ESP_LOGI(TAG, "PRE reject due to first-lock: reqIdx=%d lockedIdx=%d", i, s_first_lock_idx);
                return ESP_ERR_NOT_ALLOWED;
            } else {
                // Same index as locked; allow silently
                return ESP_OK;
            }
        }

        if (desired) {
            // Start first-press lock
            s_first_lock_active = true;
            s_first_lock_idx = i;
            s_last_requested_mode_idx = i;
            s_last_request_seq++;
            ESP_LOGI(TAG, "First-lock start: idx=%d seq=%u", s_first_lock_idx, (unsigned)s_last_request_seq);
            if (!s_first_lock_timer_armed) {
                chip::DeviceLayer::PlatformMgr().ScheduleWork(first_lock_hold_arm_work, 0);
            }
        }
        return ESP_OK;
    }
    /* Button PRE gate and pulse schedule */
    for (int b = 0; b < g_anna_cfg.button_cnt; ++b) {
        if (endpoint_id != g_anna_cfg.a_button[b].base.endpoint_id) {
            continue;
        }
        if (!(cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)) {
            return ESP_OK;
        }

        // Internal writes from timer should always pass
        if (s_internal_write) {
            return ESP_OK;
        }

        bool desired = val->val.b;
        if (desired) {
            // Cooldown gate: reject new press within cooldown window
            if (s_button_cooldown_active[b]) {
                s_button_cooldown_blocked_count[b]++;
                ESP_LOGW(TAG, "Button PRE reject (cooldown) idx=%d ep=%u blocked=%u", b, (unsigned)endpoint_id, (unsigned)s_button_cooldown_blocked_count[b]);
                return ESP_ERR_NOT_ALLOWED;
            }
            bool has_overlap = (g_anna_cfg.on_pin & g_anna_cfg.a_button[b].base.not_on_mask) != 0;
            if (has_overlap) {
                ESP_LOGI(TAG, "Button PRE reject due to not_on_mask overlap: ep=%u", (unsigned)endpoint_id);
                return ESP_ERR_NOT_ALLOWED;
            }
            // Drive the associated GPIO HIGH (ON) immediately
            int pin_no = (int)g_anna_cfg.a_button[b].base.pin_no;
            gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
            if (GPIO_IS_VALID_GPIO(gpio)) {
                if (!s_button_gpio_inited[b]) {
                    // Fallback one-time init if not initialized during startup
                    gpio_reset_pin(gpio);
                    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
                    s_button_gpio_inited[b] = true;
                    ESP_LOGW(TAG, "Button GPIO late-init: idx=%d pin=%d", b, pin_no);
                }
                gpio_set_level(gpio, 1);
                ESP_LOGI(TAG, "Button GPIO ON: idx=%d ep=%u pin=%d", b, (unsigned)endpoint_id, pin_no);
            } else {
                ESP_LOGW(TAG, "Invalid GPIO for button idx=%d pin=%d", b, pin_no);
            }
            // Arm pulse auto-off and cooldown via CHIP-thread worker
            s_button_cooldown_active[b] = true;
            uintptr_t packed = (static_cast<uintptr_t>(static_cast<uint16_t>(b)) << 16) |
                               static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
            // Schedule timers on CHIP thread to avoid SystemLayer lock issues; GPIO was already toggled synchronously
            chip::DeviceLayer::PlatformMgr().ScheduleWork(button_arm_pulse_and_cooldown_work, (intptr_t)packed);
        }
        return ESP_OK;
    }
    /* Switch PRE gate and state application */
    for (int s = 0; s < g_anna_cfg.switch_cnt; ++s) {
        if (endpoint_id != g_anna_cfg.a_switch[s].base.endpoint_id) {
            continue;
        }
        if (!(cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)) {
            return ESP_OK;
        }
        if (s_internal_write) {
            return ESP_OK;
        }
        bool desired_sw = val->val.b;
        // Cooldown for switch (shared constant, currently 350ms)
        if (s_switch_cooldown_active[s]) {
            s_switch_cooldown_blocked_count[s]++;
            ESP_LOGW(TAG, "Switch PRE reject (cooldown) idx=%d ep=%u blocked=%u", s, (unsigned)endpoint_id, (unsigned)s_switch_cooldown_blocked_count[s]);
            return ESP_ERR_NOT_ALLOWED;
        }
        // NotOn/NotOff gates
        if (desired_sw) {
            bool has_not_on_overlap = (g_anna_cfg.on_pin & g_anna_cfg.a_switch[s].base.not_on_mask) != 0;
            if (has_not_on_overlap) {
                ESP_LOGI(TAG, "Switch PRE reject due to not_on_mask overlap: ep=%u", (unsigned)endpoint_id);
                return ESP_ERR_NOT_ALLOWED;
            }
        } else {
            bool has_not_off_overlap = (g_anna_cfg.on_pin & g_anna_cfg.a_switch[s].base.not_off_mask) != 0;
            if (has_not_off_overlap) {
                ESP_LOGI(TAG, "Switch PRE reject due to not_off_mask overlap: ep=%u", (unsigned)endpoint_id);
                return ESP_ERR_NOT_ALLOWED;
            }
        }
        // Apply physical GPIO immediately and update on_pin mask
        {
            int pin_no = (int)g_anna_cfg.a_switch[s].base.pin_no;
            gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
            if (GPIO_IS_VALID_GPIO(gpio)) {
                if (!s_switch_gpio_inited[s]) {
                    // Fallback one-time init if not initialized during startup
                    gpio_reset_pin(gpio);
                    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
                    s_switch_gpio_inited[s] = true;
                    ESP_LOGW(TAG, "Switch GPIO late-init: idx=%d pin=%d", s, pin_no);
                }
                gpio_set_level(gpio, desired_sw ? 1 : 0);
                update_on_pin_mask((uint8_t)pin_no, desired_sw);
                ESP_LOGI(TAG, "Switch GPIO set: idx=%d ep=%u pin=%d level=%d on_pin=0x%08x", s, (unsigned)endpoint_id, pin_no, desired_sw ? 1 : 0, (unsigned)g_anna_cfg.on_pin);
            } else {
                ESP_LOGW(TAG, "Invalid GPIO for switch idx=%d pin=%d", s, pin_no);
            }
        }
        // Arm cooldown timer for switch
        s_switch_cooldown_active[s] = true;
        uintptr_t packed_sw = (static_cast<uintptr_t>(static_cast<uint16_t>(s)) << 16) |
                              static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
        // Arm switch cooldown on CHIP thread (prevent SystemLayer access from PRE context)
        chip::DeviceLayer::PlatformMgr().ScheduleWork(switch_cooldown_arm_work, (intptr_t)packed_sw);
        return ESP_OK;
    }
    /* ConBtn PRE gate (group-OR; NotOffPin excluded) */
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        if (endpoint_id != g_anna_cfg.con_btn[c].base.endpoint_id) {
            continue;
        }
        if (!(cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)) {
            return ESP_OK;
        }
        if (s_internal_write) {
            return ESP_OK;
        }
        bool desired_cb = val->val.b;

        // Build group indices for this endpoint
        int idx_list[ANNA_MAX_CON_ACT];
        int idx_count = 0;
        for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
            if (g_anna_cfg.con_btn[i].base.endpoint_id == endpoint_id) {
                idx_list[idx_count++] = i;
            }
        }

        if (!desired_cb) {
            // Group OFF: turn OFF all group items (no NotOffPin for con_btn)
            for (int k = 0; k < idx_count; ++k) {
                uintptr_t packed_cf = (static_cast<uintptr_t>(static_cast<uint16_t>(idx_list[k])));
                chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_execute_off_work, (intptr_t)packed_cf);
            }
            // Cancel endpoint-level group timeout
            {
                uintptr_t packed_to = (static_cast<uintptr_t>(0u) << 16) |
                                      static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
                chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_group_timeout_update_work, (intptr_t)packed_to);
            }
            // Mark abort so maintain won't resurrect until next PRE ON
            con_btn_group_abort_mark(endpoint_id);
            return ESP_OK;
        }

        // Cooldown gate: if any group item is in cooldown, reject (use first hit)
        for (int k = 0; k < idx_count; ++k) {
            int ci = idx_list[k];
            if (s_con_btn_cooldown_active[ci]) {
                s_con_btn_cooldown_blocked_count[ci]++;
                ESP_LOGW(TAG, "ConBtn PRE reject (cooldown) idx=%d ep=%u blocked=%u", ci, (unsigned)endpoint_id, (unsigned)s_con_btn_cooldown_blocked_count[ci]);
                return ESP_ERR_NOT_ALLOWED;
            }
        }

        // Group not_on gate
        pin_mask_t group_not_on_mask = 0;
        for (int k = 0; k < idx_count; ++k) {
            group_not_on_mask |= g_anna_cfg.con_btn[idx_list[k]].base.not_on_mask;
        }
        if ((g_anna_cfg.on_pin & group_not_on_mask) != 0) {
            ESP_LOGI(TAG, "ConBtn PRE reject due to group not_on_mask overlap: ep=%u on_pin=0x%08x mask=0x%08x", (unsigned)endpoint_id, (unsigned)g_anna_cfg.on_pin, (unsigned)group_not_on_mask);
            return ESP_ERR_NOT_ALLOWED;
        }

        // any_satisfied in group: (mode ∧ bs ∧ as) OR
        bool any_satisfied = false;
        // endpoint group timeout seconds = max(max_sec) among satisfied items for this ON request,
        // but only for BS/AS-backed conditional-hold groups.
        int32_t group_max_sec = -1;
        con_btn_group_run_mode group_mode = con_btn_group_mode_for_endpoint(endpoint_id);
        for (int k = 0; k < idx_count; ++k) {
            int ci = idx_list[k];
            if (con_btn_item_pre_on_conditions_ok(ci)) {
                any_satisfied = true;
                if (group_mode == con_btn_group_run_mode::conditional_hold) {
                    int32_t sec = (int32_t)g_anna_cfg.con_btn[ci].max_sec;
                    if (sec > group_max_sec) {
                        group_max_sec = sec;
                    }
                }
            }
        }
        if (!any_satisfied) {
            ESP_LOGI(TAG, "ConBtn PRE reject (no satisfied item in group): ep=%u", (unsigned)endpoint_id);
            return ESP_ERR_NOT_ALLOWED;
        }

        // Execute group ON: set ALL pins HIGH and is_on=true; arm pulse/cooldown once per representative
        for (int k = 0; k < idx_count; ++k) {
            int ci = idx_list[k];
            // Drive GPIO HIGH synchronously (reuse existing function via work or inline similar to con_btn_execute_on)
            // Keep the existing execution path for consistency (may arm multiple timers; acceptable for now)
            uintptr_t packed_ce = (static_cast<uintptr_t>(static_cast<uint16_t>(ci)) << 16) |
                                  static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
            chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_execute_on_work, (intptr_t)packed_ce);
        }

        // Arm (or cancel) endpoint-level group timeout only for conditional-hold groups.
        if (group_mode != con_btn_group_run_mode::pulse) {
            uint16_t sec_plus1 = 0; // 0 == cancel/disabled
            if (group_max_sec >= 0) {
                uint32_t sec_u = (uint32_t)group_max_sec;
                if (sec_u > 65534u) {
                    sec_u = 65534u;
                }
                sec_plus1 = (uint16_t)(sec_u + 1u);
            }
            uintptr_t packed_to = (static_cast<uintptr_t>(sec_plus1) << 16) |
                                  static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
            chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_group_timeout_update_work, (intptr_t)packed_to);
        } else {
            uintptr_t packed_to = static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
            chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_group_timeout_update_work, (intptr_t)packed_to);
        }

        // Clear abort on successful group ON
        con_btn_group_abort_clear(endpoint_id);
        ESP_LOGI(TAG, "ConBtn PRE accept: ep=%u mode=%s group_max_sec=%ld", (unsigned)endpoint_id, con_btn_group_mode_name(group_mode), (long)group_max_sec);
        return ESP_OK;
    }
    /* ConSwt PRE gate (latched; not_off mask supported; no timeout) */
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        if (endpoint_id != g_anna_cfg.con_swt[c].base.endpoint_id) {
            continue;
        }
        if (!(cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id)) {
            return ESP_OK;
        }
        if (s_internal_write) {
            return ESP_OK;
        }
        bool desired_cs = val->val.b;
        // NEW: Group-based PRE handling (old per-item logic commented above)
        // Collect all indices sharing this endpoint
        int idx_list[ANNA_MAX_CON_SWT_ACT];
        int idx_count = 0;
        for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
            if (g_anna_cfg.con_swt[i].base.endpoint_id == endpoint_id) {
                idx_list[idx_count++] = i;
            }
        }
        if (desired_cs) {
            // Cooldown gate: reject new ON within cooldown window (same representative used for arming)
            if (idx_count > 0) {
                int rep = idx_list[0];
                if (s_con_swt_cooldown_active[rep]) {
                    s_con_swt_cooldown_blocked_count[rep]++;
                    ESP_LOGW(TAG, "ConSwt PRE reject (cooldown) idx=%d ep=%u blocked=%u",
                             rep, (unsigned) endpoint_id, (unsigned) s_con_swt_cooldown_blocked_count[rep]);
                    return ESP_ERR_NOT_ALLOWED;
                }
            }
            // Group-level NotOnPin gate: if any item's not_on_mask overlaps current on_pin, reject ON
            pin_mask_t group_not_on_mask = 0;
            for (int k = 0; k < idx_count; ++k) {
                int ci = idx_list[k];
                group_not_on_mask |= g_anna_cfg.con_swt[ci].base.not_on_mask;
            }
            if ((g_anna_cfg.on_pin & group_not_on_mask) != 0) {
                ESP_LOGI(TAG, "ConSwt PRE reject due to group not_on_mask overlap: ep=%u on_pin=0x%08x mask=0x%08x", (unsigned)endpoint_id, (unsigned)g_anna_cfg.on_pin, (unsigned)group_not_on_mask);
                return ESP_ERR_NOT_ALLOWED;
            }
            // mode/bs/as gating is no longer part of PRE ON. GPIO state is
            // reconciled after commit using the current runtime conditions.
            // Arm cooldown once (use first index for counter)
            if (idx_count > 0) {
                int ci = idx_list[0];
                s_con_swt_cooldown_active[ci] = true;
                uintptr_t packed_cs = (static_cast<uintptr_t>(static_cast<uint16_t>(ci)) << 16) |
                                      static_cast<uintptr_t>(static_cast<uint16_t>(endpoint_id));
                chip::DeviceLayer::PlatformMgr().ScheduleWork(con_swt_cooldown_arm_work, (intptr_t)packed_cs);
            }
            ESP_LOGI(TAG, "ConSwt PRE accept: ep=%u (GPIO decision deferred to post-commit reconcile)", (unsigned)endpoint_id);
            return ESP_OK;
        } else {
            // Group-level NotOffPin gate: if any item's not_off_mask overlaps current on_pin, reject OFF
            pin_mask_t group_not_off_mask = 0;
            for (int k = 0; k < idx_count; ++k) {
                int ci = idx_list[k];
                group_not_off_mask |= g_anna_cfg.con_swt[ci].base.not_off_mask;
            }
            if ((g_anna_cfg.on_pin & group_not_off_mask) != 0) {
                ESP_LOGI(TAG, "ConSwt PRE reject due to group not_off_mask overlap: ep=%u on_pin=0x%08x mask=0x%08x", (unsigned)endpoint_id, (unsigned)g_anna_cfg.on_pin, (unsigned)group_not_off_mask);
                return ESP_ERR_NOT_ALLOWED;
            }
            ESP_LOGI(TAG, "ConSwt PRE OFF accept: ep=%u (GPIO OFF deferred to post-commit reconcile)", (unsigned)endpoint_id);
            return ESP_OK;
        }
    }
    return ESP_OK;
}

/* Internal: compute and apply final mode state given a winner index. */
// Turn off all other modes and ensure the winner mode is ON
static void apply_mode_winner(int winner_idx)
{
    if (winner_idx < 0 || winner_idx >= g_anna_cfg.modes.mode_count) {
        return;
    }
    s_in_apply = true;
    s_internal_write = true;
    // Phase 1: turn OFF all others
    for (int j = 0; j < g_anna_cfg.modes.mode_count; j++) {
        if (j == winner_idx) { continue; }
        bool cur = false;
        (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[j], &cur);
        if (cur) {
            ESP_LOGI(TAG, "apply_mode_winner cur: true %d", j);
            (void) OnOff::Attributes::OnOff::Set(g_anna_cfg.modes.endpoint_id[j], false);
        }
    }
    // Phase 2: ensure winner is ON
    {
        bool cur = false;
        (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[winner_idx], &cur);
        if (!cur) {
            ESP_LOGI(TAG, "apply_mode_winner cur: false %d", winner_idx);
            (void) OnOff::Attributes::OnOff::Set(g_anna_cfg.modes.endpoint_id[winner_idx], true);
        }
    }
    g_anna_cfg.on_mode = winner_idx;
    // Reflect mode on RGB LED if available
    if (g_led_handle) {
        (void) led_controller_set_color_idx(g_led_handle, (uint8_t)winner_idx);
    }
    s_internal_write = false;
    s_in_apply = false;
}

// First-lock release timer callback: clear the lock after the hold window
static void first_lock_release_timer_handler(chip::System::Layer *, void *)
{
    if (s_first_lock_active) {
        ESP_LOGI(TAG, "First-lock release timer: idx=%d", s_first_lock_idx);
        s_first_lock_active = false;
        s_first_lock_idx = -1;
    }
    s_first_lock_timer_armed = false;
}


// Deprecated: kept for compatibility with older flow
esp_err_t app_driver_apply_mode_invariants_debounced(uint32_t seq)
{
    ESP_LOGI(TAG, "Debounce path deprecated; seq=%u", (unsigned)seq);
    return ESP_OK;
}

// Apply invariant: ensure exactly one mode is ON according to winner policy
static void mode_apply_work_handler(intptr_t)
{
    if (s_in_apply) {
        ESP_LOGI(TAG, "Apply skip: already inApply");
        s_apply_queued = false;
        return;
    }

    s_in_apply = true;

    bool stable = false;
    int maxPass = 4; // safety bound (short), re-queue if not yet stable
    while (maxPass-- > 0) {
        uint32_t startSeq = s_last_request_seq;
        s_apply_queued = false; // clear pending for this pass

        // Snapshot current committed states
        int onCount = 0;
        int lastOnIdx = -1;
        for (int j = 0; j < g_anna_cfg.modes.mode_count; j++) {
            bool s = false;
            (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[j], &s);
            if (s) { 
                onCount++; 
                lastOnIdx = j; 
            }
        }

        if (onCount == 0) {
            ESP_LOGI(TAG, "Apply: all off -> index 0 only");
            apply_mode_winner(0);
        } else if (onCount > 1) {
            int winner = s_first_lock_active ? s_first_lock_idx : ((s_last_requested_mode_idx >= 0) ? s_last_requested_mode_idx : 0);
            ESP_LOGI(TAG, "Apply: multi-on=%d -> winner=%d", onCount, winner);
            apply_mode_winner(winner);
        } else {
            if (lastOnIdx >= 0) {
                g_anna_cfg.on_mode = lastOnIdx;
                ESP_LOGI(TAG, "Apply: single-on -> sync on_mode=%d", lastOnIdx);
                if (g_led_handle) {
                    (void) led_controller_set_color_idx(g_led_handle, (uint8_t)lastOnIdx);
                }
            }
        }

        // Re-snapshot to verify stability
        int verifyOnCount = 0;
        int verifyOnIdx = -1;
        for (int j = 0; j < g_anna_cfg.modes.mode_count; j++) {
            bool s = false;
            (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[j], &s);
            if (s) { verifyOnCount++; verifyOnIdx = j; }
        }

        bool new_intent_arrived = (s_last_request_seq != startSeq);
        stable = (!new_intent_arrived && !s_apply_queued && verifyOnCount == 1 && (!s_first_lock_active || verifyOnIdx == s_first_lock_idx));
        if (stable) {
            break; // stable
        }
        ESP_LOGI(TAG, "Apply: re-run (pending=%d, newIntent=%d, onCount=%d)", s_apply_queued ? 1 : 0, new_intent_arrived ? 1 : 0, verifyOnCount);
    }

    // Final summary after apply
    ESP_LOGI(TAG, "Mode summary after apply:");
    for (int j = 0; j < g_anna_cfg.modes.mode_count; j++) {
        bool s = false;
        (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[j], &s);
        ESP_LOGI(TAG, "  Mode[%d] ep=%u on=%d", j, (unsigned)g_anna_cfg.modes.endpoint_id[j], s ? 1 : 0);
    }

    // Enforce con_btn per endpoint (Label-group OR, maintain-only, PRE-only auto-ON)
    {
        // Build list of distinct endpoints in ConBtn
        uint16_t ep_list_cb[ANNA_MAX_CON_ACT];
        int ep_count_cb = 0;
        for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
            uint16_t ep = g_anna_cfg.con_btn[i].base.endpoint_id;
            bool seen = false;
            for (int j = 0; j < ep_count_cb; ++j) {
                if (ep_list_cb[j] == ep) { seen = true; break; }
            }
            if (!seen && ep != ENDPOINT_ID_INVALID && ep_count_cb < (int)(sizeof(ep_list_cb)/sizeof(ep_list_cb[0]))) {
                ep_list_cb[ep_count_cb++] = ep;
            }
        }
        for (int e = 0; e < ep_count_cb; ++e) {
            uint16_t ep = ep_list_cb[e];
            if (con_btn_group_mode_for_endpoint(ep) != con_btn_group_run_mode::conditional_hold) {
                continue;
            }
            bool any_satisfied = false;
            for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
                if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) { continue; }
                if (con_btn_item_hold_conditions_ok(i)) { any_satisfied = true; }
            }
            if (any_satisfied) {
                // Maintain-only: ensure group pins ON if group already ON; do not auto-ON here
                (void) app_driver_con_btn_maintain_group_on_if_satisfied(ep);
            } else {
                // No satisfied items: force OFF all items in this endpoint group
                for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
                    if (g_anna_cfg.con_btn[i].base.endpoint_id != ep) { continue; }
                    if (g_anna_cfg.con_btn[i].is_on) {
                        con_btn_execute_off(i);
                    }
                }
            }
        }
    }
    // Enforce con_swt runtime condition AFTER winner commit
    {
        uint16_t ep_list[ANNA_MAX_CON_SWT_ACT];
        int ep_count = 0;
        for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
            uint16_t ep = g_anna_cfg.con_swt[i].base.endpoint_id;
            bool seen = false;
            for (int j = 0; j < ep_count; ++j) {
                if (ep_list[j] == ep) { 
                    seen = true; break; 
                }
            }
            if (!seen && ep != ENDPOINT_ID_INVALID && ep_count < (int)(sizeof(ep_list)/sizeof(ep_list[0]))) {
                ep_list[ep_count++] = ep;
            }
        }
        for (int e = 0; e < ep_count; ++e) {
            uint16_t ep = ep_list[e];
            app_driver_con_swt_group_reconcile_by_endpoint(ep);
        }
    }

    // Do NOT clear first-lock here; timer controls release

    s_in_apply = false;

    if (s_apply_queued) {
        // schedule another pass
        s_apply_queued = false; // will be set again by incoming POSTs; force one more pass now
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t){ mode_apply_work_handler(0); }, 0);
    }
}

void app_driver_queue_mode_apply(void)
{
    if (s_in_apply) {
        // Mark another pass is needed after current apply
        s_apply_queued = true;
        ESP_LOGI(TAG, "Queue marked pending (inApply)");
        return;
    }
    if (s_apply_queued) {
        ESP_LOGI(TAG, "Queue skip: already queued");
        return;
    }
    s_apply_queued = true;
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t){ mode_apply_work_handler(0); }, 0);
}

/* Boot-only reconcile:
 * - preserve persisted single-on state
 * - normalize all-off/multi-on to index 0 only
 * - do not re-enter runtime apply or downstream action reconcile paths
 */
extern "C" void app_driver_boot_reconcile_mode_only(void)
{
    if (g_anna_cfg.modes.mode_count <= 0) {
        ESP_LOGI(TAG, "Boot mode reconcile skipped: no mode endpoints");
        return;
    }

    int onCount = 0;
    int lastOnIdx = -1;
    for (int j = 0; j < g_anna_cfg.modes.mode_count; ++j) {
        bool s = false;
        (void) OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[j], &s);
        if (s) {
            onCount++;
            lastOnIdx = j;
        }
    }

    if (onCount == 1 && lastOnIdx >= 0) {
        g_anna_cfg.on_mode = lastOnIdx;
        if (g_led_handle) {
            (void) led_controller_set_color_idx(g_led_handle, (uint8_t)lastOnIdx);
            ESP_LOGI(TAG, "LED synced to persisted single-on mode: %d", lastOnIdx);
        } else {
            ESP_LOGI(TAG, "Boot mode preserved without LED: %d", lastOnIdx);
        }
        return;
    }

    if (onCount == 0) {
        ESP_LOGI(TAG, "Boot mode reconcile: all off -> index 0 only");
    } else {
        ESP_LOGI(TAG, "Boot mode reconcile: multi-on=%d -> index 0 only", onCount);
    }
    apply_mode_winner(0);
}

extern "C" void app_driver_boot_safe_off_sync_non_mode(void)
{
    uint16_t con_btn_ep_list[ANNA_MAX_CON_ACT] = {};
    int con_btn_ep_count = 0;
    uint16_t con_swt_ep_list[ANNA_MAX_CON_SWT_ACT] = {};
    int con_swt_ep_count = 0;

    for (int b = 0; b < g_anna_cfg.button_cnt; ++b) {
        boot_safe_off_button_idx(b);
    }
    for (int s = 0; s < g_anna_cfg.switch_cnt; ++s) {
        boot_safe_off_switch_idx(s);
    }

    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        boot_safe_off_con_btn_idx(c);
        uint16_t ep = g_anna_cfg.con_btn[c].base.endpoint_id;
        bool seen = false;
        for (int i = 0; i < con_btn_ep_count; ++i) {
            if (con_btn_ep_list[i] == ep) {
                seen = true;
                break;
            }
        }
        if (!seen && ep != ENDPOINT_ID_INVALID && con_btn_ep_count < ANNA_MAX_CON_ACT) {
            con_btn_ep_list[con_btn_ep_count++] = ep;
        }
    }
    if (s_con_btn_as_timer_armed && !any_con_btn_on_with_as()) {
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_as_eval_timer_handler, nullptr);
        s_con_btn_as_timer_armed = false;
    }
    for (int i = 0; i < con_btn_ep_count; ++i) {
        uint16_t ep = con_btn_ep_list[i];
        void * ep_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(ep));
        con_btn_cancel_endpoint_auto_timers(ep);
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_timeout_timer_handler, ep_ctx);
        con_btn_group_abort_clear(ep);
        s_internal_write = true;
        (void) OnOff::Attributes::OnOff::Set(ep, false);
        s_internal_write = false;
    }

    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        boot_safe_off_con_swt_idx(c);
        uint16_t ep = g_anna_cfg.con_swt[c].base.endpoint_id;
        bool seen = false;
        for (int i = 0; i < con_swt_ep_count; ++i) {
            if (con_swt_ep_list[i] == ep) {
                seen = true;
                break;
            }
        }
        if (!seen && ep != ENDPOINT_ID_INVALID && con_swt_ep_count < ANNA_MAX_CON_SWT_ACT) {
            con_swt_ep_list[con_swt_ep_count++] = ep;
        }
    }
    if (s_con_swt_as_timer_armed && !any_con_swt_on()) {
        chip::DeviceLayer::SystemLayer().CancelTimer(con_swt_as_eval_timer_handler, nullptr);
        s_con_swt_as_timer_armed = false;
    }
    for (int i = 0; i < con_swt_ep_count; ++i) {
        uint16_t ep = con_swt_ep_list[i];
        s_internal_write = true;
        (void) OnOff::Attributes::OnOff::Set(ep, false);
        s_internal_write = false;
    }

    // All tracked maintained outputs are forced LOW at this point.
    g_anna_cfg.on_pin = 0;
    ESP_LOGI(TAG, "Boot safe-off sync complete: button=%d switch=%d con_btn=%d con_swt=%d",
             g_anna_cfg.button_cnt, g_anna_cfg.switch_cnt, g_anna_cfg.con_btn_cnt, g_anna_cfg.con_swt_cnt);
}

void app_driver_schedule_mode_debounce(void)
{
    // No longer used; kept for compatibility
    ESP_LOGI(TAG, "Debounce schedule ignored (using immediate apply)");
}

esp_err_t app_driver_attribute_post_update(uint16_t endpoint_id, uint32_t cluster_id,
                                           uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    (void) val;
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) {
        return ESP_OK;
    }
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) {
            continue;
        }
        app_driver_con_swt_group_reconcile_by_endpoint(endpoint_id);
        break;
    }
    return ESP_OK;
}

esp_err_t app_driver_led_init(void)
{
    // Initialize the new led_controller
    // The GPIO pin is defined in app_priv.h
    return led_controller_init(GPIO_RGB_CONTROLLER, &g_led_handle);
}

/* Maintain-only group ON: if the endpoint group is already physically ON and
 * any sibling item still satisfies AND conditions, re-assert all group pins ON
 * and keep Attribute ON. If the group is fully OFF, do nothing (no auto-ON).
 */
extern "C" bool app_driver_con_swt_maintain_group_on_if_satisfied(uint16_t endpoint_id)
{
    bool logical_on = con_swt_endpoint_is_logically_on(endpoint_id);
    if (!logical_on) {
        return false;
    }
    bool any_on_in_group = false;
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) { continue; }
        if (con_swt_is_on_by_pin(i)) { any_on_in_group = true; break; }
    }
    if (!any_on_in_group) {
        return false; // do not create ON from fully-OFF state
    }
    // Evaluate if any item is currently satisfied (AND of Mode/BS/AS)
    bool any_satisfied = false;
    pin_mask_t group_not_on_mask = 0;
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (g_anna_cfg.con_swt[i].base.endpoint_id != endpoint_id) { continue; }
        group_not_on_mask |= g_anna_cfg.con_swt[i].base.not_on_mask;
        int m = g_anna_cfg.con_swt[i].mode_idx;
        bool mode_ok = (m < 0) || (g_anna_cfg.on_mode == m);
        bool bs_ok = con_swt_bs_conditions_ok(i);
        bool as_ok = con_swt_as_conditions_ok(i);
        if (mode_ok && bs_ok && as_ok) { any_satisfied = true; }
    }
    if (!any_satisfied) {
        return false;
    }
    // Respect group not_on gate; if blocked, do not modify pins
    if ((g_anna_cfg.on_pin & group_not_on_mask) != 0) {
        ESP_LOGW(TAG, "ConSwt maintain blocked by group not_on_mask: ep=%u on_pin=0x%08x", (unsigned)endpoint_id, (unsigned)g_anna_cfg.on_pin);
        return false;
    }
    app_driver_con_swt_group_reconcile_by_endpoint(endpoint_id);
    ESP_LOGI(TAG, "ConSwt maintain group ON: ep=%u", (unsigned)endpoint_id);
    return true;
}
