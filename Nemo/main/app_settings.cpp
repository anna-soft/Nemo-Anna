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
static node_t *s_runtime_node = nullptr;
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
        bool hold_ok = bs_ok_all;
        ESP_LOGI(TAG, "ConBtn BS EVAL: idx=%d pin=%d hold_ok=%d bs_ok=%d is_on=%d", c, pin, hold_ok ? 1 : 0, bs_ok_all ? 1 : 0, g_anna_cfg.con_btn[c].is_on ? 1 : 0);
        if (g_anna_cfg.con_btn[c].is_on && !hold_ok) {
            uint16_t ep = g_anna_cfg.con_btn[c].base.endpoint_id;
            if (app_driver_con_btn_maintain_group_on_if_satisfied(ep)) {
                continue;
            }
            ESP_LOGW(TAG, "ConBtn BS -> FORCE OFF: idx=%d pin=%d", c, pin);
            app_driver_con_btn_force_off_by_index(c);
        }
    }

    /* Evaluate con_swt referencing this pin */
    uint16_t con_swt_ep_list[ANNA_MAX_CON_SWT_ACT] = {};
    int con_swt_ep_count = 0;
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
        bool cond_ok = (mode_ok && bs_ok_all);
        uint16_t ep = g_anna_cfg.con_swt[c].base.endpoint_id;
        ESP_LOGI(TAG, "ConSwt BS EVAL: idx=%d pin=%d cond_ok=%d mode_ok=%d bs_ok=%d ep=%u",
                 c,
                 pin,
                 cond_ok ? 1 : 0,
                 mode_ok ? 1 : 0,
                 bs_ok_all ? 1 : 0,
                 (unsigned)ep);
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
    for (int i = 0; i < con_swt_ep_count; ++i) {
        if (con_swt_ep_list[i] == ENDPOINT_ID_INVALID) {
            continue;
        }
        app_driver_con_swt_group_reconcile_by_endpoint(con_swt_ep_list[i]);
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

static uint16_t resolve_reuse_endpoint_id(const anna_endpoint_reuse_plan_t *reuse_plan, const char *type_name, int index)
{
    if (!reuse_plan || !type_name || index < 0) {
        return ENDPOINT_ID_INVALID;
    }

    if (strcmp(type_name, "mode") == 0 && index < ANNA_MAX_MODE_COUNT) {
        return reuse_plan->mode[index];
    }
    if (strcmp(type_name, "button") == 0 && index < ANNA_MAX_BUTTON) {
        return reuse_plan->button[index];
    }
    if (strcmp(type_name, "switch") == 0 && index < ANNA_MAX_SWITCH) {
        return reuse_plan->swt[index];
    }
    if (strcmp(type_name, "con_btn") == 0 && index < ANNA_MAX_CON_ACT) {
        return reuse_plan->con_btn[index];
    }
    if (strcmp(type_name, "con_swt") == 0 && index < ANNA_MAX_CON_SWT_ACT) {
        return reuse_plan->con_swt[index];
    }
    return ENDPOINT_ID_INVALID;
}

static endpoint_t *resume_on_off_endpoint(node_t *node, on_off_plugin_unit::config_t *plugin_unit_config, bool add_fixed_label,
                                          uint16_t endpoint_id, const char *type_name, int index)
{
    endpoint_t *ep_on_off_plugin = endpoint::resume(node, ENDPOINT_FLAG_DESTROYABLE, endpoint_id, NULL);
    ABORT_APP_ON_FAILURE(ep_on_off_plugin != nullptr,
                         ESP_LOGE(TAG, "Failed to resume on_off_plugin_unit for %s[%d] ep=%u", type_name, index,
                                  (unsigned)endpoint_id));

    cluster_t *descriptor_cluster = descriptor::create(ep_on_off_plugin, &(plugin_unit_config->descriptor), CLUSTER_FLAG_SERVER);
    ABORT_APP_ON_FAILURE(descriptor_cluster != nullptr,
                         ESP_LOGE(TAG, "Failed to create descriptor cluster for %s[%d] ep=%u", type_name, index,
                                  (unsigned)endpoint_id));

    esp_err_t add_err = on_off_plugin_unit::add(ep_on_off_plugin, plugin_unit_config);
    ABORT_APP_ON_FAILURE(add_err == ESP_OK,
                         ESP_LOGE(TAG, "Failed to add on_off_plugin_unit for %s[%d] ep=%u err=%d", type_name, index,
                                  (unsigned)endpoint_id, (int)add_err));

    user_label::config_t label_config = {};
    cluster_t *label_cluster = user_label::create(ep_on_off_plugin, &label_config, CLUSTER_FLAG_SERVER);
    ABORT_APP_ON_FAILURE(label_cluster != nullptr,
                         ESP_LOGE(TAG, "Failed to create user label cluster for %s[%d] ep=%u", type_name, index,
                                  (unsigned)endpoint_id));

#if CONFIG_SUPPORT_FIXED_LABEL_CLUSTER
    if (add_fixed_label) {
        fixed_label::config_t fixed_label_config = {};
        cluster_t *fixed_label_cluster = fixed_label::create(ep_on_off_plugin, &fixed_label_config, CLUSTER_FLAG_SERVER);
        ABORT_APP_ON_FAILURE(fixed_label_cluster != nullptr,
                             ESP_LOGE(TAG, "Failed to create fixed label cluster for %s[%d] ep=%u", type_name, index,
                                      (unsigned)endpoint_id));
    }
#else
    (void)add_fixed_label;
#endif

    return ep_on_off_plugin;
}

static uint16_t create_on_off_endpoint_with_reuse(node_t *node, bool initial_on, bool add_fixed_label, const char *type_name,
                                                  int index, const anna_endpoint_reuse_plan_t *reuse_plan)
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

    uint16_t reuse_endpoint_id = resolve_reuse_endpoint_id(reuse_plan, type_name, index);
    endpoint_t *ep_on_off_plugin = nullptr;
    if (reuse_endpoint_id != ENDPOINT_ID_INVALID) {
        ep_on_off_plugin =
                resume_on_off_endpoint(node, &plugin_unit_config, add_fixed_label, reuse_endpoint_id, type_name, index);
    } else {
        ep_on_off_plugin = on_off_plugin_unit::create(node, &plugin_unit_config, ENDPOINT_FLAG_DESTROYABLE, NULL);
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
    }

    uint16_t ep_id = endpoint::get_id(ep_on_off_plugin);
    record_created_ep(ep_id);
    return ep_id;
}

static uint16_t create_on_off_endpoint(node_t *node, bool initial_on, bool add_fixed_label, const char *type_name, int index)
{
    return create_on_off_endpoint_with_reuse(node, initial_on, add_fixed_label, type_name, index, nullptr);
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

/* 부팅 직후: persisted mode를 boot helper로 점검/정규화
 * - 정확히 하나만 ON이면 해당 index를 유지하고 LED가 있으면 동기화
 * - 모두 OFF 또는 둘 이상 ON이면 helper가 index 0 only로 정규화
 * - LED가 없어도 mode invariant 정규화는 계속 수행
 */
static void sync_led_with_persisted_mode()
{
    app_driver_boot_reconcile_mode_only();
}

static void init_output_gpios_from_cfg()
{
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
}

static void init_bs_inputs_from_cfg()
{
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
            gpio_isr_handler_add(g, con_btn_bs_isr, (void *)(intptr_t)bpin);
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
            gpio_isr_handler_add(g, con_btn_bs_isr, (void *)(intptr_t)bpin);
            s_bs_pin_inited[bpin] = true;
            s_bs_cached_level[bpin] = (uint8_t)gpio_get_level(g);
            ESP_LOGI(TAG, "ConSwt BS GPIO init, con_swt[%d].bs_pin[%d]: pin=%d (IRQ ANYEDGE)", c, i, bpin);
        }
    }
}

static void init_adc_channels_from_cfg()
{
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
}

static void finalize_dynamic_endpoint_build()
{
    log_endpoint_id_collisions();
    init_output_gpios_from_cfg();
    init_bs_inputs_from_cfg();
    init_adc_channels_from_cfg();
}

static esp_err_t enable_runtime_endpoint_if_started(node_t *node, uint16_t endpoint_id, const char *type_name, int index)
{
    if (!is_started() || !node || endpoint_id == ENDPOINT_ID_INVALID || endpoint_id == 0) {
        return ESP_OK;
    }

    endpoint_t *endpoint = endpoint::get(node, endpoint_id);
    if (!endpoint) {
        ESP_LOGE(TAG, "runtime endpoint publish lookup failed for %s[%d] ep=%u", type_name, index, (unsigned)endpoint_id);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = endpoint::enable(endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime endpoint publish failed for %s[%d] ep=%u err=%s", type_name, index, (unsigned)endpoint_id,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "runtime endpoint published: %s[%d] ep=%u", type_name, index, (unsigned)endpoint_id);
    return ESP_OK;
}

static esp_err_t rebuild_dynamic_endpoints_from_current_cfg(node_t *node, const anna_endpoint_reuse_plan_t *reuse_plan)
{
    if (!node) {
        return ESP_ERR_INVALID_ARG;
    }

    s_created_ep_count = 0;
    s_created_ep_overflow_warned = false;

    int mode_count = (int)g_anna_cfg.modes.mode_count;
    for (int i = 0; i < mode_count; ++i) {
        bool initial_on = (i == 0);
        g_anna_cfg.modes.endpoint_id[i] = create_on_off_endpoint_with_reuse(node, initial_on, false, "mode", i, reuse_plan);
        esp_err_t err = enable_runtime_endpoint_if_started(node, g_anna_cfg.modes.endpoint_id[i], "mode", i);
        if (err != ESP_OK) {
            return err;
        }
    }

    for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
        g_anna_cfg.a_button[i].base.endpoint_id = create_on_off_endpoint_with_reuse(node, false, true, "button", i, reuse_plan);
        esp_err_t err = enable_runtime_endpoint_if_started(node, g_anna_cfg.a_button[i].base.endpoint_id, "button", i);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
        g_anna_cfg.a_switch[i].base.endpoint_id = create_on_off_endpoint_with_reuse(node, false, true, "switch", i, reuse_plan);
        esp_err_t err = enable_runtime_endpoint_if_started(node, g_anna_cfg.a_switch[i].base.endpoint_id, "switch", i);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        g_anna_cfg.con_btn[i].base.endpoint_id = create_on_off_endpoint_with_reuse(node, false, true, "con_btn", i, reuse_plan);
        esp_err_t err = enable_runtime_endpoint_if_started(node, g_anna_cfg.con_btn[i].base.endpoint_id, "con_btn", i);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        g_anna_cfg.con_swt[i].base.endpoint_id = create_on_off_endpoint_with_reuse(node, false, true, "con_swt", i, reuse_plan);
        esp_err_t err = enable_runtime_endpoint_if_started(node, g_anna_cfg.con_swt[i].base.endpoint_id, "con_swt", i);
        if (err != ESP_OK) {
            return err;
        }
    }

    finalize_dynamic_endpoint_build();
    return ESP_OK;
}

esp_matter::node_t *app_settings_get_runtime_node(void)
{
    return s_runtime_node;
}

void app_settings_clear_runtime_state(void)
{
    for (int pin = 0; pin < 32; ++pin) {
        void *ctx = (void *)(intptr_t)pin;
        chip::DeviceLayer::SystemLayer().CancelTimer(con_btn_bs_db_timer_handler, ctx);
        s_bs_db_timer_armed[pin] = false;
        s_bs_cached_level[pin] = 0;
        if (s_bs_pin_inited[pin]) {
            gpio_num_t gpio = static_cast<gpio_num_t>(pin);
            if (GPIO_IS_VALID_GPIO(gpio)) {
                gpio_isr_handler_remove(gpio);
            }
            s_bs_pin_inited[pin] = false;
        }
    }

    s_created_ep_count = 0;
    s_created_ep_overflow_warned = false;
}

esp_err_t app_settings_rebuild_from_current_cfg(const anna_endpoint_reuse_plan_t *reuse_plan)
{
    return rebuild_dynamic_endpoints_from_current_cfg(s_runtime_node, reuse_plan);
}

void settings_init(node_t *node) {
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "settings_init start");

    s_runtime_node = node;

    s_created_ep_count = 0;
    s_created_ep_overflow_warned = false;

    int mode_count = (int)g_anna_cfg.modes.mode_count;
    for (int i = 0; i < mode_count; ++i) {
        bool initial_on = (i == 0);
        g_anna_cfg.modes.endpoint_id[i] = create_on_off_endpoint(node, initial_on, false, "mode", i);
    }

    for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
        g_anna_cfg.a_button[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "button", i);
    }
    for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
        g_anna_cfg.a_switch[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "switch", i);
    }
    for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
        g_anna_cfg.con_btn[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "con_btn", i);
    }
    for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
        g_anna_cfg.con_swt[i].base.endpoint_id = create_on_off_endpoint(node, false, true, "con_swt", i);
    }

    finalize_dynamic_endpoint_build();

    err = app_driver_led_init();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize plug driver"));

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
