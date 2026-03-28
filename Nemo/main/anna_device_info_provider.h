#pragma once

#include <sdkconfig.h>

// Custom DeviceInfoProvider for Anna.
//
// Goal (current): keep UserLabel behavior identical to the ESP32 default provider
// even when CONFIG_CUSTOM_DEVICE_INFO_PROVIDER=y.
//
// Future: extend/override FixedLabel / SupportedLocales / SupportedCalendarTypes once
// the server JSON schema is finalized.

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
#include <platform/ESP32/ESP32DeviceInfoProvider.h>
#include <lib/support/CHIPMem.h>
#include <string.h>

#include "anna_cfg.h"

/**
 * @brief Anna 프로젝트용 Custom DeviceInfoProvider.
 *
 * 현재는 UserLabel 기능이 깨지지 않도록 ESP32 기본 구현(ESP32DeviceInfoProvider)을 그대로 사용한다.
 * (UserLabel은 `main/app_settings.cpp`에서 GetDeviceInfoProvider()->SetUserLabelList()로 저장/갱신됨)
 */
class AnnaDeviceInfoProvider : public chip::DeviceLayer::ESP32DeviceInfoProvider
{
public:
    AnnaDeviceInfoProvider() = default;
    ~AnnaDeviceInfoProvider() override = default;

    /**
     * @brief FixedLabel cluster read path 지원.
     *
     * Matter의 FixedLabel 서버 구현(`connectedhomeip/src/app/clusters/fixed-label-server/fixed-label-server.cpp`)
     * 은 `DeviceLayer::GetDeviceInfoProvider()->IterateFixedLabel(endpoint)`를 호출해
     * LabelList를 인코딩한다. (SetFixedLabelList 같은 setter API는 존재하지 않음)
     *
     * Anna는 Host가 전달한 `product-info.json`의 각 항목(Button/Switch/ConButton/ConSwitch)의
     * `FixedLabel` 값을 `g_anna_cfg`에 저장해두므로, 여기서는 endpoint 기준으로 “첫 번째 wins” 정책으로
     * FixedLabel 리스트를 제공한다.
     */
    FixedLabelIterator * IterateFixedLabel(chip::EndpointId endpoint) override
    {
        return chip::Platform::New<AnnaFixedLabelIterator>(endpoint);
    }

private:
    class AnnaFixedLabelIterator : public FixedLabelIterator
    {
    public:
        explicit AnnaFixedLabelIterator(chip::EndpointId endpoint) : mEndpoint(endpoint)
        {
            mIndex = 0;
            mCount = 0;
            mKvs = nullptr;

            // Deterministic selection order: Button → Switch → ConButton → ConSwitch
            for (int i = 0; i < g_anna_cfg.button_cnt; ++i) {
                if (g_anna_cfg.a_button[i].base.endpoint_id == mEndpoint && g_anna_cfg.a_button[i].fixed_label_cnt > 0) {
                    mKvs = g_anna_cfg.a_button[i].fixed_label;
                    mCount = g_anna_cfg.a_button[i].fixed_label_cnt;
                    return;
                }
            }
            for (int i = 0; i < g_anna_cfg.switch_cnt; ++i) {
                if (g_anna_cfg.a_switch[i].base.endpoint_id == mEndpoint && g_anna_cfg.a_switch[i].fixed_label_cnt > 0) {
                    mKvs = g_anna_cfg.a_switch[i].fixed_label;
                    mCount = g_anna_cfg.a_switch[i].fixed_label_cnt;
                    return;
                }
            }
            for (int i = 0; i < g_anna_cfg.con_btn_cnt; ++i) {
                if (g_anna_cfg.con_btn[i].base.endpoint_id == mEndpoint && g_anna_cfg.con_btn[i].fixed_label_cnt > 0) {
                    mKvs = g_anna_cfg.con_btn[i].fixed_label;
                    mCount = g_anna_cfg.con_btn[i].fixed_label_cnt;
                    return;
                }
            }
            for (int i = 0; i < g_anna_cfg.con_swt_cnt; ++i) {
                if (g_anna_cfg.con_swt[i].base.endpoint_id == mEndpoint && g_anna_cfg.con_swt[i].fixed_label_cnt > 0) {
                    mKvs = g_anna_cfg.con_swt[i].fixed_label;
                    mCount = g_anna_cfg.con_swt[i].fixed_label_cnt;
                    return;
                }
            }
        }

        size_t Count() override { return (size_t)mCount; }

        bool Next(FixedLabelType & output) override
        {
            if (!mKvs || mIndex >= mCount) {
                return false;
            }

            // Copy to local buffers because output uses CharSpan referencing memory.
            memset(mLabelBuf, 0, sizeof(mLabelBuf));
            memset(mValueBuf, 0, sizeof(mValueBuf));

            // `anna_cfg_parse.c`에서 이미 소문자/공백정규화/길이(<=16) 보장
            strlcpy(mLabelBuf, mKvs[mIndex].label, sizeof(mLabelBuf));
            strlcpy(mValueBuf, mKvs[mIndex].value, sizeof(mValueBuf));

            output.label = chip::CharSpan::fromCharString(mLabelBuf);
            output.value = chip::CharSpan::fromCharString(mValueBuf);

            mIndex++;
            return true;
        }

        void Release() override { chip::Platform::Delete(this); }

    private:
        chip::EndpointId mEndpoint = 0;
        uint8_t mIndex = 0;
        uint8_t mCount = 0;
        const anna_fixed_label_kv_t * mKvs = nullptr;

        // DeviceInfoProvider contract: max 16 chars (+NUL)
        char mLabelBuf[chip::DeviceLayer::kMaxLabelNameLength + 1] = {0};
        char mValueBuf[chip::DeviceLayer::kMaxLabelValueLength + 1] = {0};
    };
};

#else
#error "AnnaDeviceInfoProvider requires CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER=y"
#endif


