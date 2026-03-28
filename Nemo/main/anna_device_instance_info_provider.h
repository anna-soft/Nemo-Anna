#pragma once

#include <platform/DeviceInstanceInfoProvider.h>

/**
 * @brief Anna 프로젝트용 Custom DeviceInstanceInfoProvider.
 *
 * - ProductInfo 값: `components/anna_cfg/include/anna_cfg.h`의 `g_anna_cfg.product_info`에서 제공
 *   (Host가 `product-info.json`을 전송하면 `main/host_serial_rx.cpp`에서 `anna_cfg_parse_json()`로 파싱됨)
 * - SerialNumber 값: `unit-info.json`이 제공하는 UnitInfo.SerialNumber를 best-effort로 사용
 *   (저장소: `components/anna_cfg/anna_state_storage.c`의 unit_blob)
 *   - 값이 없으면 빈 문자열("")을 반환한다. (MAC 등 다른 값으로 대체하지 않음)
 *
 * NOTE:
 * - 이 Provider는 `CONFIG_CUSTOM_DEVICE_INSTANCE_INFO_PROVIDER=y`일 때
 *   `esp_matter::set_custom_device_instance_info_provider()`로 등록되어야 하며,
 *   `esp_matter::start()` 내부 `setup_providers()`에서 전역 provider로 적용된다.
 */
class AnnaDeviceInstanceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider
{
public:
    CHIP_ERROR GetVendorName(char * buf, size_t bufSize) override;
    CHIP_ERROR GetVendorId(uint16_t & vendorId) override;
    CHIP_ERROR GetProductName(char * buf, size_t bufSize) override;
    CHIP_ERROR GetProductId(uint16_t & productId) override;
    CHIP_ERROR GetPartNumber(char * buf, size_t bufSize) override;
    CHIP_ERROR GetProductURL(char * buf, size_t bufSize) override;
    CHIP_ERROR GetProductLabel(char * buf, size_t bufSize) override;
    CHIP_ERROR GetSerialNumber(char * buf, size_t bufSize) override;
    CHIP_ERROR GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day) override;
    CHIP_ERROR GetHardwareVersion(uint16_t & hardwareVersion) override;
    CHIP_ERROR GetHardwareVersionString(char * buf, size_t bufSize) override;
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan & uniqueIdSpan) override;

private:
    static constexpr size_t kMaxCachedSerialLen  = 32;
    static constexpr size_t kMaxCachedUniqueIdLen = 32;

    CHIP_ERROR EnsureUnitInfoLoaded();
    bool mUnitLoaded = false;
    char mCachedSerial[kMaxCachedSerialLen + 1]   = { 0 };
    char mCachedUniqueId[kMaxCachedUniqueIdLen + 1] = { 0 };
};


