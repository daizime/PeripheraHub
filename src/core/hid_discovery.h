#pragma once

#include "device_model.h"
#include "platform/WindowsBackend.h"

#include <vector>

namespace peripheral {

// 只根据 HID 顶层集合用途和已验证的专用接口分类，不向设备发送报文。
// 同一物理设备的集合保留各自路径；DeviceRegistry 依据 ContainerId 与 kind 合并。
std::vector<Endpoint> discoverHidEndpoints(const std::vector<platform::HidDeviceInfo>& devices);

} // namespace peripheral
