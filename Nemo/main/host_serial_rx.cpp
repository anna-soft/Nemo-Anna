

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "host_serial_rx.h"


#include <cJSON.h>
#include <mbedtls/base64.h>

#include "anna_cfg.h"
#include "anna_cfg_parse.h"
#include "anna_state_storage.h"

static const char *TAG = "host_serial_rx";

// app_main.cpp에서 제공하는 extern "C" 링커 심볼(래퍼)
extern "C" void anna_emit_board_info_json_for_rx_task(void);

// 라인 1개(JSON 1개 + '\n')의 최대 크기 (base64 포함 고려)
static constexpr size_t kMaxLineBytes = 64 * 1024;

// Task stack은 JSON 파싱/임시 버퍼 사용을 고려해 여유 있게.
static constexpr uint32_t kTaskStackBytes = 8192;
static constexpr UBaseType_t kTaskPrio = 4;

static EventGroupHandle_t s_evt = nullptr;
static EventBits_t s_bit_product_ok = 0;
static EventBits_t s_bit_unit_done = 0;

extern "C" void anna_host_serial_rx_set_event_group(EventGroupHandle_t eg, EventBits_t productOkBit,
                                                    EventBits_t unitDoneBit)
{
    s_evt = eg;
    s_bit_product_ok = productOkBit;
    s_bit_unit_done = unitDoneBit;
}

static uint32_t crc32c_castagnoli(const uint8_t *data, size_t len)
{
    // CRC-32C (Castagnoli) reflected polynomial
    constexpr uint32_t kPoly = 0x82F63B78U;

    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ kPoly;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return ~crc;
}

static void to_upper_copy_8(char out[9], const char *in)
{
    // in은 최소 8 글자라고 가정하지 않는다(검증은 caller).
    for (int i = 0; i < 8; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[8] = '\0';
}

static void emit_file_ack(const char *file, bool ok, const char *reason_or_null)
{
    // guideline.md 규칙: JSON 1줄 + '\n'
    if (!file || !*file) file = "unknown";

    if (ok) {
        printf("{\"type\":\"file_ack\",\"file\":\"%s\",\"ok\":true}\n", file);
    } else {
        const char *reason = reason_or_null ? reason_or_null : "BAD_FORMAT";
        printf("{\"type\":\"file_ack\",\"file\":\"%s\",\"ok\":false,\"reason\":\"%s\"}\n", file, reason);
    }
    fflush(stdout);
}

static void emit_file_ack_crc_mismatch_with_data(const char *file,
                                                 const char *expected_crc32c_upper,
                                                 const char *actual_crc32c_upper,
                                                 const cJSON *payload_data_or_null)
{
    // 요청 포맷(요약):
    // {"type":"file_ack","file":"product-info.json","ok":false,"reason":"CRC_MISMATCH",
    //  "expected_crc32c":"...","actual_crc32c":"...","data":{...}}\n
    if (!file || !*file) file = "unknown";

    cJSON *ack = cJSON_CreateObject();
    if (!ack) {
        emit_file_ack(file, false, "CRC_MISMATCH");
        return;
    }

    cJSON_AddStringToObject(ack, "type", "file_ack");
    cJSON_AddStringToObject(ack, "file", file);
    cJSON_AddBoolToObject(ack, "ok", false);
    cJSON_AddStringToObject(ack, "reason", "CRC_MISMATCH");
    cJSON_AddStringToObject(ack, "expected_crc32c", expected_crc32c_upper ? expected_crc32c_upper : "");
    cJSON_AddStringToObject(ack, "actual_crc32c", actual_crc32c_upper ? actual_crc32c_upper : "");

    if (payload_data_or_null) {
        cJSON *dup = cJSON_Duplicate(payload_data_or_null, true /* recurse */);
        if (dup) {
            cJSON_AddItemToObject(ack, "data", dup);
        } else {
            cJSON_AddNullToObject(ack, "data");
        }
    } else {
        cJSON_AddNullToObject(ack, "data");
    }

    char *line = cJSON_PrintUnformatted(ack);
    if (line) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    } else {
        emit_file_ack(file, false, "CRC_MISMATCH");
    }

    cJSON_Delete(ack);
}

static bool is_supported_schema_version(const char *schema_version, const char *expected_prefix)
{
    if (!schema_version || !expected_prefix) return false;

    const size_t prefix_len = strlen(expected_prefix);
    if (strncmp(schema_version, expected_prefix, prefix_len) != 0) return false;

    const char *suffix = schema_version + prefix_len;
    if (*suffix == '\0' || !isdigit((unsigned char)*suffix)) return false;

    for (const char *p = suffix; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p) && *p != '.') {
            return false;
        }
    }

    return true;
}

static char *read_line_blocking(void)
{
    // '\n'까지 읽어 0-terminated 문자열을 반환한다. (caller free)
    // 너무 길면 남은 라인을 버리고 NULL 반환.
    size_t cap = 1024;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            // stdin이 아직 준비되지 않았거나 일시적 EOF일 수 있으므로 잠깐 양보
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == '\r') {
            // CR은 무시(Windows line ending)
            continue;
        }

        if (c == '\n') {
            buf[len] = '\0';
            return buf;
        }

        if (len + 1 >= cap) {
            size_t next = cap * 2;
            if (next > kMaxLineBytes) {
                // 라인 너무 김: '\n'까지 버퍼 비우고 종료
                while (c != '\n' && c != EOF) {
                    c = fgetc(stdin);
                }
                free(buf);
                return NULL;
            }
            char *nb = (char *)realloc(buf, next);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = next;
        }

        buf[len++] = (char)c;
    }
}

static esp_err_t handle_request_board_info(void (*emit_board_info_fn)(void))
{
    if (!emit_board_info_fn) return ESP_ERR_INVALID_ARG;
    emit_board_info_fn();
    return ESP_OK;
}

static esp_err_t handle_file_send(cJSON *root)
{
    const char *file = "unknown";

    const cJSON *j_file = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (cJSON_IsString(j_file) && j_file->valuestring) {
        file = j_file->valuestring;
    }

    const cJSON *j_raw_b64 = cJSON_GetObjectItemCaseSensitive(root, "raw_b64");
    const cJSON *j_data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsString(j_raw_b64) || !j_raw_b64->valuestring || !cJSON_IsObject(j_data)) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *j_meta = cJSON_GetObjectItemCaseSensitive(j_data, "meta");
    if (!cJSON_IsObject(j_meta)) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }

    // 요청한 "data: {product-info.json의 data}"는 message.data.data(= payload.data)를 의미
    const cJSON *j_payload_data = cJSON_GetObjectItemCaseSensitive(j_data, "data");

    const cJSON *j_payload_len = cJSON_GetObjectItemCaseSensitive(j_meta, "payloadByteLength");
    const cJSON *j_crc32c = cJSON_GetObjectItemCaseSensitive(j_meta, "crc32c");
    if (!cJSON_IsNumber(j_payload_len) || !cJSON_IsString(j_crc32c) || !j_crc32c->valuestring) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }

    const double payload_len_d = j_payload_len->valuedouble;
    if (!(payload_len_d >= 0) || payload_len_d > (double)SIZE_MAX) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t expected_len = (size_t)payload_len_d;

    const char *expected_crc = j_crc32c->valuestring;
    if (strlen(expected_crc) != 8) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }
    char expected_crc_upper[9];
    to_upper_copy_8(expected_crc_upper, expected_crc);

    // base64 decode (2-pass: size query -> decode)
    const uint8_t *in = (const uint8_t *)j_raw_b64->valuestring;
    const size_t in_len = strlen(j_raw_b64->valuestring);
    size_t out_len = 0;

    int rc = mbedtls_base64_decode(nullptr, 0, &out_len, in, in_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || out_len == 0) {
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }

    // +1 for NUL termination (raw bytes are expected to be UTF-8 JSON)
    uint8_t *raw = (uint8_t *)malloc(out_len + 1);
    if (!raw) {
        emit_file_ack(file, false, "WRITE_FAIL");
        return ESP_ERR_NO_MEM;
    }

    size_t raw_len = 0;
    rc = mbedtls_base64_decode(raw, out_len, &raw_len, in, in_len);
    if (rc != 0) {
        free(raw);
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }
    raw[raw_len] = '\0';

    if (raw_len != expected_len) {
        free(raw);
        emit_file_ack(file, false, "BAD_FORMAT");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t crc = crc32c_castagnoli(raw, raw_len);

    char actual_crc[9];
    snprintf(actual_crc, sizeof(actual_crc), "%08" PRIX32, crc);

    if (strcmp(actual_crc, expected_crc_upper) != 0) {
        emit_file_ack_crc_mismatch_with_data(file, expected_crc_upper, actual_crc,
                                             cJSON_IsObject(j_payload_data) ? j_payload_data : nullptr);
        free(raw);
        return ESP_FAIL;
    }

    // CRC OK: file별 후처리
    if (strcmp(file, "product-info.json") == 0) {
        // meta.schemaVersion은 "AnnaJson.v" 접두사 + 숫자/점 suffix 형식(필수)
        const cJSON *j_schema = cJSON_GetObjectItemCaseSensitive(j_meta, "schemaVersion");
        if (!cJSON_IsString(j_schema) || !j_schema->valuestring ||
            !is_supported_schema_version(j_schema->valuestring, "AnnaJson.v")) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }

        // payload.data.ProductInfo.SoftwareVersion 존재(필수)
        if (!cJSON_IsObject(j_payload_data)) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *j_pi = cJSON_GetObjectItemCaseSensitive(j_payload_data, "ProductInfo");
        const cJSON *j_sv = j_pi ? cJSON_GetObjectItemCaseSensitive(j_pi, "SoftwareVersion") : nullptr;
        if (!cJSON_IsObject(j_pi) || !(cJSON_IsNumber(j_sv) || cJSON_IsString(j_sv))) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }

        // raw bytes는 Host가 CRC 계산에 사용한 정확한 JSON.stringify(payload.data) 결과다.
        // 기존 파서/스토리지는 payload.data(JSON) 문자열을 기대한다.
        int perr = anna_cfg_parse_json((const char *)raw, raw_len);
        if (perr != ESP_OK) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_FAIL;
        }

        int serr = anna_cfg_save_to_nvs((const char *)raw, raw_len);
        if (serr != ESP_OK) {
            free(raw);
            emit_file_ack(file, false, "WRITE_FAIL");
            return ESP_FAIL;
        }

        /* meta.schemaVersion은 payload.data(JSON)와 별도로 NVS에 저장한다(추후 형태/호환 판단용). */
        if (cJSON_IsString(j_schema) && j_schema->valuestring) {
            (void)anna_cfg_save_schema_version_to_nvs(j_schema->valuestring);
        }

        // 브라우저로 성공 ACK를 먼저 보낸 뒤 app_main에서 provisioned=1로 전환/재부팅한다.
        emit_file_ack(file, true, nullptr);

        if (s_evt && s_bit_product_ok) {
            xEventGroupSetBits(s_evt, s_bit_product_ok);
        }
        free(raw);
        return ESP_OK;
    }

    if (strcmp(file, "unit-info.json") == 0) {
        // meta.schemaVersion은 "AnnaUnit.v" 접두사 + 숫자/점 suffix 형식(필수)
        const cJSON *j_schema = cJSON_GetObjectItemCaseSensitive(j_meta, "schemaVersion");
        if (!cJSON_IsString(j_schema) || !j_schema->valuestring ||
            !is_supported_schema_version(j_schema->valuestring, "AnnaUnit.v")) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }

        // payload.data.UnitInfo.SerialNumber / UniqueID 존재(필수)
        if (!cJSON_IsObject(j_payload_data)) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *j_ui = cJSON_GetObjectItemCaseSensitive(j_payload_data, "UnitInfo");
        const cJSON *j_sn = j_ui ? cJSON_GetObjectItemCaseSensitive(j_ui, "SerialNumber") : nullptr;
        const cJSON *j_uid = j_ui ? cJSON_GetObjectItemCaseSensitive(j_ui, "UniqueID") : nullptr;
        if (!cJSON_IsObject(j_ui) || !cJSON_IsString(j_sn) || !cJSON_IsString(j_uid) ||
            !j_sn->valuestring || !j_uid->valuestring) {
            free(raw);
            emit_file_ack(file, false, "BAD_FORMAT");
            return ESP_ERR_INVALID_ARG;
        }

        // Best-effort 저장 시도(실패해도 전체 진행을 막지 않음)
        esp_err_t uerr = (esp_err_t)anna_state_set_unit_blob(raw, raw_len);
        if (uerr != ESP_OK) {
            ESP_LOGW(TAG, "unit_blob save failed (best-effort): %s", esp_err_to_name(uerr));
        }

        emit_file_ack(file, true, nullptr);
        if (s_evt && s_bit_unit_done) {
            xEventGroupSetBits(s_evt, s_bit_unit_done);
        }
        free(raw);
        return ESP_OK;
    }

    // Unknown file: CRC ok이면 일단 ACK
    emit_file_ack(file, true, nullptr);
    free(raw);
    return ESP_OK;
}

static void host_serial_rx_task(void *arg)
{
    // app_main.cpp에서 emit_board_info_json()를 재사용하기 위해 함수 포인터로 전달받는다.
    void (*emit_board_info_fn)(void) = (void (*)(void))arg;

    // stdin/stdout 버퍼링이 남아 있으면 라인 단위 프로토콜이 흔들릴 수 있어 unbuffered로 설정.
    setvbuf(stdin, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);

    ESP_LOGI(TAG, "host_serial_rx_task started (UART0 line reader via stdin)");

    while (true) {
        char *line = read_line_blocking();
        if (!line) {
            emit_file_ack("unknown", false, "BAD_FORMAT");
            continue;
        }

        // 빈 줄은 무시
        if (line[0] == '\0') {
            free(line);
            continue;
        }

        cJSON *root = cJSON_Parse(line);
        if (!root) {
            free(line);
            emit_file_ack("unknown", false, "BAD_FORMAT");
            continue;
        }

        const cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        if (cJSON_IsString(j_cmd) && j_cmd->valuestring) {
            if (strcmp(j_cmd->valuestring, "REQUEST_BOARD_INFO") == 0) {
                (void)handle_request_board_info(emit_board_info_fn);
            } else if (strcmp(j_cmd->valuestring, "FILE_SEND") == 0) {
                (void)handle_file_send(root);
            } else {
                // 알 수 없는 cmd는 무시(향후 확장)
            }
        } else {
            // cmd가 없으면 무시
        }

        cJSON_Delete(root);
        free(line);
    }
}

void anna_host_serial_rx_start(void)
{
    // app_main.cpp의 emit_board_info_json()를 그대로 재사용하려고, 구현 쪽에서 symbol을 참조하지 않고
    // 시작 시점에 함수 포인터를 전달받는 구조로 했다(링크 의존성/순환 include 최소화).
    BaseType_t ok = xTaskCreate(host_serial_rx_task, "host_serial_rx", kTaskStackBytes / sizeof(StackType_t),
                               (void *)anna_emit_board_info_json_for_rx_task, kTaskPrio, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create host_serial_rx_task");
    }
}

