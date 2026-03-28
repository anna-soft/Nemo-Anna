#pragma once

#include <platform/CommissionableDataProvider.h>

/**
 * @brief Anna 프로젝트용 Custom CommissionableDataProvider.
 *
 * - 저장소: NVS partition `runtime_anna`, namespace `anna_cfg`
 * - 키: `pin_code`, `long_dis`, `spake_iter`, `spake_salt`, `spake_verifier`
 *
 * NOTE: esp-matter에서 `CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y`일 때,
 * `esp_matter::set_custom_commissionable_data_provider()`로 등록되면
 * `esp_matter::start()` 내부 `setup_providers()`에서 전역 provider로 적용된다.
 */
class AnnaCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t & setupDiscriminator) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t /*setupDiscriminator*/) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t & iterationCount) override;
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan & saltBuf) override;
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan & verifierBuf, size_t & outVerifierLen) override;

    CHIP_ERROR GetSetupPasscode(uint32_t & setupPasscode) override;
    CHIP_ERROR SetSetupPasscode(uint32_t /*setupPasscode*/) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};


