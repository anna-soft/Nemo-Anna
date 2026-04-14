/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <inttypes.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <esp_matter.h>
#include "mode_manager.h"
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <esp_matter_providers.h>

#include <common_macros.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include <nvs.h>

#include "app_priv.h"
#include "anna_cfg.h"
#include "anna_factory_reset.h"
#include "anna_state_storage.h"
#include "host_serial_rx.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>
#include <esp_system.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>

#include "anna_commissionable_data_provider.h"
#include "anna_device_instance_info_provider.h"
#if CONFIG_CUSTOM_DEVICE_INFO_PROVIDER
#include "anna_device_info_provider.h"
#endif

#define ANNA_RT_PARTITION_NAME "runtime_anna"
#define ANNA_CFG_NAMESPACE     "anna_cfg"
#define ANNA_KEY_PIN           "pin_code"
#define ANNA_KEY_LONG_DIS      "long_dis"
#define ANNA_KEY_MAC           "mac_addr"
// CommissionableDataProvider용 Spake2p 데이터(프로비저닝 단계에서 생성/저장)
#define ANNA_KEY_SPAKE_ITER     "spake_iter"
#define ANNA_KEY_SPAKE_SALT     "spake_salt"
#define ANNA_KEY_SPAKE_VERIFIER "spake_verifier"

static constexpr uint32_t kAnnaSpake2pIterationCount = 1000; // user requirement (min allowed by CHIP)
static constexpr size_t kAnnaSpake2pSaltLen = 16;            // min allowed (16~32)
/* PIN은 8자리(00000000~99999999)로 UART/로그에 출력된다.
 * 너무 흔하거나 추측하기 쉬운 값들은 생성/저장 단계에서 제외한다.
 */
static inline bool is_forbidden_pin_code(uint32_t pin)
{
    switch (pin) {
    case 0U:         /* "00000000" */
    case 11111111U:
    case 22222222U:
    case 33333333U:
    case 44444444U:
    case 55555555U:
    case 66666666U:
    case 77777777U:
    case 88888888U:
    case 99999999U:
    case 12345678U:
    case 87654321U:
        return true;
    default:
        return false;
    }
}

static uint32_t generate_pin_code_8digits_excluding_forbidden(void)
{
    constexpr uint32_t kPinMod = 100000000U; /* 0 ~ 99,999,999 */

    /* 거의 항상 1회에 성공하지만, 방어적으로 제한된 횟수만 재시도한다. */
    for (int i = 0; i < 32; ++i) {
        uint32_t candidate = esp_random() % kPinMod;
        if (!is_forbidden_pin_code(candidate)) {
            return candidate;
        }
    }

    /* 마지막 폴백: 금지 목록은 12개뿐이므로 +1로 몇 번만 이동해도 빠져나온다. */
    uint32_t candidate = esp_random() % kPinMod;
    for (int i = 0; i < 64; ++i) {
        if (!is_forbidden_pin_code(candidate)) {
            return candidate;
        }
        candidate = (candidate + 1U) % kPinMod;
    }

    /* 논리적으로 도달하기 어렵지만, 최후의 안전장치 */
    return 13579246U;
}

typedef struct {
    uint32_t pin_code;
    uint16_t long_dis;
    char     mac_addr[18]; /* "AA:BB:CC:DD:EE:FF" + '\0' */
} board_identity_t;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";


// Matter 시작 전, custom CommissionableDataProvider를 등록하기 위한 인스턴스
static AnnaCommissionableDataProvider s_anna_commissionable_data_provider;

// Matter 시작 전, custom DeviceInstanceInfoProvider를 등록하기 위한 인스턴스
static AnnaDeviceInstanceInfoProvider s_anna_device_instance_info_provider;

// Matter 시작 전, custom DeviceInfoProvider(UserLabel 등)를 등록하기 위한 인스턴스
#if CONFIG_CUSTOM_DEVICE_INFO_PROVIDER
static AnnaDeviceInfoProvider s_anna_device_info_provider;
#endif

constexpr auto k_timeout_seconds = 300;
static constexpr uint8_t kPowerCycleResetThreshold = 5;
static constexpr uint32_t kPowerCycleClearWindowMs = 10000;
static TimerHandle_t s_power_cycle_clear_timer = nullptr;
static bool s_power_cycle_clear_timer_armed = false;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#ifdef CONFIG_ENABLE_MEMORY_PROFILING
static void memory_profiler_dump_heap_stat(const char *state)
{
    ESP_LOGI(TAG,"========== HEAP-DUMP-START ==========\n");
    ESP_LOGI(TAG,"state: %s\n", state);
    ESP_LOGI(TAG,"\tDescription\tInternal\n");
    ESP_LOGI(TAG,"Current Free Memory\t%d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG,"Largest Free Block\t%d\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG,"Min. Ever Free Size\t%d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG,"========== HEAP-DUMP-END ==========\n");
}

#endif

/* MAC 읽기: Wi-Fi 드라이버 초기화 없이도 동작하는 esp_read_mac(ESP_MAC_WIFI_STA) 우선.
 * 실패 시 efuse 기본 MAC을 폴백으로 사용한다. esp_wifi_get_mac()은 Wi-Fi 초기화 의존성이 있어 여기선 사용하지 않는다.
 */
static esp_err_t read_mac_string(char out[18])
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        err = esp_efuse_mac_get_default(mac);
    }
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* 프로비저닝 단계 전용:
 * - 현장 플로우에서 항상 erase-flash() 후 provisioning을 수행하므로, "기존 NVS 값이 있다"는 전제를 두지 않는다.
 * - 따라서 이 함수는 "무조건 새로 생성 + NVS에 저장"만 수행한다.
 *
 * 주의:
 * - 프로비저닝 중 서버가 board_info를 재요청할 수 있으므로(동일 부팅에서),
 *   동일 부팅 내에서는 값이 바뀌지 않도록 1회 생성 캐시를 사용한다.
 * - NVS를 읽어서 존재 여부를 판단하지 않는다(요구사항).
 */
static esp_err_t ensure_board_identity(board_identity_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    // 동일 부팅 내 재요청 대응: 1회만 생성하고, 이후에는 캐시를 반환한다.
    static bool s_generated = false;
    static board_identity_t s_cached = {0};

    if (s_generated) {
        *out = s_cached;
        return ESP_OK;
    }

    // 1) 무조건 새로 생성
    board_identity_t tmp = {0};
    tmp.pin_code = generate_pin_code_8digits_excluding_forbidden();
    tmp.long_dis = (uint16_t)(esp_random() & 0x0FFF);
    esp_err_t err = read_mac_string(tmp.mac_addr);
    if (err != ESP_OK) {
        return err;
    }

    // 2) spake2p 파라미터 생성 (iter=1000, salt=16B, verifier=Generate+Serialize)
    const uint32_t spake_iter = kAnnaSpake2pIterationCount;

    uint8_t spake_salt[kAnnaSpake2pSaltLen] = {0};
    esp_fill_random(spake_salt, sizeof(spake_salt));

    chip::Crypto::Spake2pVerifierSerialized spake_verifier = {0};
    {
        chip::Crypto::Spake2pVerifier verifier_obj;
        chip::ByteSpan salt_span(spake_salt, sizeof(spake_salt));
        CHIP_ERROR cerr = verifier_obj.Generate(spake_iter, salt_span, tmp.pin_code);
        if (cerr != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "board_info: Spake2pVerifier::Generate failed: %" CHIP_ERROR_FORMAT, cerr.Format());
            return ESP_FAIL;
        }

        chip::MutableByteSpan out_span(spake_verifier, sizeof(spake_verifier));
        cerr = verifier_obj.Serialize(out_span);
        if (cerr != CHIP_NO_ERROR || out_span.size() != chip::Crypto::kSpake2p_VerifierSerialized_Length) {
            ESP_LOGW(TAG, "board_info: Spake2pVerifier::Serialize failed: %" CHIP_ERROR_FORMAT, cerr.Format());
            return ESP_FAIL;
        }
    }

    // 3) NVS에 저장(항상 set + commit)
    nvs_handle_t h;
    err = nvs_open_from_partition(ANNA_RT_PARTITION_NAME, ANNA_CFG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "board_info: provisioning generate+store (pin=%08u, dis=%u, mac=%s, iter=%" PRIu32 ")",
             (unsigned)tmp.pin_code, (unsigned)tmp.long_dis, tmp.mac_addr, (uint32_t)spake_iter);

    esp_err_t werr = ESP_OK;
    werr = nvs_set_u32(h, ANNA_KEY_PIN, tmp.pin_code);
    if (werr == ESP_OK) werr = nvs_set_u16(h, ANNA_KEY_LONG_DIS, tmp.long_dis);
    if (werr == ESP_OK) werr = nvs_set_str(h, ANNA_KEY_MAC, tmp.mac_addr);

    if (werr == ESP_OK) werr = nvs_set_u32(h, ANNA_KEY_SPAKE_ITER, spake_iter);
    if (werr == ESP_OK) werr = nvs_set_blob(h, ANNA_KEY_SPAKE_SALT, spake_salt, sizeof(spake_salt));
    if (werr == ESP_OK) werr = nvs_set_blob(h, ANNA_KEY_SPAKE_VERIFIER, spake_verifier, sizeof(spake_verifier));

    if (werr == ESP_OK) werr = nvs_commit(h);
    nvs_close(h);
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "board_info: failed to store provisioning data: %s", esp_err_to_name(werr));
        return werr;
    }

    // 4) 캐시/반환
    s_cached = tmp;
    s_generated = true;
    *out = s_cached;
    return ESP_OK;
}

/**
 * 플래시 후 호스트(웹)가 보드 정보를 자동 수집할 수 있도록
 * PIN 코드 / Long Discriminator / MAC을 JSON 한 줄로 UART에 출력한다.
 * 웹 측 `boardProtocol.ts`는 줄 단위 JSON 중 type==="board_info"를 파싱한다.
 */
static void emit_board_info_json(void)
{
    board_identity_t ident = {0};
    esp_err_t err = ensure_board_identity(&ident);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "board_info: failed to ensure identity: %s", esp_err_to_name(err));
        return;
    }

    // 한 줄 JSON (마지막 '\n' 필수)
    printf("{\"type\":\"board_info\",\"PinCode\":\"%08" PRIu32 "\",\"LongDis\":%u,\"MacAddr\":\"%s\"}\n",
        ident.pin_code, static_cast<unsigned>(ident.long_dis), ident.mac_addr);
    fflush(stdout);

    ESP_LOGI(TAG, "board_info sent: pin=%08" PRIu32 ", dis=%u, mac=%s",
             ident.pin_code, static_cast<unsigned>(ident.long_dis), ident.mac_addr);
}

/**
 * host_serial_rx.cpp에서 app_main.cpp의 static 함수인 emit_board_info_json()을 호출할 수 있도록
 * 외부 링커 심볼(래퍼)을 제공한다.
 *
 * - 위치: `main/app_main.cpp` (본 파일)
 * - 사용처: `main/host_serial_rx.cpp`의 `anna_host_serial_rx_start()` / `host_serial_rx_task`
 */
extern "C" void anna_emit_board_info_json_for_rx_task(void)
{
    emit_board_info_json();
}

static void open_basic_commissioning_window_if_needed(void)
{
    chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
    if (commissionMgr.IsCommissioningWindowOpen()) {
        return;
    }

    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(
        kTimeoutSeconds, chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void last_fabric_factory_reset_work(intptr_t)
{
    esp_err_t err = anna_factory_reset_request_pending();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Deferred last-fabric factory reset failed: %s", esp_err_to_name(err));
        open_basic_commissioning_window_if_needed();
    }
}

static void power_cycle_clear_timer_callback(TimerHandle_t)
{
    s_power_cycle_clear_timer_armed = false;

    if (!anna_factory_reset_power_cycle_feature_enabled()) {
        return;
    }
    if (anna_factory_reset_is_pending() || anna_factory_reset_is_requested()) {
        return;
    }

    esp_err_t err = anna_factory_reset_store_power_cycle_count(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear power-cycle count after 10s uptime: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Power-cycle count cleared after %u ms uptime",
             static_cast<unsigned>(kPowerCycleClearWindowMs));
}

static void cancel_power_cycle_clear_timer_if_armed(void)
{
    if (!s_power_cycle_clear_timer || !s_power_cycle_clear_timer_armed) {
        return;
    }
    if (xTimerStop(s_power_cycle_clear_timer, 0) == pdPASS) {
        s_power_cycle_clear_timer_armed = false;
    }
}

static void arm_power_cycle_clear_timer_if_needed(void)
{
    if (!anna_factory_reset_power_cycle_feature_enabled() || anna_factory_reset_is_pending()) {
        return;
    }

    if (!s_power_cycle_clear_timer) {
        s_power_cycle_clear_timer = xTimerCreate("anna_pcycle_clear",
                                                 pdMS_TO_TICKS(kPowerCycleClearWindowMs), pdFALSE, nullptr,
                                                 power_cycle_clear_timer_callback);
        if (!s_power_cycle_clear_timer) {
            ESP_LOGW(TAG, "Failed to create power-cycle clear timer");
            return;
        }
    }

    if (xTimerStop(s_power_cycle_clear_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to stop existing power-cycle clear timer");
    }
    if (xTimerChangePeriod(s_power_cycle_clear_timer, pdMS_TO_TICKS(kPowerCycleClearWindowMs), 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to change power-cycle clear timer period");
        return;
    }
    if (xTimerStart(s_power_cycle_clear_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start power-cycle clear timer");
        return;
    }
    s_power_cycle_clear_timer_armed = true;
}

static void maybe_arm_power_cycle_factory_reset(void)
{
    anna_factory_reset_persistent_state_t reset_state = {};
    esp_err_t err = anna_factory_reset_prepare_power_cycle_state(true, &reset_state);
    if (err != ESP_OK || !reset_state.feature_enabled) {
        return;
    }

    if (reset_state.persistent_pending) {
        ESP_LOGW(TAG, "Persistent factory reset already armed: source=%s",
                 anna_factory_reset_source_name(reset_state.pending_source));
        return;
    }

    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason != ESP_RST_POWERON) {
        ESP_LOGI(TAG, "Skip power-cycle increment for reset_reason=%d", static_cast<int>(reset_reason));
        arm_power_cycle_clear_timer_if_needed();
        return;
    }

    uint8_t next_count = reset_state.power_cycle_count;
    if (next_count < UINT8_MAX) {
        ++next_count;
    }

    if (next_count < kPowerCycleResetThreshold) {
        err = anna_factory_reset_store_power_cycle_count(next_count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to store power-cycle count=%u: %s",
                     static_cast<unsigned>(next_count), esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "Power-cycle count updated: %u/%u",
                 static_cast<unsigned>(next_count), static_cast<unsigned>(kPowerCycleResetThreshold));
        arm_power_cycle_clear_timer_if_needed();
        return;
    }

    err = anna_factory_reset_store_power_cycle_count(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear power-cycle count before arm: %s", esp_err_to_name(err));
        return;
    }

    err = anna_factory_reset_store_pending_source(ANNA_FACTORY_RESET_SOURCE_POWER_CYCLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist power-cycle pending state: %s", esp_err_to_name(err));
        return;
    }

    err = anna_factory_reset_arm(ANNA_FACTORY_RESET_SOURCE_POWER_CYCLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm power-cycle factory reset: %s", esp_err_to_name(err));
        esp_err_t clear_err = anna_factory_reset_clear_persistent_state();
        if (clear_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to clear stale power-cycle pending state: %s", esp_err_to_name(clear_err));
        }
        return;
    }

    ESP_LOGW(TAG, "Power-cycle threshold reached, deferred factory reset armed");
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
#ifdef CONFIG_ENABLE_MEMORY_PROFILING
        memory_profiler_dump_heap_stat("commissioning complete");
#endif

        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
#ifdef CONFIG_ENABLE_MEMORY_PROFILING
        memory_profiler_dump_heap_stat("commissioning window opened");
#endif

        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        {
            ESP_LOGI(TAG, "Fabric removed successfully");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                if (anna_factory_reset_is_requested()) {
                    ESP_LOGW(TAG, "Factory reset already requested, ignore last-fabric trigger");
                    break;
                }

                esp_err_t arm_err = anna_factory_reset_arm(ANNA_FACTORY_RESET_SOURCE_LAST_FABRIC);
                if (arm_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to arm last-fabric factory reset: %s", esp_err_to_name(arm_err));
                    break;
                }

                CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(last_fabric_factory_reset_work, 0);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to schedule last-fabric factory reset, err:%" CHIP_ERROR_FORMAT, err.Format());
                    anna_factory_reset_clear_pending();
                    open_basic_commissioning_window_if_needed();
                }
            }
        break;
        }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
#ifdef CONFIG_ENABLE_MEMORY_PROFILING
        memory_profiler_dump_heap_stat("BLE deinitialized");
#endif
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Phase-1: route through mode_manager first (thin boundary) */
        err = mode_manager_pre_update(endpoint_id, cluster_id, attribute_id, val);
        if (err == ESP_OK) {
            /* Driver update (current authoritative logic) */
            app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
            err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
        }
    }

    if (type == POST_UPDATE) {
        /* Phase-1: allow mode_manager post hook (currently no-op) */
        mode_manager_on_post_update(endpoint_id, cluster_id, attribute_id);
        /* Immediate enforce (work queue) after commit */
        if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
            bool is_mode_ep = false;
            for (int i = 0; i < g_anna_cfg.modes.mode_count; i++) {
                if (endpoint_id == g_anna_cfg.modes.endpoint_id[i]) { 
                    is_mode_ep = true; 
                    break; 
                }
            }
            if (is_mode_ep) {
                // Queue mode apply; con_btn enforcement runs after mode winner commit in app_driver.cpp
                app_driver_queue_mode_apply();
            }
        }
        err = app_driver_attribute_post_update(endpoint_id, cluster_id, attribute_id, val);
    }

    if (cluster_id == ModeSelect::Id && attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        uint8_t new_mode = *(uint8_t *)val;
        ESP_LOGI(TAG, "CurrentMode updated to %d on endpoint %d", new_mode, endpoint_id);
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    anna_cfg_nvs_init();

    // provisioned=0이면 USB 프로비저닝 모드로 진입하고, Matter는 절대 시작하지 않는다.
    if (!anna_state_is_provisioned()) {
        // 부팅 직후 1회 board_info 송신
        emit_board_info_json();

        // app_main이 product/unit 수신 완료를 기다릴 수 있도록 EventGroup 연결
        EventGroupHandle_t eg = xEventGroupCreate();
        const EventBits_t kBitProductOk = BIT0;
        const EventBits_t kBitUnitDone = BIT1;
        anna_host_serial_rx_set_event_group(eg, kBitProductOk, kBitUnitDone);

        // UART0 라인 수신 태스크 시작 (FILE_SEND 처리 + file_ack 응답)
        anna_host_serial_rx_start();

        // product-info.json을 일정 시간 내에 받지 못하면 TIMEOUT ACK 후 재시작
        constexpr uint32_t kProductTimeoutMs = 30000;
        EventBits_t bits = xEventGroupWaitBits(eg, kBitProductOk, pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(kProductTimeoutMs));
        if ((bits & kBitProductOk) == 0) {
            printf("{\"type\":\"file_ack\",\"file\":\"product-info.json\",\"ok\":false,\"reason\":\"TIMEOUT\"}\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
            return;
        }

        // product 성공 후 unit-info.json은 best-effort로 짧게 대기(없어도 진행)
        (void)xEventGroupWaitBits(eg, kBitUnitDone, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
        // unit 처리 시도(성공/실패/미수신) 이후에 provisioned=1로 확정
        (void)anna_state_set_provisioned(1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    maybe_arm_power_cycle_factory_reset();

    /* json 파일 처리하기 */
    err = (esp_err_t)anna_cfg_load_from_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "anna_cfg_load_from_nvs failed: %s", esp_err_to_name(err));
    }


    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    /* Node Label 추가*/
    /* node label max size: 32bytes + 1byte(null terminator) */
    // ProductInfo.NodeLabel이 있으면 우선 사용, 없으면 기존 디폴트 유지
    const char *node_label = (g_anna_cfg.product_info.node_label[0] != '\0') ? g_anna_cfg.product_info.node_label : "Product Nemo";
    strlcpy(node_config.root_node.basic_information.node_label, node_label, sizeof(node_config.root_node.basic_information.node_label));


    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    settings_init(node);
#ifdef CONFIG_ENABLE_MEMORY_PROFILING
    memory_profiler_dump_heap_stat("settings_init");
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    /* Matter start */
#if CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER
    // Custom CommissionableDataProvider는 esp_matter::start() 내부 setup_providers()에서 적용되므로,
    // 반드시 start() 호출 전에 등록해야 한다.
    esp_matter::set_custom_commissionable_data_provider(&s_anna_commissionable_data_provider);
#endif

#if CONFIG_CUSTOM_DEVICE_INSTANCE_INFO_PROVIDER
    // Custom DeviceInstanceInfoProvider는 esp_matter::start() 내부 setup_providers()에서 적용되므로,
    // 반드시 start() 호출 전에 등록해야 한다.
    esp_matter::set_custom_device_instance_info_provider(&s_anna_device_instance_info_provider);
    ESP_LOGI(TAG, "Registered custom DeviceInstanceInfoProvider (AnnaDeviceInstanceInfoProvider)");
#endif

#if CONFIG_CUSTOM_DEVICE_INFO_PROVIDER
    // Custom DeviceInfoProvider는 esp_matter::start() 내부 setup_providers()에서 적용되므로,
    // 반드시 start() 호출 전에 등록해야 한다.
    //
    // 현재 AnnaDeviceInfoProvider는 ESP32DeviceInfoProvider를 그대로 상속하여
    // UserLabel(SetUserLabelList 등) 동작이 유지되도록 한다.
    esp_matter::set_custom_device_info_provider(&s_anna_device_info_provider);
    ESP_LOGI(TAG, "Registered custom DeviceInfoProvider (AnnaDeviceInfoProvider)");
#endif

    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

#ifdef CONFIG_ENABLE_MEMORY_PROFILING
    memory_profiler_dump_heap_stat("esp matter start");
#endif

    err = anna_factory_reset_request_pending();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Deferred factory reset request after esp_matter::start() failed: %s", esp_err_to_name(err));
    }
    if (anna_factory_reset_is_requested()) {
        cancel_power_cycle_clear_timer_if_armed();
        ESP_LOGW(TAG, "Factory reset requested after esp_matter::start(); skip post-start init");
        return;
    }

    settings_post_esp_start_init();

    /* chip shell 사용할 때 사용 */
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif
}
