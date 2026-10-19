#pragma once

#include <cstdint>
#include <optional>

namespace peripheral {

enum class DeviceFeature {
    Battery,
    Charging,
    Brightness,
    Contrast,
    Volume,
    Mute,
    InputSource,
    Scene,
    ColorPreset,
    Dpi,
    KeyMapping,
    Rgb,
    Equalizer
};

// 仅映射当前已实现的显示器功能；能否写入仍由后端实时检测。
constexpr std::optional<std::uint8_t> monitorVcpCode(DeviceFeature feature) {
    switch (feature) {
    case DeviceFeature::Contrast:
        return 0x12;
    case DeviceFeature::InputSource:
        return 0x60;
    case DeviceFeature::Scene:
        return 0xDC;
    case DeviceFeature::ColorPreset:
        return 0x14;
    default:
        return std::nullopt;
    }
}

constexpr std::optional<DeviceFeature> featureForMonitorVcp(std::uint8_t code) {
    switch (code) {
    case 0x12:
        return DeviceFeature::Contrast;
    case 0x60:
        return DeviceFeature::InputSource;
    case 0xDC:
        return DeviceFeature::Scene;
    case 0x14:
        return DeviceFeature::ColorPreset;
    default:
        return std::nullopt;
    }
}

} // namespace peripheral
