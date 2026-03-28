#include "anna_device_instance_info_provider.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include <cJSON.h>

#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceError.h>

// These headers already provide proper extern "C" guards where needed.
#include "anna_cfg.h"
#include "anna_state_storage.h"

namespace {
constexpr char TAG[] = "anna_diip";

enum : uint32_t
{
    kLogVendorName      = 1u << 0,
    kLogVendorId        = 1u << 1,
    kLogProductName     = 1u << 2,
    kLogProductId       = 1u << 3,
    kLogProductLabel    = 1u << 4,
    kLogHwVer           = 1u << 5,
    kLogHwVerStr        = 1u << 6,
    kLogSerialNumber    = 1u << 7,
    kLogProductUrl      = 1u << 8,
    kLogPartNumber      = 1u << 9,
    kLogManufactureDate = 1u << 10,
    kLogRotatingUid     = 1u << 11,
};

static void LogOnce(uint32_t bit, const char * fmt, ...)
{
    static uint32_t s_logged_mask = 0;
    if ((s_logged_mask & bit) != 0)
    {
        return;
    }
    s_logged_mask |= bit;

    va_list ap;
    va_start(ap, fmt);
    esp_log_writev(ESP_LOG_INFO, TAG, fmt, ap);
    va_end(ap);
}

CHIP_ERROR CopyStringChecked(const char * src, char * dst, size_t dstSize)
{
    VerifyOrReturnError(dst != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    if (dstSize == 0)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    if (src == nullptr)
    {
        src = "";
    }

    size_t written = strlcpy(dst, src, dstSize);
    VerifyOrReturnError(written < dstSize, CHIP_ERROR_BUFFER_TOO_SMALL);
    return CHIP_NO_ERROR;
}

const char * NonEmptyOr(const char * s, const char * fallback)
{
    if (s && s[0] != '\0')
    {
        return s;
    }
    return fallback ? fallback : "";
}
} // namespace

CHIP_ERROR AnnaDeviceInstanceInfoProvider::EnsureUnitInfoLoaded()
{
    if (mUnitLoaded)
    {
        return CHIP_NO_ERROR;
    }

    mCachedSerial[0]   = '\0';
    mCachedUniqueId[0] = '\0';

    if (!anna_state_has_unit_blob())
    {
        mUnitLoaded = true;
        return CHIP_NO_ERROR;
    }

    size_t len = 0;
    int rc     = anna_state_get_unit_blob(nullptr, &len);
    if (rc != ESP_OK || len == 0)
    {
        mUnitLoaded = true;
        return CHIP_NO_ERROR;
    }

    // +1 for NUL termination; stored blob is expected to be UTF-8 JSON bytes
    char * raw = static_cast<char *>(malloc(len + 1));
    if (!raw)
    {
        mUnitLoaded = true;
        return CHIP_ERROR_NO_MEMORY;
    }

    size_t inout = len;
    rc           = anna_state_get_unit_blob(raw, &inout);
    if (rc != ESP_OK || inout == 0)
    {
        free(raw);
        mUnitLoaded = true;
        return CHIP_NO_ERROR;
    }
    raw[inout] = '\0';

    cJSON * root = cJSON_Parse(raw);
    if (!root || !cJSON_IsObject(root))
    {
        if (root)
        {
            cJSON_Delete(root);
        }
        free(raw);
        mUnitLoaded = true;
        return CHIP_NO_ERROR;
    }

    const cJSON * j_ui = cJSON_GetObjectItemCaseSensitive(root, "UnitInfo");
    if (cJSON_IsObject(j_ui))
    {
        const cJSON * j_sn  = cJSON_GetObjectItemCaseSensitive(j_ui, "SerialNumber");
        const cJSON * j_uid = cJSON_GetObjectItemCaseSensitive(j_ui, "UniqueID");

        if (cJSON_IsString(j_sn) && j_sn->valuestring)
        {
            // Cache; truncation is OK (best-effort). Provider APIs will buffer-check on copy-out.
            strlcpy(mCachedSerial, j_sn->valuestring, sizeof(mCachedSerial));
        }
        if (cJSON_IsString(j_uid) && j_uid->valuestring)
        {
            strlcpy(mCachedUniqueId, j_uid->valuestring, sizeof(mCachedUniqueId));
        }
    }

    cJSON_Delete(root);
    free(raw);
    mUnitLoaded = true;

    if (mCachedSerial[0] != '\0')
    {
        ESP_LOGI(TAG, "UnitInfo loaded: SerialNumber=%s", mCachedSerial);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetVendorName(char * buf, size_t bufSize)
{
    // VendorName is effectively required for BasicInformation cluster reads.
    const char * v = NonEmptyOr(g_anna_cfg.product_info.vendor_name, "UnknownVendor");
    LogOnce(kLogVendorName, "DeviceInstanceInfoProvider: VendorName='%s'\n", v);
    return CopyStringChecked(v, buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetVendorId(uint16_t & vendorId)
{
    vendorId = g_anna_cfg.product_info.vendor_id;
    LogOnce(kLogVendorId, "DeviceInstanceInfoProvider: VendorID=0x%04X (%u)\n", vendorId, vendorId);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetProductName(char * buf, size_t bufSize)
{
    const char * p = NonEmptyOr(g_anna_cfg.product_info.product_name, "UnknownProduct");
    LogOnce(kLogProductName, "DeviceInstanceInfoProvider: ProductName='%s'\n", p);
    return CopyStringChecked(p, buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetProductId(uint16_t & productId)
{
    productId = g_anna_cfg.product_info.product_id;
    LogOnce(kLogProductId, "DeviceInstanceInfoProvider: ProductID=0x%04X (%u)\n", productId, productId);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetPartNumber(char * buf, size_t bufSize)
{
    // ProductInfo does not define part number. Return empty (success) to keep attribute reads resilient.
    LogOnce(kLogPartNumber, "DeviceInstanceInfoProvider: PartNumber=''\n");
    return CopyStringChecked("", buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetProductURL(char * buf, size_t bufSize)
{
    // ProductInfo does not define product URL. Return empty (success).
    LogOnce(kLogProductUrl, "DeviceInstanceInfoProvider: ProductURL=''\n");
    return CopyStringChecked("", buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetProductLabel(char * buf, size_t bufSize)
{
    // ProductLabel is optional (may be empty string).
    const char * l = NonEmptyOr(g_anna_cfg.product_info.product_label, "");
    LogOnce(kLogProductLabel, "DeviceInstanceInfoProvider: ProductLabel='%s'\n", l);
    return CopyStringChecked(l, buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetSerialNumber(char * buf, size_t bufSize)
{
    // Prefer UnitInfo.SerialNumber (if provisioned). If missing, return empty string (no fallback).
    (void) EnsureUnitInfoLoaded();

    if (mCachedSerial[0] != '\0')
    {
        LogOnce(kLogSerialNumber, "DeviceInstanceInfoProvider: SerialNumber(source=unit-info)='%s'\n", mCachedSerial);
        return CopyStringChecked(mCachedSerial, buf, bufSize);
    }

    LogOnce(kLogSerialNumber, "DeviceInstanceInfoProvider: SerialNumber(source=empty)=''\n");
    return CopyStringChecked("", buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day)
{
    // Not provided by ProductInfo/UnitInfo currently. Let upper layers apply their own defaults.
    (void) year;
    (void) month;
    (void) day;
    LogOnce(kLogManufactureDate, "DeviceInstanceInfoProvider: ManufacturingDate=(not set)\n");
    return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetHardwareVersion(uint16_t & hardwareVersion)
{
    uint16_t hv = g_anna_cfg.product_info.hw_ver;
    if (hv == 0)
    {
        hv = static_cast<uint16_t>(CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION);
    }
    hardwareVersion = hv;
    LogOnce(kLogHwVer, "DeviceInstanceInfoProvider: HardwareVersion=%u\n", static_cast<unsigned>(hardwareVersion));
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetHardwareVersionString(char * buf, size_t bufSize)
{
    if (g_anna_cfg.product_info.hw_ver_str[0] != '\0')
    {
        LogOnce(kLogHwVerStr, "DeviceInstanceInfoProvider: HardwareVersionString='%s'\n", g_anna_cfg.product_info.hw_ver_str);
        return CopyStringChecked(g_anna_cfg.product_info.hw_ver_str, buf, bufSize);
    }

    // Fallback: stringify hw_ver
    char tmp[8] = { 0 };
    snprintf(tmp, sizeof(tmp), "%u", static_cast<unsigned>(g_anna_cfg.product_info.hw_ver));
    LogOnce(kLogHwVerStr, "DeviceInstanceInfoProvider: HardwareVersionString(fallback)='%s'\n", tmp);
    return CopyStringChecked(tmp, buf, bufSize);
}

CHIP_ERROR AnnaDeviceInstanceInfoProvider::GetRotatingDeviceIdUniqueId(chip::MutableByteSpan & uniqueIdSpan)
{
    // Not used unless CHIP_ENABLE_ROTATING_DEVICE_ID is enabled. Keep behavior explicit.
    (void) uniqueIdSpan;
    LogOnce(kLogRotatingUid, "DeviceInstanceInfoProvider: RotatingDeviceIdUniqueId=(not implemented)\n");
    return CHIP_ERROR_NOT_IMPLEMENTED;
}


