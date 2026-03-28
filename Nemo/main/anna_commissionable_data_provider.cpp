#include "anna_commissionable_data_provider.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>

namespace {
constexpr char TAG[] = "anna_cdp";

constexpr char ANNA_RT_PARTITION_NAME[] = "runtime_anna";
constexpr char ANNA_CFG_NAMESPACE[]     = "anna_cfg";

constexpr char ANNA_KEY_PIN[]           = "pin_code";
constexpr char ANNA_KEY_LONG_DIS[]      = "long_dis";
constexpr char ANNA_KEY_SPAKE_ITER[]    = "spake_iter";
constexpr char ANNA_KEY_SPAKE_SALT[]    = "spake_salt";
constexpr char ANNA_KEY_SPAKE_VERIFIER[]= "spake_verifier";

static CHIP_ERROR ChipErrorFromEspErr(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return CHIP_NO_ERROR;
    case ESP_ERR_NVS_NOT_FOUND:
        return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    case ESP_ERR_NVS_INVALID_LENGTH:
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    default:
        return CHIP_ERROR_INTERNAL;
    }
}

static CHIP_ERROR OpenAnnaCfgNvsReadOnly(nvs_handle_t & outHandle)
{
    esp_err_t err = nvs_open_from_partition(ANNA_RT_PARTITION_NAME, ANNA_CFG_NAMESPACE, NVS_READONLY, &outHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition(%s,%s) failed: %s", ANNA_RT_PARTITION_NAME, ANNA_CFG_NAMESPACE,
                 esp_err_to_name(err));
        return ChipErrorFromEspErr(err);
    }
    return CHIP_NO_ERROR;
}
} // namespace

CHIP_ERROR AnnaCommissionableDataProvider::GetSetupDiscriminator(uint16_t & setupDiscriminator)
{
    nvs_handle_t h;
    ReturnErrorOnFailure(OpenAnnaCfgNvsReadOnly(h));

    uint16_t dis = 0;
    esp_err_t err = nvs_get_u16(h, ANNA_KEY_LONG_DIS, &dis);
    nvs_close(h);
    ReturnErrorOnFailure(ChipErrorFromEspErr(err));

    VerifyOrReturnError(dis <= chip::kMaxDiscriminatorValue, CHIP_ERROR_INVALID_ARGUMENT);
    setupDiscriminator = dis;
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaCommissionableDataProvider::GetSetupPasscode(uint32_t & setupPasscode)
{
    nvs_handle_t h;
    ReturnErrorOnFailure(OpenAnnaCfgNvsReadOnly(h));

    uint32_t pin = 0;
    esp_err_t err = nvs_get_u32(h, ANNA_KEY_PIN, &pin);
    nvs_close(h);
    ReturnErrorOnFailure(ChipErrorFromEspErr(err));

    // 추가 검증은 SetupPayload 쪽에서 하므로 여기서는 범위만 방어적으로 체크
    VerifyOrReturnError(pin >= chip::kMinSetupPasscode && pin <= chip::kMaxSetupPasscode, CHIP_ERROR_INVALID_ARGUMENT);
    setupPasscode = pin;
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaCommissionableDataProvider::GetSpake2pIterationCount(uint32_t & iterationCount)
{
    nvs_handle_t h;
    ReturnErrorOnFailure(OpenAnnaCfgNvsReadOnly(h));

    uint32_t iter = 0;
    esp_err_t err = nvs_get_u32(h, ANNA_KEY_SPAKE_ITER, &iter);
    nvs_close(h);
    ReturnErrorOnFailure(ChipErrorFromEspErr(err));

    VerifyOrReturnError(iter >= chip::Crypto::kSpake2p_Min_PBKDF_Iterations && iter <= chip::Crypto::kSpake2p_Max_PBKDF_Iterations,
                        CHIP_ERROR_INVALID_ARGUMENT);
    iterationCount = iter;
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaCommissionableDataProvider::GetSpake2pSalt(chip::MutableByteSpan & saltBuf)
{
    nvs_handle_t h;
    ReturnErrorOnFailure(OpenAnnaCfgNvsReadOnly(h));

    size_t required = 0;
    esp_err_t err = nvs_get_blob(h, ANNA_KEY_SPAKE_SALT, nullptr, &required);
    if (err != ESP_OK) {
        nvs_close(h);
        return ChipErrorFromEspErr(err);
    }

    VerifyOrReturnError(required >= chip::Crypto::kSpake2p_Min_PBKDF_Salt_Length &&
                            required <= chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length,
                        CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(required <= saltBuf.size(), CHIP_ERROR_BUFFER_TOO_SMALL);

    size_t readLen = required;
    err = nvs_get_blob(h, ANNA_KEY_SPAKE_SALT, saltBuf.data(), &readLen);
    nvs_close(h);
    ReturnErrorOnFailure(ChipErrorFromEspErr(err));

    VerifyOrReturnError(readLen == required, CHIP_ERROR_INTERNAL);
    saltBuf.reduce_size(readLen);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AnnaCommissionableDataProvider::GetSpake2pVerifier(chip::MutableByteSpan & verifierBuf, size_t & outVerifierLen)
{
    nvs_handle_t h;
    ReturnErrorOnFailure(OpenAnnaCfgNvsReadOnly(h));

    size_t required = 0;
    esp_err_t err = nvs_get_blob(h, ANNA_KEY_SPAKE_VERIFIER, nullptr, &required);
    if (err != ESP_OK) {
        nvs_close(h);
        return ChipErrorFromEspErr(err);
    }

    outVerifierLen = required;
    VerifyOrReturnError(required == chip::Crypto::kSpake2p_VerifierSerialized_Length, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(required <= verifierBuf.size(), CHIP_ERROR_BUFFER_TOO_SMALL);

    size_t readLen = required;
    err = nvs_get_blob(h, ANNA_KEY_SPAKE_VERIFIER, verifierBuf.data(), &readLen);
    nvs_close(h);
    ReturnErrorOnFailure(ChipErrorFromEspErr(err));

    VerifyOrReturnError(readLen == required, CHIP_ERROR_INTERNAL);
    verifierBuf.reduce_size(readLen);
    outVerifierLen = readLen;
    return CHIP_NO_ERROR;
}


