#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <stdlib.h>
#include <sdkconfig.h>
#include <platform/DeviceInfoProvider.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemLayer.h>

#include "esp_adc/adc_oneshot.h"

#include "anna_cfg.h"
#include "app_priv.h"
#include "common_macros.h"

#include "led_controller.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

using chip::CharSpan;
using chip::EndpointId;
using chip::DeviceLayer::AttributeList;
using chip::DeviceLayer::GetDeviceInfoProvider;
using UserLabelType =
        chip::app::Clusters::UserLabel::Structs::LabelStruct::Type;


static const char *TAG = "app_settings";
/* ---- con_btn: boolean sensor IRQ plumbing ---- */
static bool s_bs_isr_installed = false;
static bool s_bs_pin_inited[32] = {false};
static bool s_bs_db_timer_armed[32] = {false};
static uint8_t s_bs_cached_level[32] = {0};
static QueueHandle_t s_bs_isr_queue = nullptr;
static TaskHandle_t s_bs_isr_task = nullptr;
static void con_btn_bs_isr_worker(void *);

extern "C" int app_bs_get_debounced_level(int pin)
{
    if (pin < 0 || pin >= 32) {
        return -1;
    }
    return (int)s_bs_cached_level[pin];
}

/* ---- con_btn: analog sensor (ADC oneshot) ---- */
static adc_oneshot_unit_handle_t s_adc1_unit = nullptr;

static bool ensure_adc1_unit()
{
    if (s_adc1_unit) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.clk_src = ADC_DIGI_CLK_SRC_DEFAULT;
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc1_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 unit init failed: %d", (int)err);
        s_adc1_unit = nullptr;
        return false;
    }
    return true;
}

static bool ensure_adc1_channel_for_gpio(int gpio, adc_channel_t *out_ch)
{
    if (gpio < 0) {
        return false;
    }
    if (!ensure_adc1_unit()) {
        return false;
    }
    adc_channel_t ch = (adc_channel_t)0;
    adc_unit_t unit_id = ADC_UNIT_1;
    /* IDF v5.4 API: io_to_channel(io_num, &unit_id, &channel) */
    esp_err_t err = adc_oneshot_io_to_channel((int)gpio, &unit_id, &ch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC io->ch map failed for gpio=%d err=%d", gpio, (int)err);
        return false;
    }
    if (unit_id != ADC_UNIT_1) {
        ESP_LOGW(TAG, "ADC unit mismatch for gpio=%d: unit=%d (expected ADC_UNIT_1)", gpio, (int)unit_id);
        return false;
    }
    adc_oneshot_chan_cfg_t ch_cfg = {};
    ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ch_cfg.atten = ADC_ATTEN_DB_12; /* 0~3.3V 근사 범위 (DB_11은 deprecated) */
    err = adc_oneshot_config_channel(s_adc1_unit, ch, &ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC ch config failed for gpio=%d ch=%d err=%d", gpio, (int)ch, (int)err);
        return false;
    }
    if (out_ch) {
        *out_ch = ch;
    }
    return true;
}

extern "C" bool app_read_analog_scaled(uint8_t gpio, int * out_scaled)
{
    adc_channel_t ch;
    if (!ensure_adc1_channel_for_gpio((int)gpio, &ch)) {
        return false;
    }
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc1_unit, ch, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: gpio=%u ch=%d err=%d", (unsigned)gpio, (int)ch, (int)err);
        return false;
    }
    if (raw < 0) raw = 0;
#if SOC_ADC_RTC_MAX_BITWIDTH
    const int k_max = (1 << SOC_ADC_RTC_MAX_BITWIDTH) - 1;
#else
    const int k_max = 4095; /* fallback */
#endif
    if (raw > k_max) raw = k_max;
    int scaled = (raw * 100 + (k_max / 2)) / k_max; /* round */
    ESP_LOGI(TAG, "ADC read: gpio=%u raw=%d scaled=%d", (unsigned)gpio, raw, scaled);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 100) {
        scaled = 100;
    }
    if (out_scaled) {
        *out_scaled = scaled;
    }
    return true;
}

static void con_btn_bs_work_handler(intptr_t arg)
{
    int pin = (int) arg;
    if (pin < 0 || pin >= 32) {
        return;
    }
    int level = -1;
    if (GPIO_IS_VALID_GPIO((gpio_num_t)pin)) {
        level = gpio_get_level((gpio_num_t)pin);
    }
    ESP_LOGI(TAG, "ConBtn BS IRQ-WORK: pin=%d level=%d", pin, level);

    if (pin >= 0 && pin < 32 && level >= 0) {
        s_bs_cached_level[pin] = (uint8_t)(level ? 1 : 0);
    }

    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        bool this_btn_refs_pin = false;
        bool bs_ok_all = true;
        /* Check all bs pins for this con_btn */
        for (int i = 0; i < ANNA_MAX_CON_BS_COUNT; ++i) {
            int bpin = g_anna_cfg.con_btn[c].bs_pin[i];
            if (bpin < 0) { 
                continue; 
            }
            if (bpin == pin) { 
                this_btn_refs_pin = true; 
            }
            int cur = GPIO_IS_VALID_GPIO((gpio_num_t)bpin) ? gpio_get_level((gpio_num_t)bpin) : 0;
            if (bpin >= 0 && bpin < 32) {
                s_bs_cached_level[bpin] = (uint8_t)(cur ? 1 : 0);
            }
            if (((bool)cur) != g_anna_cfg.con_btn[c].bs_evt[i]) {
                bs_ok_all = false;
            }
        }
        if (!this_btn_refs_pin) { 
            continue; 
        }
        /* Mode condition 
           post_update에서 처리하고 있지만, 센서 변화가 모드 변경 직후 들어오거나, 워크 큐 처리 순서/레이스 시에도 안전 */
        int m = g_anna_cfg.con_btn[c].mode_idx;
        bool mode_ok = (m < 0) || (g_anna_cfg.on_mode == m);
        bool cond_ok = (mode_ok && bs_ok_all);
        ESP_LOGI(TAG, "ConBtn BS EVAL: idx=%d pin=%d cond_ok=%d mode_ok=%d bs_ok=%d is_on=%d", c, pin, cond_ok?1:0, mode_ok?1:0, bs_ok_all?1:0, g_anna_cfg.con_btn[c].is_on?1:0);
        if (g_anna_cfg.con_btn[c].is_on && !cond_ok) {
            uint16_t ep = g_anna_cfg.con_btn[c].base.endpoint_id;
            if (app_driver_con_btn_maintain_group_on_if_satisfied(ep)) {
                continue;
            }
            ESP_LOGW(TAG, "ConBtn BS -> FORCE OFF: idx=%d pin=%d", c, pin);
            app_driver_con_btn_force_off_by_index(c);
        }
    }

    /* Evaluate con_swt referencing this pin */
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        bool this_act_refs_pin = false;
        bool bs_ok_all = true;
        for (int i = 0; i < ANNA_MAX_CON_BS_COUNT; ++i) {
            int bpin = g_anna_cfg.con_swt[c].bs_pin[i];
            if (bpin < 0) { continue; }
            if (bpin == pin) { this_act_refs_pin = true; }
            int cur = GPIO_IS_VALID_GPIO((gpio_num_t)bpin) ? gpio_get_level((gpio_num_t)bpin) : 0;
            if (bpin >= 0 && bpin < 32) {
                s_bs_cached_level[bpin] = (uint8_t)(cur ? 1 : 0);
            }
            if (((bool)cur) != g_anna_cfg.con_swt[c].bs_evt[i]) {
                bs_ok_all = false;
            }
        }
        if (!this_act_refs_pin) { 
            continue; 
        }
        int m = g_anna_cfg.con_swt[c].mode_idx;
        bool mode_ok = (m < 0) || (g_anna_cfg.on_mode == m);
        bool is_on = false;
        {
            int pin_no = (int)g_anna_cfg.con_swt[c].base.pin_no;
            if (pin_no >= 0 && pin_no < 32) {
                is_on = (g_anna_cfg.on_pin & (1u << pin_no)) != 0;
            }
        }
        bool cond_ok = (mode_ok && bs_ok_all);
        ESP_LOGI(TAG, "ConSwt BS EVAL: idx=%d pin=%d cond_ok=%d mode_ok=%d bs_ok=%d is_on=%d", c, pin, cond_ok?1:0, mode_ok?1:0, bs_ok_all?1:0, is_on?1:0);
        // OFF-only: if currently ON and condition fails, force OFF for this item
        if (is_on && !cond_ok) {
            // Maintain-only: if any sibling remains satisfied and group already ON, keep group ON
            uint16_t ep = g_anna_cfg.con_swt[c].base.endpoint_id;
            if (app_driver_con_swt_maintain_group_on_if_satisfied(ep)) {
                continue;
            }
            ESP_LOGW(TAG, "ConSwt BS -> FORCE OFF: idx=%d pin=%d", c, pin);
            app_driver_con_swt_force_off_by_index(c);
        }
    }
}

static void con_btn_bs_db_timer_handler(chip::System::Layer*, void* ctx)
{
    int pin = (int)(intptr_t)ctx;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(con_btn_bs_work_handler, (intptr_t)pin);
    if (pin >= 0 && pin < 32) { s_bs_db_timer_armed[pin] = false; }
}

static void IRAM_ATTR con_btn_bs_isr(void *arg)
{
    int pin = (int)(intptr_t)arg;
    BaseType_t hpTaskWoken = pdFALSE;
    if (s_bs_isr_queue) {
        (void)xQueueSendFromISR(s_bs_isr_queue, &pin, &hpTaskWoken);
    }
    if (hpTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* Worker: arm 50ms BS debounce on CHIP thread */
static void bs_arm_debounce_work(intptr_t arg)
{
    int pin = (int)arg;
    void * ctx = (void*)(intptr_t)pin;
    chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_bs_db_timer_handler, ctx);
    chip::DeviceLayer::SystemLayer().StartTimer(
        chip::System::Clock::Milliseconds32(50),
        con_btn_bs_db_timer_handler,
        ctx);
    if (pin >= 0 && pin < 32) { s_bs_db_timer_armed[pin] = true; }
}

static void con_btn_bs_isr_worker(void * /*arg*/)
{
    int pin = -1;
    for (;;) {
        if (xQueueReceive(s_bs_isr_queue, &pin, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        chip::DeviceLayer::PlatformMgr().ScheduleWork(bs_arm_debounce_work, (intptr_t)pin);
    }
}


const char *USER_LABEL_LABEL = "endpointLabel";

/* Track endpoints created during settings_init() so we can set labels only for them */
/*
 * Intentionally fixed at 16 for current product assumptions.
 * If supported endpoint count grows, this capacity must be increased manually.
 */
static constexpr size_t kCreatedEndpointTrackCapacity = 16;
static uint16_t s_created_ep_ids[kCreatedEndpointTrackCapacity];
static size_t   s_created_ep_count = 0;
static bool     s_created_ep_overflow_warned = false;

static void record_created_ep(uint16_t ep_id)
{
    for (size_t i = 0; i < s_created_ep_count; ++i) {
        if (s_created_ep_ids[i] == ep_id) {
            return;
        }
    }
    if (s_created_ep_count < (sizeof(s_created_ep_ids) / sizeof(s_created_ep_ids[0]))) {
        s_created_ep_ids[s_created_ep_count++] = ep_id;
        return;
    }

    if (!s_created_ep_overflow_warned) {
        s_created_ep_overflow_warned = true;
        ESP_LOGW(TAG,
                 "created endpoint tracker full (capacity=%u). Increase kCreatedEndpointTrackCapacity if endpoint count grows.",
                 (unsigned)kCreatedEndpointTrackCapacity);
    }
}

static bool is_created_ep(uint16_t ep_id)
{
    for (size_t i = 0; i < s_created_ep_count; ++i) {
        if (s_created_ep_ids[i] == ep_id) {
            return true;
        }
    }
    return false;
}

static uint16_t create_on_off_endpoint(node_t *node, bool initial_on, bool add_fixed_label, const char *type_name, int index)
{
    on_off_plugin_unit::config_t plugin_unit_config = {};
    bool is_mode_endpoint = (type_name != nullptr) && (strcmp(type_name, "mode") == 0);
    if (is_mode_endpoint) {
        plugin_unit_config.on_off.lighting.start_up_on_off = nullable<uint8_t>();
    } else {
        plugin_unit_config.on_off.lighting.start_up_on_off =
                chip::to_underlying(OnOff::StartUpOnOffEnum::kOff);
    }
    if (initial_on) {
        /* mode는 최초 시작 시 0번째가 on */
        plugin_unit_config.on_off.on_off = true;
    }

    endpoint_t *ep_on_off_plugin = on_off_plugin_unit::create(node, &plugin_unit_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(ep_on_off_plugin != nullptr,
                         ESP_LOGE(TAG, "Failed to create on_off_plugin_unit for %s[%d]", type_name, index));

    user_label::config_t label_config = {};
    cluster_t *label_cluster = user_label::create(ep_on_off_plugin, &label_config, CLUSTER_FLAG_SERVER);
    ABORT_APP_ON_FAILURE(label_cluster != nullptr,
                         ESP_LOGE(TAG, "Failed to create user label cluster for %s[%d]", type_name, index));

#if CONFIG_SUPPORT_FIXED_LABEL_CLUSTER
    if (add_fixed_label) {
        fixed_label::config_t fixed_label_config = {};
        cluster_t *fixed_label_cluster = fixed_label::create(ep_on_off_plugin, &fixed_label_config, CLUSTER_FLAG_SERVER);
        ABORT_APP_ON_FAILURE(fixed_label_cluster != nullptr,
                             ESP_LOGE(TAG, "Failed to create fixed label cluster for %s[%d]", type_name, index));
    }
#else
    (void)add_fixed_label;
#endif

    uint16_t ep_id = endpoint::get_id(ep_on_off_plugin);
    record_created_ep(ep_id);
    return ep_id;
}

typedef struct {
    const char *type_name;
    int index;
    uint16_t endpoint_id;
} endpoint_ref_t;

static void log_endpoint_id_collisions(void)
{
    endpoint_ref_t refs[ANNA_MAX_MODE_COUNT + ANNA_MAX_BUTTON + ANNA_MAX_SWITCH + ANNA_MAX_CON_ACT + ANNA_MAX_CON_SWT_ACT] = {};
    size_t ref_count = 0;
    int mode_count = (int)g_anna_cfg.modes.mode_count;

    for (int i = 0; i < mode_count && i < ANNA_MAX_MODE_COUNT; ++i) {
        refs[ref_count].type_name = "mode";
        refs[ref_count].index = i;
        refs[ref_count].endpoint_id = g_anna_cfg.modes.endpoint_id[i];
        ++ref_count;
    }
    for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
        refs[ref_count].type_name = "button";
        refs[ref_count].index = i;
        refs[ref_count].endpoint_id = g_anna_cfg.a_button[i].base.endpoint_id;
        ++ref_count;
    }
    for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
        refs[ref_count].type_name = "switch";
        refs[ref_count].index = i;
        refs[ref_count].endpoint_id = g_anna_cfg.a_switch[i].base.endpoint_id;
        ++ref_count;
    }
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        refs[ref_count].type_name = "con_btn";
        refs[ref_count].index = i;
        refs[ref_count].endpoint_id = g_anna_cfg.con_btn[i].base.endpoint_id;
        ++ref_count;
    }
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        refs[ref_count].type_name = "con_swt";
        refs[ref_count].index = i;
        refs[ref_count].endpoint_id = g_anna_cfg.con_swt[i].base.endpoint_id;
        ++ref_count;
    }

    bool has_collision = false;
    for (size_t i = 0; i < ref_count; ++i) {
        if (refs[i].endpoint_id == ENDPOINT_ID_INVALID) {
            continue;
        }
        for (size_t j = i + 1; j < ref_count; ++j) {
            if (refs[i].endpoint_id != refs[j].endpoint_id) {
                continue;
            }
            has_collision = true;
            ESP_LOGE(TAG,
                     "Endpoint ID collision: ep=%u %s[%d] <-> %s[%d] (first-wins fallback may expose wrong fixed label)",
                     (unsigned)refs[i].endpoint_id, refs[i].type_name, refs[i].index, refs[j].type_name, refs[j].index);
        }
    }

    if (has_collision) {
        ESP_LOGE(TAG, "Endpoint ID uniqueness check failed; runtime continues with existing fallback behavior");
    } else {
        ESP_LOGI(TAG, "Endpoint ID uniqueness check passed (%u entries)", (unsigned)ref_count);
    }
}

chip::ChipError set_user_label(chip::EndpointId ep, const char * label)
{
    ESP_LOGI(TAG, "set_user_label(%s): Start", label);
    chip::ChipError err = CHIP_NO_ERROR;

    auto * prov = GetDeviceInfoProvider();        // esp_matter::start() 이후 유효
    if (!prov) {
        ESP_LOGW(TAG, "Provider null");
        err = CHIP_ERROR_INTERNAL;
        return err;
    }

    UserLabelType s;
    s.label = CharSpan::fromCharString(USER_LABEL_LABEL);      // 언어코드(선택)
    s.value = CharSpan::fromCharString(label);      // 실제 표시명

    
    AttributeList<UserLabelType, chip::DeviceLayer::kMaxUserLabelListLength> list;
    err = list.add(s);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Success to add user label: %s", label);
    } else {
        ESP_LOGE(TAG, "Err: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }

    err = prov->SetUserLabelList(ep, list);              // NVS에 저장
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "set_user_label Success. Label: %s", label); 
    } else {
        ESP_LOGE(TAG, "set_user_label Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, label, err.Format()); 
    }
    return err;
}

chip::ChipError set_all_user_label(void)
{
    ESP_LOGI(TAG, "set_all_user_label: Start");
    chip::ChipError err = CHIP_NO_ERROR;
    int number_ep = 0;

    /* button의 user label 저장 */
    for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
        if (!is_created_ep(g_anna_cfg.a_button[i].base.endpoint_id)) {
            continue; 
        }
        ++number_ep;
        err = set_user_label(g_anna_cfg.a_button[i].base.endpoint_id, g_anna_cfg.a_button[i].label);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "set_user_label button Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, g_anna_cfg.a_button[i].label, err.Format()); 
            return err;
        }
    }

    for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
        if (!is_created_ep(g_anna_cfg.a_switch[i].base.endpoint_id)) {
            continue; 
        }
        ++number_ep;
        err = set_user_label(g_anna_cfg.a_switch[i].base.endpoint_id, g_anna_cfg.a_switch[i].label);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "set_user_label switch Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, g_anna_cfg.a_switch[i].label, err.Format()); 
            return err;
        }
    }

    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        if (!is_created_ep(g_anna_cfg.con_btn[i].base.endpoint_id)) {
            continue; 
        }
        ++number_ep;
        err = set_user_label(g_anna_cfg.con_btn[i].base.endpoint_id, g_anna_cfg.con_btn[i].label);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "set_user_label con_btn Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, g_anna_cfg.con_btn[i].label, err.Format()); 
            return err;
        }
    }

    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        if (!is_created_ep(g_anna_cfg.con_swt[i].base.endpoint_id)) {
            continue; 
        }
        ++number_ep;
        err = set_user_label(g_anna_cfg.con_swt[i].base.endpoint_id, g_anna_cfg.con_swt[i].label);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "set_user_label con_swt Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, g_anna_cfg.con_swt[i].label, err.Format()); 
            return err;
        }
    }

    /* mode의 user label 저장 */
    for (int i = 0; i < g_anna_cfg.modes.mode_count; i++) {
        err = set_user_label(g_anna_cfg.modes.endpoint_id[i], g_anna_cfg.modes.labels[i]);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "set_user_label mode Failed. Label: %s, err: %" CHIP_ERROR_FORMAT, g_anna_cfg.modes.labels[i], err.Format()); 
            return err;
        }
    }

    ESP_LOGI(TAG, "set_all_user_label End");
    return err;
}

/* 부팅 직후: NVS에 저장된 모드 상태에 맞춰 LED 동기화
 * - 정확히 하나의 모드만 ON이면 해당 인덱스 색으로 LED 설정
 * - 모두 OFF면 LED 끔
 * - 둘 이상 ON이면 적용 루프를 큐잉하여 불변식 강제(LED는 적용 경로에서 색 반영)
 */
static void sync_led_with_persisted_mode()
{
    if (!g_led_handle) {
        return;
    }

    int onCount = 0;
    int lastOnIdx = -1;
    for (int i = 0; i < g_anna_cfg.modes.mode_count; i++) {
        bool s = false;
        (void) chip::app::Clusters::OnOff::Attributes::OnOff::Get(g_anna_cfg.modes.endpoint_id[i], &s);
        if (s) { 
            onCount++;
            lastOnIdx = i; 
        }
    }

    if (onCount == 1) {
        g_anna_cfg.on_mode = lastOnIdx;
        (void) led_controller_set_color_idx(g_led_handle, (uint8_t)lastOnIdx);
        ESP_LOGI(TAG, "LED synced to persisted single-on mode: %d", lastOnIdx);
    } else {
        (void) led_controller_turn_off(g_led_handle);
        ESP_LOGI(TAG, "No mode ON at boot; LED turned off");
    }
}

void settings_init(node_t *node) {
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "settings_init start");

    int number_ep = 0;
    s_created_ep_count = 0;
    s_created_ep_overflow_warned = false;

    /* Initialize modes settings first. mode_count is signed and can be -1(null). */
    int mode_count = (int)g_anna_cfg.modes.mode_count;
    for (int i = 0; i < mode_count; ++i) {
        bool initial_on = (i == 0);
        g_anna_cfg.modes.endpoint_id[i] = create_on_off_endpoint(node, initial_on, false, "mode", i);
        ++number_ep;
    }

    for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
        g_anna_cfg.a_button[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "button", i);
        ++number_ep;
    }
    for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
        g_anna_cfg.a_switch[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "switch", i);
        ++number_ep;
    }
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        g_anna_cfg.con_btn[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "con_btn", i);
        ++number_ep;
    }
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        g_anna_cfg.con_swt[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "con_swt", i);
        ++number_ep;
    }

    log_endpoint_id_collisions();

    err = app_driver_led_init();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize plug driver"));
    
    /* Initialize button GPIOs once */
    for (int b = 0; b < g_anna_cfg.button_cnt; ++b) {
        int pin_no = (int)g_anna_cfg.a_button[b].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            gpio_set_level(gpio, 0);
            app_driver_mark_button_gpio_inited(b);
            ESP_LOGI(TAG, "Button GPIO init: idx=%d pin=%d (LOW)", b, pin_no);
        } else {
            ESP_LOGW(TAG, "Button GPIO invalid at init: idx=%d pin=%d", b, pin_no);
        }
    }

    /* Initialize switch GPIOs once */
    for (int s = 0; s < g_anna_cfg.switch_cnt; ++s) {
        int pin_no = (int)g_anna_cfg.a_switch[s].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            // default LOW
            gpio_set_level(gpio, 0);
            app_driver_mark_switch_gpio_inited(s);
            ESP_LOGI(TAG, "Switch GPIO init: idx=%d pin=%d (LOW)", s, pin_no);
        } else {
            ESP_LOGW(TAG, "Switch GPIO invalid at init: idx=%d pin=%d", s, pin_no);
        }
    }

    /* Initialize con_btn GPIOs once (output actuators) */
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        int pin_no = (int)g_anna_cfg.con_btn[c].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            gpio_set_level(gpio, 0);
            app_driver_mark_con_btn_gpio_inited(c);
            ESP_LOGI(TAG, "ConBtn GPIO init: idx=%d pin=%d (LOW)", c, pin_no);
        } else {
            ESP_LOGW(TAG, "ConBtn GPIO invalid at init: idx=%d pin=%d", c, pin_no);
        }
    }

    /* Initialize con_swt GPIOs once (latched actuators) */
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        int pin_no = (int)g_anna_cfg.con_swt[c].base.pin_no;
        gpio_num_t gpio = static_cast<gpio_num_t>(pin_no);
        if (GPIO_IS_VALID_GPIO(gpio)) {
            gpio_reset_pin(gpio);
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
            gpio_set_level(gpio, 0);
            app_driver_mark_con_swt_gpio_inited(c);
            ESP_LOGI(TAG, "ConSwt GPIO init: idx=%d pin=%d (LOW)", c, pin_no);
        } else {
            ESP_LOGW(TAG, "ConSwt GPIO invalid at init: idx=%d pin=%d", c, pin_no);
        }
    }

    /* Initialize boolean sensor GPIOs (IRQ-based, event-driven) */
    if (!s_bs_isr_queue) {
        s_bs_isr_queue = xQueueCreate(16, sizeof(int));
        if (!s_bs_isr_queue) {
            ESP_LOGE(TAG, "Failed to create BS ISR queue");
        }
    }
    if (s_bs_isr_queue && !s_bs_isr_task) {
        BaseType_t ok = xTaskCreate(con_btn_bs_isr_worker, "bs_isr_w", 3072, nullptr, 5, &s_bs_isr_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create BS ISR worker task");
            s_bs_isr_task = nullptr;
        }
    }
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        for (int i = 0; i < ANNA_MAX_CON_BS_COUNT; ++i) {
            int bpin = g_anna_cfg.con_btn[c].bs_pin[i];
            if (bpin < 0 || bpin >= 32) { 
                continue; 
            }
            if (s_bs_pin_inited[bpin]) { 
                continue; 
            }
            gpio_num_t g = (gpio_num_t)bpin;
            if (!GPIO_IS_VALID_GPIO(g)) { 
                continue; 
            }
            gpio_reset_pin(g);
            gpio_set_direction(g, GPIO_MODE_INPUT);
            gpio_set_intr_type(g, GPIO_INTR_ANYEDGE);
            if (!s_bs_isr_installed) {
                gpio_install_isr_service(0);
                s_bs_isr_installed = true;
            }
            gpio_isr_handler_add(g, con_btn_bs_isr, (void*)(intptr_t)bpin);
            s_bs_pin_inited[bpin] = true;
            s_bs_cached_level[bpin] = (uint8_t)gpio_get_level(g);
            ESP_LOGI(TAG, "ConBtn BS GPIO init, con_btn[%d].bs_pin[%d]: pin=%d (IRQ ANYEDGE)", c, i, bpin);
        }
    }

    /* Also initialize BS pins referenced by con_swt */
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        for (int i = 0; i < ANNA_MAX_CON_BS_COUNT; ++i) {
            int bpin = g_anna_cfg.con_swt[c].bs_pin[i];
            if (bpin < 0 || bpin >= 32) { 
                continue; 
            }
            if (s_bs_pin_inited[bpin]) { 
                continue; 
            }
            gpio_num_t g = (gpio_num_t)bpin;
            if (!GPIO_IS_VALID_GPIO(g)) { 
                continue; 
            }
            gpio_reset_pin(g);
            gpio_set_direction(g, GPIO_MODE_INPUT);
            gpio_set_intr_type(g, GPIO_INTR_ANYEDGE);
            if (!s_bs_isr_installed) {
                gpio_install_isr_service(0);
                s_bs_isr_installed = true;
            }
            gpio_isr_handler_add(g, con_btn_bs_isr, (void*)(intptr_t)bpin);
            s_bs_pin_inited[bpin] = true;
            s_bs_cached_level[bpin] = (uint8_t)gpio_get_level(g);
            ESP_LOGI(TAG, "ConSwt BS GPIO init, con_swt[%d].bs_pin[%d]: pin=%d (IRQ ANYEDGE)", c, i, bpin);
        }
    }

    /* Initialize analog sensor ADC channels for con_btn (as_pin) */
    for (int c = 0; c < g_anna_cfg.con_btn_cnt; ++c) {
        for (int i = 0; i < ANNA_MAX_CON_AS_COUNT; ++i) {
            int apin = g_anna_cfg.con_btn[c].as_pin[i];
            if (apin < 0) {
                continue;
            }
            adc_channel_t ch;
            if (ensure_adc1_channel_for_gpio(apin, &ch)) {
                ESP_LOGI(TAG, "ConBtn AS ADC init, con_btn[%d].as_pin[%d]: gpio=%d ch=%d", c, i, apin, (int)ch);
            } else {
                ESP_LOGW(TAG, "ConBtn AS ADC init failed, con_btn[%d].as_pin[%d]: gpio=%d", c, i, apin);
            }
        }
    }

    /* Initialize analog sensor ADC channels for con_swt (as_pin) */
    for (int c = 0; c < g_anna_cfg.con_swt_cnt; ++c) {
        for (int i = 0; i < ANNA_MAX_CON_AS_COUNT; ++i) {
            int apin = g_anna_cfg.con_swt[c].as_pin[i];
            if (apin < 0) {
                continue;
            }
            adc_channel_t ch;
            if (ensure_adc1_channel_for_gpio(apin, &ch)) {
                ESP_LOGI(TAG, "ConSwt AS ADC init, con_swt[%d].as_pin[%d]: gpio=%d ch=%d", c, i, apin, (int)ch);
            } else {
                ESP_LOGW(TAG, "ConSwt AS ADC init failed, con_swt[%d].as_pin[%d]: gpio=%d", c, i, apin);
            }
        }
    }

    ESP_LOGI(TAG, "settings_init end");
}

void settings_post_esp_start_init(void) {
    ESP_LOGI(TAG, "settings_post_esp_start_init start");

    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t){
        app_driver_boot_safe_off_sync_non_mode();
        set_all_user_label();
        sync_led_with_persisted_mode();
        ESP_LOGI(TAG, "settings_post_esp_start_init End");
    }, 0);
}
