#include "anna_cloud_sync.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <app/server/Server.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <platform/CHIPDeviceLayer.h>

#include "anna_cloud_identity.h"
#include "anna_runtime_rebuild.h"
#include "anna_state_storage.h"

using chip::DeviceLayer::ChipDeviceEvent;
using chip::DeviceLayer::DeviceEventType::kCommissioningComplete;
using chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted;
using chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped;
using chip::DeviceLayer::DeviceEventType::kBLEDeinitialized;
using chip::DeviceLayer::DeviceEventType::kFabricCommitted;
using chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged;
using chip::DeviceLayer::InterfaceIpChangeType;

namespace {
constexpr char TAG[] = "anna_cloud_sync";
constexpr int64_t kHttpsUnixTimeFloor = 1776643200LL; // 2026-04-20T00:00:00Z
constexpr size_t kSchemaVersionMaxLen = 64;
constexpr size_t kBaseUrlMaxLen = 256;
constexpr size_t kAuthHeaderMaxLen = ANNA_MAX_DEVICE_TOKEN_LEN + 16;
constexpr size_t kResultCodeMaxLen = 40;
constexpr size_t kDebugTraceRingSize = 16;

enum class worker_command_type : uint8_t {
    pull_apply = 0,
    ack_only = 1,
};

typedef struct {
    worker_command_type type;
    bool late_ip_fallback_used;
    bool test_harness_forced;
    char result_code[kResultCodeMaxLen];
} anna_cloud_worker_cmd_t;

typedef struct {
    bool session_open;
    bool ip_ready;
    bool fabric_committed_seen;
    bool commissioning_complete_seen;
    bool session_stopped_seen;
    bool ble_deinitialized_seen;
    bool sync_attempt_started;
    bool sync_attempt_finished;
    bool atomic_apply_started;
    bool terminal_result_locked;
    bool late_ip_fallback_used;
    bool primary_condition_observed;
    bool current_ip_ready;
    bool test_harness_force_pull_pending;
    bool test_harness_defer_primary_pending;
    bool test_harness_force_pre_apply_late_pending;
    bool test_harness_force_pre_apply_post_fabric_pending;
    int64_t session_started_at_us;
    int64_t sync_attempt_started_at_us;
} anna_cloud_sync_state_t;

typedef struct {
    bool session_open;
    bool ip_ready;
    bool fabric_committed_seen;
    bool commissioning_complete_seen;
    bool session_stopped_seen;
    bool ble_deinitialized_seen;
    bool sync_attempt_started;
    bool sync_attempt_finished;
    bool atomic_apply_started;
    bool terminal_result_locked;
    bool late_ip_fallback_used;
    bool primary_condition_observed;
    bool current_ip_ready;
    bool test_harness_force_pull_pending;
    int64_t session_started_at_us;
    int64_t sync_attempt_started_at_us;
    size_t fabric_count;
} anna_cloud_sync_debug_snapshot_t;

typedef struct {
    uint32_t event_type;
    const char *event_name;
    const char *event_note;
    const char *pull_skip_reason;
    bool prepared_pull;
    bool prepared_ack;
    bool needs_terminal_close_out;
    const char *default_terminal_result;
    anna_cloud_sync_debug_snapshot_t snapshot;
} anna_cloud_sync_trace_entry_t;

typedef struct {
    char *body;
    size_t len;
    size_t cap;
} http_buffer_t;

typedef struct {
    char *candidate_raw_json;
    char candidate_schema_version[kSchemaVersionMaxLen];
    anna_runtime_apply_result_t result;
    TaskHandle_t waiter_task;
} sync_apply_context_t;

static QueueHandle_t s_worker_queue = nullptr;
static TaskHandle_t s_worker_task = nullptr;
static anna_cloud_sync_state_t s_state = {};
static anna_cloud_sync_trace_entry_t s_trace_ring[kDebugTraceRingSize] = {};
static size_t s_trace_ring_count = 0;
static size_t s_trace_ring_next = 0;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *device_event_name(const ChipDeviceEvent *event)
{
    if (!event) {
        return nullptr;
    }

    switch (event->Type) {
    case kCommissioningSessionStarted:
        return "kCommissioningSessionStarted";
    case kInterfaceIpAddressChanged:
        return "kInterfaceIpAddressChanged";
    case kCommissioningComplete:
        return "kCommissioningComplete";
    case kCommissioningSessionStopped:
        return "kCommissioningSessionStopped";
    case kFabricCommitted:
        return "kFabricCommitted";
    case kBLEDeinitialized:
        return "kBLEDeinitialized";
    default:
        return nullptr;
    }
}

static bool is_supported_schema_version(const char *schema_version, const char *expected_prefix)
{
    if (!schema_version || !expected_prefix) {
        return false;
    }

    size_t prefix_len = strlen(expected_prefix);
    if (strncmp(schema_version, expected_prefix, prefix_len) != 0) {
        return false;
    }

    const char *suffix = schema_version + prefix_len;
    if (*suffix == '\0' || !isdigit(static_cast<unsigned char>(*suffix))) {
        return false;
    }
    for (const char *p = suffix; *p != '\0'; ++p) {
        if (!isdigit(static_cast<unsigned char>(*p)) && *p != '.') {
            return false;
        }
    }
    return true;
}

static int month_from_abbrev(const char mon[4])
{
    static const char *kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; ++i) {
        if (strncmp(mon, kMonths[i], 3) == 0) {
            return i + 1;
        }
    }
    return 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

static bool get_compile_time_unix(int64_t *out_unix_seconds)
{
    if (!out_unix_seconds) {
        return false;
    }

    char month[4] = { 0 };
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 || sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return false;
    }

    const int month_value = month_from_abbrev(month);
    if (month_value <= 0 || day <= 0 || hour < 0 || minute < 0 || second < 0) {
        return false;
    }

    const int64_t days = days_from_civil(year, static_cast<unsigned>(month_value), static_cast<unsigned>(day));
    *out_unix_seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

static void ensure_time_floor_for_https(void)
{
    static bool s_compile_time_known = false;
    static int64_t s_compile_time_unix = 0;
    int64_t floor_unix = kHttpsUnixTimeFloor;

    if (!s_compile_time_known) {
        if (!get_compile_time_unix(&s_compile_time_unix)) {
            ESP_LOGW(TAG, "failed to parse compile time for HTTPS time floor");
        } else {
            s_compile_time_known = true;
        }
    }

    if (s_compile_time_known && s_compile_time_unix > floor_unix) {
        floor_unix = s_compile_time_unix;
    }

    struct timeval now = {};
    gettimeofday(&now, nullptr);
    if (static_cast<int64_t>(now.tv_sec) >= floor_unix) {
        return;
    }

    struct timeval floor_time = {};
    floor_time.tv_sec = static_cast<time_t>(floor_unix);
    floor_time.tv_usec = 0;
    if (settimeofday(&floor_time, nullptr) != 0) {
        ESP_LOGW(TAG, "failed to raise RTC to HTTPS time floor (%lld)", static_cast<long long>(floor_unix));
        return;
    }

    ESP_LOGI(TAG, "raised RTC to HTTPS time floor: %lld", static_cast<long long>(floor_unix));
}

static void build_base_url(char out[kBaseUrlMaxLen])
{
    strlcpy(out, CONFIG_ANNA_CLOUD_SYNC_BASE_URL, kBaseUrlMaxLen);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[len - 1] = '\0';
        --len;
    }
}

static int64_t elapsed_ms_since(int64_t started_at_us)
{
    if (started_at_us <= 0) {
        return -1;
    }
    return (esp_timer_get_time() - started_at_us) / 1000;
}

static void capture_debug_snapshot_locked(anna_cloud_sync_debug_snapshot_t *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->session_open = s_state.session_open;
    out->ip_ready = s_state.ip_ready;
    out->fabric_committed_seen = s_state.fabric_committed_seen;
    out->commissioning_complete_seen = s_state.commissioning_complete_seen;
    out->session_stopped_seen = s_state.session_stopped_seen;
    out->ble_deinitialized_seen = s_state.ble_deinitialized_seen;
    out->sync_attempt_started = s_state.sync_attempt_started;
    out->sync_attempt_finished = s_state.sync_attempt_finished;
    out->atomic_apply_started = s_state.atomic_apply_started;
    out->terminal_result_locked = s_state.terminal_result_locked;
    out->late_ip_fallback_used = s_state.late_ip_fallback_used;
    out->primary_condition_observed = s_state.primary_condition_observed;
    out->current_ip_ready = s_state.current_ip_ready;
    out->test_harness_force_pull_pending =
        s_state.test_harness_force_pull_pending || s_state.test_harness_defer_primary_pending ||
        s_state.test_harness_force_pre_apply_late_pending || s_state.test_harness_force_pre_apply_post_fabric_pending;
    out->session_started_at_us = s_state.session_started_at_us;
    out->sync_attempt_started_at_us = s_state.sync_attempt_started_at_us;
}

static bool safe_apply_window_closed_locked(void)
{
    return s_state.commissioning_complete_seen || s_state.session_stopped_seen || s_state.ble_deinitialized_seen;
}

static const char *default_close_result_locked(void)
{
    return s_state.fabric_committed_seen ? ANNA_RESULT_CODE_SKIPPED_POST_FABRIC : ANNA_RESULT_CODE_LATE_IP_WINDOW_MISSED;
}

static void mark_terminal_result_locked(const char *result_code)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.terminal_result_locked = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "terminal result locked: result=%s", result_code ? result_code : "-");
}

static const char *describe_pull_gate_locked(void)
{
    if (!s_state.session_open) {
        return "session_closed";
    }
    if (!s_state.ip_ready) {
        return "ip_not_ready";
    }
    if (s_state.commissioning_complete_seen) {
        return "commissioning_already_complete";
    }
    if (s_state.session_stopped_seen) {
        return "session_already_stopped";
    }
    if (s_state.ble_deinitialized_seen) {
        return "ble_already_deinitialized";
    }
    if (s_state.sync_attempt_started) {
        return "attempt_already_started";
    }
    if (s_state.sync_attempt_finished) {
        return "attempt_already_finished";
    }
    return "blocked_unknown";
}

static void log_debug_snapshot(uint32_t event_type, const char *event_name, const char *event_note, const char *pull_skip_reason,
                               bool prepared_pull, bool prepared_ack, bool needs_terminal_close_out,
                               const char *default_terminal_result,
                               const anna_cloud_sync_debug_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    ESP_LOGI(TAG,
             "event trace: event_type=%" PRIu32 " event=%s note=%s pull_skip=%s prepared_pull=%d prepared_ack=%d "
             "terminal_candidate=%d default_terminal=%s fabric_count=%u session_ms=%lld attempt_ms=%lld "
             "session_open=%d ip_ready=%d current_ip_ready=%d "
             "fabric_committed=%d commissioning_complete=%d session_stopped=%d ble_deinitialized=%d started=%d finished=%d "
             "atomic_apply_started=%d terminal_locked=%d late_ip_fallback=%d primary_seen=%d test_harness_pending=%d",
             event_type, event_name ? event_name : "unknown", event_note ? event_note : "-",
             pull_skip_reason ? pull_skip_reason : "-", prepared_pull ? 1 : 0, prepared_ack ? 1 : 0,
             needs_terminal_close_out ? 1 : 0, default_terminal_result ? default_terminal_result : "-",
             static_cast<unsigned>(snapshot->fabric_count),
             static_cast<long long>(elapsed_ms_since(snapshot->session_started_at_us)),
             static_cast<long long>(elapsed_ms_since(snapshot->sync_attempt_started_at_us)), snapshot->session_open ? 1 : 0,
             snapshot->ip_ready ? 1 : 0, snapshot->current_ip_ready ? 1 : 0, snapshot->fabric_committed_seen ? 1 : 0,
             snapshot->commissioning_complete_seen ? 1 : 0, snapshot->session_stopped_seen ? 1 : 0,
             snapshot->ble_deinitialized_seen ? 1 : 0, snapshot->sync_attempt_started ? 1 : 0,
             snapshot->sync_attempt_finished ? 1 : 0, snapshot->atomic_apply_started ? 1 : 0,
             snapshot->terminal_result_locked ? 1 : 0,
             snapshot->late_ip_fallback_used ? 1 : 0, snapshot->primary_condition_observed ? 1 : 0,
             snapshot->test_harness_force_pull_pending ? 1 : 0);
}

static void reset_debug_trace_locked(void)
{
    memset(s_trace_ring, 0, sizeof(s_trace_ring));
    s_trace_ring_count = 0;
    s_trace_ring_next = 0;
}

static void store_debug_trace(uint32_t event_type, const char *event_name, const char *event_note, const char *pull_skip_reason,
                              bool prepared_pull, bool prepared_ack, bool needs_terminal_close_out,
                              const char *default_terminal_result,
                              const anna_cloud_sync_debug_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    anna_cloud_sync_trace_entry_t *entry = &s_trace_ring[s_trace_ring_next];
    memset(entry, 0, sizeof(*entry));
    entry->event_type = event_type;
    entry->event_name = event_name;
    entry->event_note = event_note;
    entry->pull_skip_reason = pull_skip_reason;
    entry->prepared_pull = prepared_pull;
    entry->prepared_ack = prepared_ack;
    entry->needs_terminal_close_out = needs_terminal_close_out;
    entry->default_terminal_result = default_terminal_result;
    entry->snapshot = *snapshot;
    s_trace_ring_next = (s_trace_ring_next + 1) % kDebugTraceRingSize;
    if (s_trace_ring_count < kDebugTraceRingSize) {
        ++s_trace_ring_count;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void dump_debug_trace_history(const char *reason)
{
    anna_cloud_sync_trace_entry_t local_entries[kDebugTraceRingSize] = {};
    size_t count = 0;
    size_t start = 0;

    portENTER_CRITICAL(&s_state_lock);
    count = s_trace_ring_count;
    if (count > kDebugTraceRingSize) {
        count = kDebugTraceRingSize;
    }
    start = (s_trace_ring_count == kDebugTraceRingSize) ? s_trace_ring_next : 0;
    for (size_t i = 0; i < count; ++i) {
        local_entries[i] = s_trace_ring[(start + i) % kDebugTraceRingSize];
    }
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "trace history dump start: reason=%s entries=%u", reason ? reason : "-", static_cast<unsigned>(count));
    for (size_t i = 0; i < count; ++i) {
        const anna_cloud_sync_trace_entry_t &entry = local_entries[i];
        ESP_LOGI(TAG,
                 "trace history[%u]: event_type=%" PRIu32 " event=%s note=%s pull_skip=%s prepared_pull=%d prepared_ack=%d "
                 "terminal_candidate=%d default_terminal=%s fabric_count=%u session_ms=%lld attempt_ms=%lld "
                 "session_open=%d ip_ready=%d current_ip_ready=%d fabric_committed=%d commissioning_complete=%d "
                 "session_stopped=%d ble_deinitialized=%d started=%d finished=%d atomic_apply_started=%d terminal_locked=%d "
                 "late_ip_fallback=%d primary_seen=%d test_harness_pending=%d",
                 static_cast<unsigned>(i), entry.event_type, entry.event_name ? entry.event_name : "unknown",
                 entry.event_note ? entry.event_note : "-", entry.pull_skip_reason ? entry.pull_skip_reason : "-",
                 entry.prepared_pull ? 1 : 0, entry.prepared_ack ? 1 : 0, entry.needs_terminal_close_out ? 1 : 0,
                 entry.default_terminal_result ? entry.default_terminal_result : "-",
                 static_cast<unsigned>(entry.snapshot.fabric_count),
                 static_cast<long long>(elapsed_ms_since(entry.snapshot.session_started_at_us)),
                 static_cast<long long>(elapsed_ms_since(entry.snapshot.sync_attempt_started_at_us)),
                 entry.snapshot.session_open ? 1 : 0, entry.snapshot.ip_ready ? 1 : 0,
                 entry.snapshot.current_ip_ready ? 1 : 0, entry.snapshot.fabric_committed_seen ? 1 : 0,
                 entry.snapshot.commissioning_complete_seen ? 1 : 0, entry.snapshot.session_stopped_seen ? 1 : 0,
                 entry.snapshot.ble_deinitialized_seen ? 1 : 0, entry.snapshot.sync_attempt_started ? 1 : 0,
                 entry.snapshot.sync_attempt_finished ? 1 : 0, entry.snapshot.atomic_apply_started ? 1 : 0,
                 entry.snapshot.terminal_result_locked ? 1 : 0,
                 entry.snapshot.late_ip_fallback_used ? 1 : 0, entry.snapshot.primary_condition_observed ? 1 : 0,
                 entry.snapshot.test_harness_force_pull_pending ? 1 : 0);
    }
    ESP_LOGI(TAG, "trace history dump end: reason=%s", reason ? reason : "-");
}

static bool queue_worker_command(const anna_cloud_worker_cmd_t *cmd)
{
    if (!s_worker_queue || !cmd) {
        return false;
    }
    BaseType_t ok = xQueueSend(s_worker_queue, cmd, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "worker queue full, dropping command type=%u", static_cast<unsigned>(cmd->type));
        return false;
    }
    ESP_LOGI(TAG, "worker command queued: type=%u result=%s late_ip_fallback=%d test_harness_forced=%d",
             static_cast<unsigned>(cmd->type), cmd->type == worker_command_type::ack_only ? cmd->result_code : "-",
             cmd->late_ip_fallback_used ? 1 : 0, cmd->test_harness_forced ? 1 : 0);
    return true;
}

static bool prepare_pull_command_locked(bool late_ip_fallback, bool test_harness_forced, anna_cloud_worker_cmd_t *out_cmd)
{
    if (!out_cmd || !s_state.session_open || !s_state.ip_ready || safe_apply_window_closed_locked() || s_state.sync_attempt_started ||
        s_state.sync_attempt_finished) {
        return false;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->type = worker_command_type::pull_apply;
    out_cmd->late_ip_fallback_used = late_ip_fallback;
    out_cmd->test_harness_forced = test_harness_forced;
    s_state.sync_attempt_started = true;
    s_state.late_ip_fallback_used = late_ip_fallback;
    s_state.sync_attempt_started_at_us = esp_timer_get_time();
    return true;
}

static bool maybe_prepare_pull_command_locked(bool from_ip_event, anna_cloud_worker_cmd_t *out_cmd,
                                              anna_cloud_sync_test_intent_t *out_consumed_test_intent)
{
    if (out_consumed_test_intent) {
        *out_consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_NONE;
    }
    if (!out_cmd || !s_state.session_open || !s_state.ip_ready || safe_apply_window_closed_locked() || s_state.sync_attempt_started ||
        s_state.sync_attempt_finished) {
        return false;
    }

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    if (from_ip_event && s_state.test_harness_defer_primary_pending && !s_state.primary_condition_observed) {
        s_state.primary_condition_observed = true;
        s_state.test_harness_defer_primary_pending = false;
        if (out_consumed_test_intent) {
            *out_consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP;
        }
        return false;
    }
    const bool test_harness_forced = from_ip_event && s_state.test_harness_force_pull_pending;
#else
    const bool test_harness_forced = false;
#endif
    const bool late_ip_fallback = from_ip_event && s_state.primary_condition_observed && !s_state.fabric_committed_seen;
    s_state.primary_condition_observed = true;
    return prepare_pull_command_locked(late_ip_fallback, test_harness_forced, out_cmd);
}

static bool prepare_ack_only_command_locked(const char *result_code, bool late_ip_fallback_used, anna_cloud_worker_cmd_t *out_cmd)
{
    if (!out_cmd || s_state.sync_attempt_finished) {
        return false;
    }

    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->type = worker_command_type::ack_only;
    out_cmd->late_ip_fallback_used = late_ip_fallback_used;
    strlcpy(out_cmd->result_code, result_code, sizeof(out_cmd->result_code));
    s_state.sync_attempt_finished = true;
    s_state.terminal_result_locked = true;
    return true;
}

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
static const char *cloud_sync_test_intent_name(anna_cloud_sync_test_intent_t intent);
static void consume_cloud_sync_test_harness_intent(anna_cloud_sync_test_intent_t intent);
#endif

static bool runtime_pre_apply_guard(const anna_cfg_diff_t *diff, void *ctx, const char **out_block_result_code)
{
    (void)diff;
    (void)ctx;

    bool window_closed = false;
    const char *block_result = nullptr;
#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    anna_cloud_sync_test_intent_t consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_NONE;
#endif

    portENTER_CRITICAL(&s_state_lock);
#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    if (s_state.test_harness_force_pre_apply_late_pending) {
        s_state.test_harness_force_pre_apply_late_pending = false;
        s_state.ble_deinitialized_seen = true;
        consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_LATE_WINDOW_CLOSE;
    } else if (s_state.test_harness_force_pre_apply_post_fabric_pending) {
        s_state.test_harness_force_pre_apply_post_fabric_pending = false;
        s_state.fabric_committed_seen = true;
        s_state.ble_deinitialized_seen = true;
        consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_POST_FABRIC_CLOSE;
    }
#endif
    window_closed = safe_apply_window_closed_locked();
    if (window_closed) {
        block_result = default_close_result_locked();
    } else {
        s_state.atomic_apply_started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    if (consumed_test_intent != ANNA_CLOUD_SYNC_TEST_INTENT_NONE) {
        ESP_LOGI(TAG, "test harness forcing pre-apply window close: mode=%s result=%s",
                 cloud_sync_test_intent_name(consumed_test_intent), block_result ? block_result : "-");
        consume_cloud_sync_test_harness_intent(consumed_test_intent);
    }
#endif

    if (window_closed) {
        if (out_block_result_code) {
            *out_block_result_code = block_result;
        }
        ESP_LOGW(TAG, "pre-apply guard blocked atomic apply: result=%s", block_result ? block_result : "-");
        return false;
    }

    ESP_LOGI(TAG, "pre-apply guard allowed atomic apply");
    return true;
}

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
static const char *cloud_sync_test_intent_name(anna_cloud_sync_test_intent_t intent)
{
    switch (intent) {
    case ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PULL_ON_NEXT_IP:
        return "FORCE_PULL_ON_NEXT_IP";
    case ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP:
        return "DEFER_PRIMARY_TO_NEXT_IP";
    case ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_LATE_WINDOW_CLOSE:
        return "FORCE_PRE_APPLY_LATE_WINDOW_CLOSE";
    case ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_POST_FABRIC_CLOSE:
        return "FORCE_PRE_APPLY_POST_FABRIC_CLOSE";
    case ANNA_CLOUD_SYNC_TEST_INTENT_NONE:
    default:
        return "NONE";
    }
}

static void arm_cloud_sync_test_harness_if_present(void)
{
    anna_cloud_sync_test_intent_t intent = ANNA_CLOUD_SYNC_TEST_INTENT_NONE;
    esp_err_t err = (esp_err_t) anna_state_get_cloud_sync_test_intent(&intent);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "test harness intent load failed: %s", esp_err_to_name(err));
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_state.test_harness_force_pull_pending = false;
    s_state.test_harness_defer_primary_pending = false;
    s_state.test_harness_force_pre_apply_late_pending = false;
    s_state.test_harness_force_pre_apply_post_fabric_pending = false;
    if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PULL_ON_NEXT_IP) {
        s_state.test_harness_force_pull_pending = true;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP) {
        s_state.test_harness_defer_primary_pending = true;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_LATE_WINDOW_CLOSE) {
        s_state.test_harness_force_pre_apply_late_pending = true;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_POST_FABRIC_CLOSE) {
        s_state.test_harness_force_pre_apply_post_fabric_pending = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "test harness armed: mode=%s", cloud_sync_test_intent_name(intent));
}

static void consume_cloud_sync_test_harness_intent(anna_cloud_sync_test_intent_t intent)
{
    if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_NONE) {
        return;
    }

    esp_err_t err = (esp_err_t) anna_state_clear_cloud_sync_test_intent();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "test harness consume clear failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "test harness consumed: mode=%s", cloud_sync_test_intent_name(intent));
    }

    portENTER_CRITICAL(&s_state_lock);
    if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PULL_ON_NEXT_IP) {
        s_state.test_harness_force_pull_pending = false;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP) {
        s_state.test_harness_defer_primary_pending = false;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_LATE_WINDOW_CLOSE) {
        s_state.test_harness_force_pre_apply_late_pending = false;
    } else if (intent == ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PRE_APPLY_POST_FABRIC_CLOSE) {
        s_state.test_harness_force_pre_apply_post_fabric_pending = false;
    }
    portEXIT_CRITICAL(&s_state_lock);
}
#endif

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) {
        return ESP_OK;
    }

    http_buffer_t *buffer = static_cast<http_buffer_t *>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = buffer->len + static_cast<size_t>(evt->data_len) + 1;
    if (needed > buffer->cap) {
        size_t next_cap = buffer->cap == 0 ? needed : buffer->cap;
        while (next_cap < needed) {
            next_cap *= 2;
        }
        char *next = static_cast<char *>(realloc(buffer->body, next_cap));
        if (!next) {
            return ESP_ERR_NO_MEM;
        }
        buffer->body = next;
        buffer->cap = next_cap;
    }

    memcpy(buffer->body + buffer->len, evt->data, evt->data_len);
    buffer->len += static_cast<size_t>(evt->data_len);
    buffer->body[buffer->len] = '\0';
    return ESP_OK;
}

static esp_err_t perform_json_post(const char *path, const char *bearer_token, const char *request_body, http_buffer_t *response_buffer,
                                   int *out_status_code)
{
    if (!path || !bearer_token || !request_body || !response_buffer || !out_status_code) {
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[kBaseUrlMaxLen] = { 0 };
    build_base_url(base_url);
    char url[kBaseUrlMaxLen + 64] = { 0 };
    snprintf(url, sizeof(url), "%s%s", base_url, path);

    char auth_header[kAuthHeaderMaxLen] = { 0 };
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);

    ensure_time_floor_for_https();

    response_buffer->body = nullptr;
    response_buffer->len = 0;
    response_buffer->cap = 0;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = CONFIG_ANNA_CLOUD_SYNC_HTTP_TIMEOUT_MS;
    config.event_handler = http_event_handler;
    config.user_data = response_buffer;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *out_status_code = esp_http_client_get_status_code(client);
    } else {
        *out_status_code = 0;
    }
    esp_http_client_cleanup(client);
    return err;
}

static const char *map_http_failure_result(esp_err_t transport_err, int status_code)
{
    if (transport_err != ESP_OK) {
        return ANNA_RESULT_CODE_NETWORK_TIMEOUT;
    }
    if (status_code == 400) {
        return ANNA_RESULT_CODE_BAD_FORMAT;
    }
    if (status_code == 401 || status_code == 404 || status_code == 409) {
        return ANNA_RESULT_CODE_AUTH_FAILED;
    }
    return ANNA_RESULT_CODE_NETWORK_TIMEOUT;
}

static char *build_pull_request_body(const anna_cloud_bootstrap_t *bootstrap, const char *mac_addr)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "UniqueID", bootstrap->unique_id);
    cJSON_AddStringToObject(root, "MacAddr", mac_addr);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static char *build_ack_request_body(const anna_cloud_bootstrap_t *bootstrap, const char *mac_addr, const char *result_code,
                                    bool late_ip_fallback_used, bool include_version, uint32_t applied_software_version)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "UniqueID", bootstrap->unique_id);
    cJSON_AddStringToObject(root, "MacAddr", mac_addr);
    cJSON_AddStringToObject(root, "ResultCode", result_code);
    if (late_ip_fallback_used) {
        cJSON_AddBoolToObject(root, "LateIpFallbackUsed", true);
    }
    if (include_version) {
        cJSON_AddNumberToObject(root, "AppliedSoftwareVersion", static_cast<double>(applied_software_version));
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static bool validate_pull_envelope(const char *response_body, char **out_candidate_json, char out_schema[kSchemaVersionMaxLen])
{
    if (!response_body || !out_candidate_json || !out_schema) {
        return false;
    }

    *out_candidate_json = nullptr;
    out_schema[0] = '\0';

    cJSON *root = cJSON_Parse(response_body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return false;
    }

    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, "meta");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(meta, "schemaVersion");
    const cJSON *product_info = cJSON_GetObjectItemCaseSensitive(data, "ProductInfo");
    const cJSON *software_version = cJSON_GetObjectItemCaseSensitive(product_info, "SoftwareVersion");

    if (!cJSON_IsObject(meta) || !cJSON_IsObject(data) || !cJSON_IsString(schema) || !schema->valuestring ||
        !is_supported_schema_version(schema->valuestring, "AnnaJson.v") || !cJSON_IsObject(product_info) ||
        !(cJSON_IsNumber(software_version) || cJSON_IsString(software_version))) {
        cJSON_Delete(root);
        return false;
    }

    strlcpy(out_schema, schema->valuestring, kSchemaVersionMaxLen);
    *out_candidate_json = cJSON_PrintUnformatted(data);
    cJSON_Delete(root);
    return *out_candidate_json != nullptr;
}

static void sync_apply_work(intptr_t arg)
{
    sync_apply_context_t *ctx = reinterpret_cast<sync_apply_context_t *>(arg);
    if (!ctx) {
        return;
    }

    (void)anna_runtime_apply_candidate(ctx->candidate_raw_json, ctx->candidate_schema_version, runtime_pre_apply_guard, nullptr,
                                       &ctx->result);
    xTaskNotifyGive(ctx->waiter_task);
}

static void finalize_worker_attempt(bool attempt_started, bool finished)
{
    portENTER_CRITICAL(&s_state_lock);
    if (attempt_started) {
        s_state.sync_attempt_started = true;
    }
    s_state.sync_attempt_finished = finished;
    portEXIT_CRITICAL(&s_state_lock);
}

static void send_ack_if_possible(const anna_cloud_bootstrap_t *bootstrap, const char *mac_addr, const char *result_code,
                                 bool late_ip_fallback_used, bool include_version, uint32_t applied_software_version)
{
    if (!bootstrap || !bootstrap->has_unique_id || !bootstrap->has_device_token) {
        return;
    }

    char *body = build_ack_request_body(bootstrap, mac_addr, result_code, late_ip_fallback_used, include_version,
                                        applied_software_version);
    if (!body) {
        return;
    }

    ESP_LOGI(TAG, "config/ack send start: result=%s late_ip_fallback=%d include_version=%d applied_version=%" PRIu32, result_code,
             late_ip_fallback_used ? 1 : 0, include_version ? 1 : 0, applied_software_version);

    http_buffer_t response = {};
    int status_code = 0;
    esp_err_t err = perform_json_post("/api/device/config/ack", bootstrap->device_token, body, &response, &status_code);
    free(body);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "config/ack failed: err=%s status=%d mapped=%s", esp_err_to_name(err), status_code,
                 map_http_failure_result(err, status_code));
    } else {
        ESP_LOGI(TAG, "config/ack sent: result=%s status=%d", result_code, status_code);
    }
    free(response.body);
}

static void worker_task(void *arg)
{
    (void)arg;

    anna_cloud_worker_cmd_t cmd = {};
    for (;;) {
        if (xQueueReceive(s_worker_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        anna_cloud_sync_debug_snapshot_t snapshot = {};
        portENTER_CRITICAL(&s_state_lock);
        capture_debug_snapshot_locked(&snapshot);
        portEXIT_CRITICAL(&s_state_lock);
        snapshot.fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();

        const int64_t worker_started_at_us = esp_timer_get_time();
        dump_debug_trace_history(cmd.type == worker_command_type::ack_only ? "ack_only_start" : "pull_apply_start");
        ESP_LOGI(TAG,
                 "worker command start: type=%u result=%s late_ip_fallback=%d test_harness_forced=%d fabric_count=%u session_ms=%lld attempt_ms=%lld",
                 static_cast<unsigned>(cmd.type), cmd.type == worker_command_type::ack_only ? cmd.result_code : "-",
                 cmd.late_ip_fallback_used ? 1 : 0, cmd.test_harness_forced ? 1 : 0, static_cast<unsigned>(snapshot.fabric_count),
                 static_cast<long long>(elapsed_ms_since(snapshot.session_started_at_us)),
                 static_cast<long long>(elapsed_ms_since(snapshot.sync_attempt_started_at_us)));

        anna_cloud_bootstrap_t bootstrap = {};
        esp_err_t bootstrap_err = anna_cloud_load_bootstrap(&bootstrap);
        char mac_addr[ANNA_CLOUD_MAC_STRING_LEN] = { 0 };
        esp_err_t mac_err = anna_cloud_get_runtime_mac(mac_addr);
        if (mac_err != ESP_OK) {
            strlcpy(mac_addr, "00:00:00:00:00:00", sizeof(mac_addr));
        }

        if (cmd.type == worker_command_type::ack_only) {
            if (bootstrap.has_unique_id && bootstrap.has_device_token) {
                send_ack_if_possible(&bootstrap, mac_addr, cmd.result_code, cmd.late_ip_fallback_used, false, 0);
            } else {
                ESP_LOGW(TAG, "ack-only skipped: bootstrap missing (result=%s)", cmd.result_code);
            }
            finalize_worker_attempt(false, true);
            continue;
        }

        anna_runtime_apply_result_t result = {};
        result.result_code = ANNA_RESULT_CODE_SKIPPED_NO_BOOTSTRAP;
        if (bootstrap_err != ESP_OK || !bootstrap.has_unique_id || !bootstrap.has_device_token) {
            ESP_LOGW(TAG, "cloud sync bootstrap missing: uid=%d token=%d err=%s", bootstrap.has_unique_id ? 1 : 0,
                     bootstrap.has_device_token ? 1 : 0, esp_err_to_name(bootstrap_err));
            mark_terminal_result_locked(result.result_code);
            finalize_worker_attempt(true, true);
            continue;
        }

        char *pull_body = build_pull_request_body(&bootstrap, mac_addr);
        if (!pull_body) {
            result.result_code = ANNA_RESULT_CODE_BAD_FORMAT;
            mark_terminal_result_locked(result.result_code);
            send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, false, 0);
            finalize_worker_attempt(true, true);
            continue;
        }

        http_buffer_t pull_response = {};
        int pull_status = 0;
        const int64_t pull_started_at_us = esp_timer_get_time();
        esp_err_t pull_err = perform_json_post("/api/device/config/pull", bootstrap.device_token, pull_body, &pull_response, &pull_status);
        free(pull_body);
        ESP_LOGI(TAG, "pull completed: status=%d err=%s body_len=%zu elapsed_ms=%lld", pull_status, esp_err_to_name(pull_err),
                 pull_response.len, static_cast<long long>((esp_timer_get_time() - pull_started_at_us) / 1000));

        if (pull_err != ESP_OK || pull_status != 200) {
            result.result_code = map_http_failure_result(pull_err, pull_status);
            mark_terminal_result_locked(result.result_code);
            send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, false, 0);
            free(pull_response.body);
            finalize_worker_attempt(true, true);
            continue;
        }

        char schema_version[kSchemaVersionMaxLen] = { 0 };
        char *candidate_json = nullptr;
        const int64_t validate_started_at_us = esp_timer_get_time();
        if (!validate_pull_envelope(pull_response.body, &candidate_json, schema_version)) {
            result.result_code = ANNA_RESULT_CODE_BAD_FORMAT;
            mark_terminal_result_locked(result.result_code);
            send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, false, 0);
            free(pull_response.body);
            finalize_worker_attempt(true, true);
            continue;
        }
        ESP_LOGI(TAG, "pull envelope validated: schema=%s candidate_len=%zu validate_ms=%lld", schema_version,
                 strlen(candidate_json), static_cast<long long>((esp_timer_get_time() - validate_started_at_us) / 1000));
        free(pull_response.body);

        sync_apply_context_t *apply_ctx = static_cast<sync_apply_context_t *>(calloc(1, sizeof(*apply_ctx)));
        if (!apply_ctx) {
            free(candidate_json);
            result.result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
            mark_terminal_result_locked(result.result_code);
            send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, false, 0);
            finalize_worker_attempt(true, true);
            continue;
        }

        apply_ctx->candidate_raw_json = candidate_json;
        strlcpy(apply_ctx->candidate_schema_version, schema_version, sizeof(apply_ctx->candidate_schema_version));
        apply_ctx->waiter_task = xTaskGetCurrentTaskHandle();

        CHIP_ERROR schedule_err = chip::DeviceLayer::PlatformMgr().ScheduleWork(sync_apply_work, reinterpret_cast<intptr_t>(apply_ctx));
        if (schedule_err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "failed to schedule sync_apply_work: %" CHIP_ERROR_FORMAT, schedule_err.Format());
            free(apply_ctx->candidate_raw_json);
            free(apply_ctx);
            result.result_code = ANNA_RESULT_CODE_REBUILD_FAIL;
            mark_terminal_result_locked(result.result_code);
            send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, false, 0);
            finalize_worker_attempt(true, true);
            continue;
        }

        const int64_t apply_started_at_us = esp_timer_get_time();
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        result = apply_ctx->result;
        mark_terminal_result_locked(result.result_code);
        ESP_LOGI(TAG,
                 "apply completed: result=%s applied_sw=%" PRIu32 " elapsed_ms=%lld diff[topology=%d non_topology_only=%d slot_layout=%d label=%d product_info=%d] worker_total_ms=%lld",
                 result.result_code ? result.result_code : "-", result.applied_software_version,
                 static_cast<long long>((esp_timer_get_time() - apply_started_at_us) / 1000), result.diff.topology_changing ? 1 : 0,
                 result.diff.non_topology_only ? 1 : 0, result.diff.slot_layout_changed ? 1 : 0,
                 result.diff.label_changed ? 1 : 0, result.diff.product_info_changed ? 1 : 0,
                 static_cast<long long>((esp_timer_get_time() - worker_started_at_us) / 1000));
        free(apply_ctx->candidate_raw_json);
        free(apply_ctx);

        bool include_version = strcmp(result.result_code, ANNA_RESULT_CODE_APPLIED) == 0 ||
                strcmp(result.result_code, ANNA_RESULT_CODE_NOOP) == 0;
        send_ack_if_possible(&bootstrap, mac_addr, result.result_code, cmd.late_ip_fallback_used, include_version,
                             result.applied_software_version);
        finalize_worker_attempt(true, true);
    }
}
} // namespace

extern "C" void anna_cloud_sync_init(void)
{
    if (s_worker_queue) {
        return;
    }

    ESP_LOGI(TAG,
             "known event ids: session_started=%u ip_changed=%u commissioning_complete=%u session_stopped=%u "
             "fabric_committed=%u ble_deinitialized=%u",
             static_cast<unsigned>(kCommissioningSessionStarted), static_cast<unsigned>(kInterfaceIpAddressChanged),
             static_cast<unsigned>(kCommissioningComplete), static_cast<unsigned>(kCommissioningSessionStopped),
             static_cast<unsigned>(kFabricCommitted), static_cast<unsigned>(kBLEDeinitialized));

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    arm_cloud_sync_test_harness_if_present();
#endif

    s_worker_queue = xQueueCreate(4, sizeof(anna_cloud_worker_cmd_t));
    if (!s_worker_queue) {
        ESP_LOGE(TAG, "failed to create worker queue");
        return;
    }

    BaseType_t ok = xTaskCreate(worker_task, "anna_cloud_sync", CONFIG_ANNA_CLOUD_SYNC_TASK_STACK_BYTES / sizeof(StackType_t),
                                nullptr, 5, &s_worker_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create worker task");
        vQueueDelete(s_worker_queue);
        s_worker_queue = nullptr;
        s_worker_task = nullptr;
        return;
    }
}

extern "C" void anna_cloud_sync_handle_device_event(const ChipDeviceEvent *event)
{
    if (!event) {
        return;
    }

    if (const char *event_name = device_event_name(event)) {
        ESP_LOGI(TAG, "device event received: %s", event_name);
    }

    anna_cloud_worker_cmd_t pending_cmd = {};
    bool have_pending_cmd = false;
    bool prepared_pull = false;
    bool prepared_ack = false;
    bool ble_deinitialized = false;
    bool needs_terminal_close_out = false;
    const char *default_terminal_result = nullptr;
    bool terminal_late_ip_fallback_used = false;
    const char *event_note = "none";
    const char *pull_skip_reason = nullptr;
    anna_cloud_sync_debug_snapshot_t snapshot = {};
    anna_cloud_sync_test_intent_t consumed_test_intent = ANNA_CLOUD_SYNC_TEST_INTENT_NONE;

    portENTER_CRITICAL(&s_state_lock);
    switch (event->Type) {
    case kInterfaceIpAddressChanged: {
        bool had_ip = s_state.ip_ready;
        bool assigned = event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV4_Assigned ||
                event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV6_Assigned;
        bool lost = event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV4_Lost;
        if (assigned) {
            event_note = had_ip ? "ip_assigned_repeat" : "ip_assigned_first";
            s_state.current_ip_ready = true;
            s_state.ip_ready = true;
            prepared_pull = maybe_prepare_pull_command_locked(true, &pending_cmd, &consumed_test_intent);
            if (!prepared_pull) {
                if (consumed_test_intent == ANNA_CLOUD_SYNC_TEST_INTENT_DEFER_PRIMARY_TO_NEXT_IP) {
                    pull_skip_reason = "test_harness_defer_primary";
                } else {
                    pull_skip_reason = describe_pull_gate_locked();
                }
            }
        } else if (lost) {
            event_note = "ip_lost";
            s_state.current_ip_ready = false;
            s_state.ip_ready = false;
        } else {
            event_note = "ip_other";
        }
        break;
    }
    case kCommissioningSessionStarted:
        event_note = "session_started";
        s_state.session_open = true;
        s_state.ip_ready = s_state.current_ip_ready;
        s_state.fabric_committed_seen = false;
        s_state.commissioning_complete_seen = false;
        s_state.session_stopped_seen = false;
        s_state.ble_deinitialized_seen = false;
        s_state.sync_attempt_started = false;
        s_state.sync_attempt_finished = false;
        s_state.atomic_apply_started = false;
        s_state.terminal_result_locked = false;
        s_state.late_ip_fallback_used = false;
        s_state.primary_condition_observed = false;
        s_state.session_started_at_us = esp_timer_get_time();
        s_state.sync_attempt_started_at_us = 0;
        reset_debug_trace_locked();
        prepared_pull = maybe_prepare_pull_command_locked(false, &pending_cmd, &consumed_test_intent);
        if (!prepared_pull) {
            pull_skip_reason = describe_pull_gate_locked();
        }
        break;
    case kFabricCommitted:
        event_note = "fabric_committed";
        s_state.fabric_committed_seen = true;
        break;
    case kCommissioningComplete:
        event_note = "commissioning_complete";
        s_state.commissioning_complete_seen = true;
        if (!s_state.sync_attempt_started && !s_state.sync_attempt_finished) {
            needs_terminal_close_out = true;
            default_terminal_result = default_close_result_locked();
            terminal_late_ip_fallback_used = s_state.late_ip_fallback_used;
        }
        break;
    case kCommissioningSessionStopped:
        event_note = "session_stopped";
        s_state.session_open = false;
        s_state.session_stopped_seen = true;
        if (!s_state.sync_attempt_started && !s_state.sync_attempt_finished) {
            needs_terminal_close_out = true;
            default_terminal_result = default_close_result_locked();
            terminal_late_ip_fallback_used = s_state.late_ip_fallback_used;
        }
        break;
    case kBLEDeinitialized:
        event_note = "ble_deinitialized";
        s_state.ble_deinitialized_seen = true;
        ble_deinitialized = true;
        if (!s_state.sync_attempt_started && !s_state.sync_attempt_finished) {
            needs_terminal_close_out = true;
            default_terminal_result = default_close_result_locked();
            terminal_late_ip_fallback_used = s_state.late_ip_fallback_used;
        }
        break;
    default:
        event_note = "ignored";
        break;
    }
    have_pending_cmd = prepared_pull || prepared_ack;
    capture_debug_snapshot_locked(&snapshot);
    portEXIT_CRITICAL(&s_state_lock);
    snapshot.fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();

    if (prepared_pull && pending_cmd.test_harness_forced) {
        ESP_LOGI(TAG, "test harness forcing pull/apply on IP-ready despite fabric_count=%u",
                 static_cast<unsigned>(snapshot.fabric_count));
    }

    store_debug_trace(static_cast<uint32_t>(event->Type), device_event_name(event), event_note, pull_skip_reason,
                      prepared_pull, prepared_ack, needs_terminal_close_out, default_terminal_result, &snapshot);
    log_debug_snapshot(static_cast<uint32_t>(event->Type), device_event_name(event), event_note, pull_skip_reason,
                       prepared_pull, prepared_ack, needs_terminal_close_out,
                       default_terminal_result, &snapshot);

    if (needs_terminal_close_out && !have_pending_cmd && default_terminal_result) {
        anna_cloud_bootstrap_t bootstrap = {};
        esp_err_t bootstrap_err = anna_cloud_load_bootstrap(&bootstrap);
        const bool bootstrap_missing = bootstrap_err != ESP_OK || !bootstrap.has_unique_id || !bootstrap.has_device_token;
        const char *terminal_result = bootstrap_missing ? ANNA_RESULT_CODE_SKIPPED_NO_BOOTSTRAP : default_terminal_result;
        ESP_LOGI(TAG,
                 "terminal close-out candidate: default=%s final=%s bootstrap_missing=%d uid=%d token=%d err=%s late_ip_fallback=%d",
                 default_terminal_result, terminal_result, bootstrap_missing ? 1 : 0, bootstrap.has_unique_id ? 1 : 0,
                 bootstrap.has_device_token ? 1 : 0, esp_err_to_name(bootstrap_err), terminal_late_ip_fallback_used ? 1 : 0);

        portENTER_CRITICAL(&s_state_lock);
        prepared_ack = prepare_ack_only_command_locked(terminal_result, terminal_late_ip_fallback_used, &pending_cmd);
        have_pending_cmd = prepared_ack;
        portEXIT_CRITICAL(&s_state_lock);
        if (prepared_ack) {
            ESP_LOGI(TAG, "ack-only prepared: reason=terminal_close_out result=%s late_ip_fallback=%d", terminal_result,
                     terminal_late_ip_fallback_used ? 1 : 0);
        }
    }

    if (have_pending_cmd && !queue_worker_command(&pending_cmd)) {
        portENTER_CRITICAL(&s_state_lock);
        if (prepared_pull) {
            s_state.sync_attempt_started = false;
            s_state.late_ip_fallback_used = false;
            s_state.sync_attempt_started_at_us = 0;
        }
        if (prepared_ack) {
            s_state.sync_attempt_finished = false;
            s_state.terminal_result_locked = false;
        }
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

#if CONFIG_ANNA_CLOUD_SYNC_TEST_HARNESS
    anna_cloud_sync_test_intent_t harness_intent_to_consume = consumed_test_intent;
    if (prepared_pull && pending_cmd.test_harness_forced) {
        harness_intent_to_consume = ANNA_CLOUD_SYNC_TEST_INTENT_FORCE_PULL_ON_NEXT_IP;
    }
    if (harness_intent_to_consume != ANNA_CLOUD_SYNC_TEST_INTENT_NONE) {
        consume_cloud_sync_test_harness_intent(harness_intent_to_consume);
    }
#endif

    if (prepared_pull) {
        ESP_LOGI(TAG, "scheduled cloud sync pull/apply (late_ip_fallback=%d test_harness_forced=%d)",
                 pending_cmd.late_ip_fallback_used ? 1 : 0, pending_cmd.test_harness_forced ? 1 : 0);
    } else if (prepared_ack) {
        ESP_LOGI(TAG, "scheduled cloud sync ack-only result=%s", pending_cmd.result_code);
    } else if (ble_deinitialized) {
        ESP_LOGI(TAG, "safe apply window close recorded without immediate ack-only; worker/apply guard will resolve remaining attempt");
    }
}
