#include "SettingsRestore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <thread>
#include <vector>

namespace peripheral::app {
namespace {

struct Report {
    bool success = true;
    std::vector<std::string> messages;

    void fail(const std::string& message) {
        success = false;
        messages.push_back(message);
    }
    RestoreResult result() const {
        std::string text;
        for (const auto& message : messages) {
            if (!text.empty())
                text += "；";
            text += message;
        }
        return {success, text.empty() ? "没有可恢复的受支持设置" : text};
    }
};

std::optional<int> requested(const std::map<std::string, int>& values, const char* key, int maximum,
                             const char* title, Report& report) {
    const auto found = values.find(key);
    if (found == values.end())
        return std::nullopt;
    if (found->second < 0 || found->second > maximum) {
        report.fail(std::string(title) + "未恢复：保存值超出允许范围");
        return std::nullopt;
    }
    return found->second;
}

template <typename T> const T* uniqueTarget(const std::vector<T>& devices, const std::wstring& id) {
    const T* result = nullptr;
    for (const auto& device : devices) {
        if (device.id != id)
            continue;
        if (result)
            return nullptr; // 同 ID 多条记录也不能猜测使用哪一条。
        result = &device;
    }
    return result;
}

bool readableBrightness(const platform::MonitorInfo& device) {
    return device.brightnessReadable && device.brightnessPercent >= 0 && device.brightnessPercent <= 100;
}

bool readableVolume(const platform::AudioEndpointInfo& device) {
    return device.volumeReadable && std::isfinite(device.volumePercent) && device.volumePercent >= 0 &&
           device.volumePercent <= 100;
}

std::optional<bool> matchingInput(const platform::MonitorControlsInfo& controls, const std::wstring& id,
                                  int saved, Report& report) {
    const platform::MonitorFeatureInfo* input = nullptr;
    if (controls.id == id) {
        for (const auto& feature : controls.features) {
            if (feature.code != 0x60)
                continue;
            if (input) {
                report.fail("未恢复：输入源记录不唯一，无法确认当前输入源");
                return std::nullopt;
            }
            input = &feature;
        }
    }
    const auto allowed = [&](std::uint32_t value) {
        return input && std::find(input->allowedValues.begin(), input->allowedValues.end(), value) !=
                            input->allowedValues.end();
    };
    if (!input || !input->advertised || !input->readable || !allowed(input->current) ||
        !allowed(static_cast<std::uint32_t>(saved))) {
        report.fail("未恢复：当前或历史输入源无法由显示器声明确认，沿用当前设置");
        return std::nullopt;
    }
    return input->current == static_cast<std::uint32_t>(saved);
}

const platform::MonitorFeatureInfo* confirmedScenario(const platform::MonitorControlsInfo& controls,
                                                      const std::wstring& id, int value, Report& report) {
    const platform::MonitorFeatureInfo* scenario = nullptr;
    if (controls.id == id) {
        for (const auto& feature : controls.features) {
            if (feature.code != 0xDC)
                continue;
            if (scenario) {
                report.fail("情景未恢复：显示器返回了重复的功能记录");
                return nullptr;
            }
            scenario = &feature;
        }
    }
    if (!scenario || !scenario->advertised || !scenario->readable || !scenario->writable) {
        report.fail("情景未恢复：当前显示器未提供可读写的情景功能");
        return nullptr;
    }
    const auto allowed = [&](std::uint32_t item) {
        return std::find(scenario->allowedValues.begin(), scenario->allowedValues.end(), item) !=
               scenario->allowedValues.end();
    };
    if (!allowed(scenario->current)) {
        report.fail("情景未恢复：当前回报值不在显示器声明列表中");
        return nullptr;
    }
    if (!allowed(static_cast<std::uint32_t>(value))) {
        report.fail("情景未恢复：保存值不在当前显示器声明列表中");
        return nullptr;
    }
    return scenario;
}

template <typename Action> void attempt(Action&& action, const char* title, bool& attempted, Report& report) {
    attempted = true;
    try {
        action();
    } catch (const std::exception& error) {
        report.fail(std::string(title) + "写入失败：" + error.what());
    }
}

RestoreResult restoreMonitor(const platform::WindowsBackend& backend, const std::wstring& id,
                             const std::map<std::string, int>& values) {
    Report report;
    const auto brightness = requested(values, "brightness", 100, "亮度", report);
    const auto scenario = requested(values, "vcp_dc", 65535, "情景", report);
    const auto lastInput = requested(values, "last_input", 65535, "输入源上下文", report);
    if (values.contains("last_input") && !lastInput)
        return report.result();
    if (!brightness && !scenario && !lastInput)
        return report.result();
    const auto monitors = backend.enumerateMonitors();
    const auto* monitor = uniqueTarget(monitors, id);
    if (id.empty() || !monitor) {
        report.fail("未恢复：目标显示器已移除或接口身份不唯一");
        return report.result();
    }

    std::optional<platform::MonitorControlsInfo> controls;
    if (lastInput) {
        try {
            controls = backend.queryMonitorControls(id);
        } catch (const std::exception& error) {
            report.fail(std::string("未恢复：无法确认当前输入源，") + error.what());
            return report.result();
        }
        const auto sameInput = matchingInput(*controls, id, *lastInput, report);
        if (!sameInput)
            return report.result();
        if (!*sameInput)
            return {true, "输入源已变化，沿用当前设置"};
    }
    const bool brightnessReady = brightness && readableBrightness(*monitor);
    if (brightness && !brightnessReady)
        report.fail("亮度未恢复：当前链路无法读取有效亮度");
    const platform::MonitorFeatureInfo* scenarioFeature = nullptr;
    if (scenario) {
        try {
            if (!controls)
                controls = backend.queryMonitorControls(id);
            scenarioFeature = confirmedScenario(*controls, id, *scenario, report);
        } catch (const std::exception& error) {
            report.fail(std::string("情景未恢复：") + error.what());
        }
    }

    bool attempted = false;
    if (brightnessReady && monitor->brightnessPercent != *brightness)
        attempt([&] { backend.setMonitorBrightness(id, *brightness); }, "亮度", attempted, report);
    if (scenarioFeature && scenarioFeature->current != static_cast<std::uint32_t>(*scenario)) {
        attempt([&] { backend.setMonitorFeature(id, 0xDC, static_cast<std::uint16_t>(*scenario)); }, "情景",
                attempted, report);
    }
    if (!attempted) {
        if (brightnessReady)
            report.messages.push_back("亮度已是保存值");
        if (scenarioFeature)
            report.messages.push_back("情景已是保存值");
        return report.result();
    }

    // 多项写入共用一次等待；读取不能触发再次写入或自动回滚。
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (brightnessReady) {
        try {
            const auto readBack = backend.enumerateMonitors();
            const auto* actual = uniqueTarget(readBack, id);
            if (actual && readableBrightness(*actual) &&
                std::abs(actual->brightnessPercent - *brightness) <= 1)
                report.messages.push_back("亮度已恢复并确认");
            else
                report.fail("亮度恢复未确认：目标不可读或回报值与保存值不符");
        } catch (const std::exception& error) {
            report.fail(std::string("亮度恢复读回失败：") + error.what());
        }
    }
    if (scenarioFeature) {
        try {
            const auto readBack = backend.queryMonitorControls(id);
            const auto* actual = confirmedScenario(readBack, id, *scenario, report);
            if (actual && actual->current == static_cast<std::uint32_t>(*scenario))
                report.messages.push_back("情景已恢复并确认");
            else if (actual)
                report.fail("情景恢复未确认：回报值与保存值不符");
        } catch (const std::exception& error) {
            report.fail(std::string("情景恢复读回失败：") + error.what());
        }
    }
    return report.result();
}

RestoreResult restoreAudio(const platform::WindowsBackend& backend, const std::wstring& id,
                           const std::map<std::string, int>& values) {
    Report report;
    const auto volume = requested(values, "volume", 100, "音量", report);
    if (!volume)
        return report.result();
    // 后端只枚举 DEVICE_STATE_ACTIVE；这里仍需按精确 ID 唯一匹配并验证读数。
    const auto endpoints = backend.enumerateAudioEndpoints();
    const auto* endpoint = uniqueTarget(endpoints, id);
    if (id.empty() || !endpoint || !readableVolume(*endpoint)) {
        report.fail("音量未恢复：目标端点已移除、身份不唯一或不可读");
        return report.result();
    }
    // 档案与界面都是整数百分比；系统小数仍显示同一值时不做无意义写入。
    if (std::lround(endpoint->volumePercent) == *volume) {
        report.messages.push_back("音量已是保存值");
        return report.result();
    }
    bool attempted = false;
    attempt([&] { backend.setAudioVolume(id, static_cast<float>(*volume)); }, "音量", attempted, report);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    try {
        const auto readBack = backend.enumerateAudioEndpoints();
        const auto* actual = uniqueTarget(readBack, id);
        if (actual && readableVolume(*actual) && std::abs(actual->volumePercent - *volume) <= 1)
            report.messages.push_back("音量已恢复并确认");
        else
            report.fail("音量恢复未确认：目标不可读或回报值与保存值不符");
    } catch (const std::exception& error) {
        report.fail(std::string("音量恢复读回失败：") + error.what());
    }
    return report.result();
}

} // namespace

RestoreResult restoreDeviceSettings(const platform::WindowsBackend& backend, const std::wstring& targetId,
                                    bool monitor, const std::map<std::string, int>& values) {
    try {
        return monitor ? restoreMonitor(backend, targetId, values) : restoreAudio(backend, targetId, values);
    } catch (const std::exception& error) {
        return {false, std::string("恢复失败：") + error.what()};
    }
}

} // namespace peripheral::app
