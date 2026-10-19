#pragma once

#include "platform/WindowsBackend.h"

#include <map>
#include <string>

namespace peripheral::app {

struct RestoreResult {
    bool success = false;
    std::string message;
};

// 在后台串行调用。白名单仅为 brightness、volume、vcp_dc；不切换输入源。
// 调用者只传入历史已读回确认的保存值；本函数再次验证当前设备能力和状态。
// last_input 仅为历史输入源上下文：不同或不可确认时跳过整台显示器，不写输入源。
// 写入后等待 250 ms 并读取一次，不重试，也不因部分失败自动回滚。
RestoreResult restoreDeviceSettings(const platform::WindowsBackend& backend, const std::wstring& targetId,
                                    bool monitor, const std::map<std::string, int>& values);

} // namespace peripheral::app
