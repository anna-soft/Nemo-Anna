#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <freertos/event_groups.h>

/**
 * @brief UART0(콘솔) 라인(JSON 1개 + '\n') 수신 태스크를 시작한다.
 *
 * - guideline.md의 "D. UART 라인 수신(USB 프로비저닝 전용)" 단계 구현용
 * - 현재 단계에서는 FILE_SEND의 raw_b64 기반 CRC32C/length 검증 후 file_ack 응답까지 담당
 */
void anna_host_serial_rx_start(void);

/**
 * @brief app_main이 프로비저닝 완료/유닛 처리 완료를 기다릴 수 있도록 이벤트 그룹을 등록한다.
 *
 * - productOkBit: product-info 저장 성공(+ACK) 시 set
 * - unitDoneBit: unit-info 처리(성공/실패 무관하게 "처리 시도 완료") 시 set
 */
void anna_host_serial_rx_set_event_group(EventGroupHandle_t eg, EventBits_t productOkBit,
                                        EventBits_t unitDoneBit);

#ifdef __cplusplus
}
#endif


