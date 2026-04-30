#pragma once

#include <platform/CHIPDeviceEvent.h>

#ifdef __cplusplus
extern "C" {
#endif

void anna_cloud_sync_init(void);
void anna_cloud_sync_handle_device_event(const chip::DeviceLayer::ChipDeviceEvent *event);

#ifdef __cplusplus
}
#endif
